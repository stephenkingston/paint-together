/*
 * Paint (C port) - the immediate-mode widget kit: frames, input, ids, layout,
 * layers (Rust: what egui does for app.rs)
 *
 * All state lives in fixed arrays in one struct; the only allocations are
 * the overlay surfaces, made when a popup or dialog first opens (and grown
 * if it gets bigger) and freed when it closes.
 *
 * Input is summed up once per frame in kit_begin: where the pointer is, the
 * first press and the last release, keys and text. A press belongs to the
 * topmost layer under it in the previous frame's stacking (a modal dialog
 * swallows presses outside it), and the first widget in that layer whose
 * rectangle holds it becomes the "active" one until the button goes up.
 * Widgets are also kit_widgets.c (the general ones) and ribbon.c.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "os/os.h"
#include "ui/kit_internal.h"

kit_state K;

/* ---- the frame ---- */

void kit_init(void)
{
    memset(&K, 0, sizeof K);
    K.repaint_at = 0;
    K.cursor = CURSOR_ARROW;
}

void kit_event(const ui_event *ev)
{
    if (K.nq == MAX_EVENTS)
    {
        /* Too many waiting: drop the oldest move, else the oldest */
        int drop = 0;
        for (int i = 0; i < K.nq; i++)
            if (K.queue[i].type == EV_MOVE)
            {
                drop = i;
                break;
            }
        memmove(&K.queue[drop], &K.queue[drop + 1], (size_t)(K.nq - drop - 1) * sizeof *K.queue);
        K.nq--;
    }
    ui_event e = *ev;
    e.files = NULL;
    e.n_files = 0;
    e.text = NULL;
    if (ev->type == EV_TEXT && ev->text)
    {
        /* Typed text is copied into a small ring, so the host's buffer can go */
        size_t len = strlen(ev->text);
        if (len >= sizeof K.qtext / 4)
            len = sizeof K.qtext / 4 - 1;
        if (K.qtext_at + len + 1 > sizeof K.qtext)
            K.qtext_at = 0;
        memcpy(K.qtext + K.qtext_at, ev->text, len);
        K.qtext[K.qtext_at + len] = 0;
        e.text = K.qtext + K.qtext_at;
        K.qtext_at += len + 1;
    }
    K.queue[K.nq++] = e;
}

int kit_wants_frame(void)
{
    return K.nq > 0 || K.repaint_at <= os_now();
}

void kit_repaint(void)
{
    K.repaint_at = 0;
}

void kit_repaint_after(double seconds)
{
    double at = K.now + seconds;
    if (at < K.repaint_at)
        K.repaint_at = at;
}

int kit_timeout_ms(void)
{
    if (K.nq > 0)
        return 0;
    if (K.repaint_at > 1e300)
        return -1;
    double ms = (K.repaint_at - os_now()) * 1000;
    return ms <= 0 ? 0 : (int)ceil(ms);
}

double kit_now(void)
{
    return K.now;
}

surface *kit_dst(void)
{
    return K.dst;
}

rect kit_screen(void)
{
    return rect_make(0, 0, K.window->w, K.window->h);
}

int kit_cursor(void)
{
    return K.cursor;
}

void kit_set_cursor(int shape)
{
    K.cursor = shape;
}

/* The topmost overlay of the previous frame at (x, y): its slot + 1, 0 for the
 * window itself, -1 when a modal dialog covers the point */
static int layer_at(int x, int y)
{
    for (int k = K.n_prev - 1; k >= 0; k--)
    {
        overlay *o = &K.slots[K.prev_order[k]];
        if (rect_has(o->r, x, y))
            return K.prev_order[k] + 1;
        if (o->modal)
            return -1;
    }
    return 0;
}

int kit_free_at(int x, int y)
{
    return layer_at(x, y) == 0;
}

static void close_popups_from(int depth)
{
    if (depth < K.n_open)
    {
        K.n_open = depth;
        kit_repaint();
    }
}

static int popup_depth(kit_id id);

void kit_begin(surface *window, double now)
{
    K.window = window;
    K.dst = window;
    K.now = now;
    K.repaint_at = 1e308;
    K.cursor = CURSOR_ARROW;
    window->clip = rect_make(window->ox, window->oy, window->w, window->h);

    /* This frame's input */
    memcpy(K.ev, K.queue, (size_t)K.nq * sizeof *K.ev);
    K.n_ev = K.nq;
    K.nq = 0;
    K.down = K.up = 0;
    K.down_clicks = 0;
    K.wheel_x = K.wheel_y = 0;
    K.n_keys = 0;
    K.text[0] = 0;
    K.text_taken = 0;
    for (int i = 0; i < K.n_ev; i++)
    {
        const ui_event *e = &K.ev[i];
        K.in.mods = e->mods;
        switch (e->type)
        {
        case EV_MOVE:
            K.in.x = e->x;
            K.in.y = e->y;
            K.in.inside = 1;
            break;
        case EV_DOWN:
            K.in.x = e->x;
            K.in.y = e->y;
            K.in.inside = 1;
            if (e->button >= 0 && e->button < 3)
                K.in.held[e->button] = 1;
            if (!K.down)
            {
                K.down = 1;
                K.down_btn = e->button;
                K.down_x = e->x;
                K.down_y = e->y;
            }
            if (e->clicks > K.down_clicks)
                K.down_clicks = e->clicks;
            break;
        case EV_UP:
            K.in.x = e->x;
            K.in.y = e->y;
            if (e->button >= 0 && e->button < 3)
                K.in.held[e->button] = 0;
            K.up = 1;
            K.up_btn = e->button;
            K.up_x = e->x;
            K.up_y = e->y;
            break;
        case EV_WHEEL:
            K.in.x = e->x;
            K.in.y = e->y;
            K.wheel_x += e->wheel_x;
            K.wheel_y += e->wheel_y;
            break;
        case EV_LEAVE:
            K.in.inside = 0;
            break;
        case EV_FOCUS:
            if (!e->button)
                K.in.held[0] = K.in.held[1] = K.in.held[2] = 0;
            break;
        case EV_KEY:
            if (K.n_keys < MAX_KEYS)
            {
                K.keys[K.n_keys].key = e->key;
                K.keys[K.n_keys].mods = e->mods;
                K.keys[K.n_keys++].taken = 0;
            }
            break;
        case EV_TEXT:
            if (e->text && strlen(K.text) + strlen(e->text) < sizeof K.text)
                strcat(K.text, e->text);
            break;
        }
    }
    K.hover_layer = K.in.inside ? layer_at(K.in.x, K.in.y) : -1;
    K.down_layer = K.down ? layer_at(K.down_x, K.down_y) : -1;
    K.down_claimed = 0;

    /* A press outside an open popup (and its anchor) closes it and those above it */
    if (K.down)
        for (int d = 0; d < K.n_open; d++)
        {
            int inside = rect_has(K.open[d].anchor, K.down_x, K.down_y);
            for (int e = d; e < K.n_open && !inside; e++)
            {
                int s = slot_find(K.open[e].id);
                inside = s >= 0 && rect_has(K.slots[s].r, K.down_x, K.down_y);
            }
            if (!inside)
            {
                close_popups_from(d);
                break;
            }
        }
    /* Escape closes the top popup, else dismisses the top modal window */
    for (int i = 0; i < K.n_keys; i++)
        if (K.keys[i].key == KEY_ESCAPE && !K.keys[i].taken)
        {
            if (K.n_open)
            {
                K.n_open--;
                K.keys[i].taken = 1;
            }
            else
                for (int k = K.n_prev - 1; k >= 0; k--)
                    if (K.slots[K.prev_order[k]].modal)
                    {
                        K.slots[K.prev_order[k]].dismiss = 1;
                        K.keys[i].taken = 1;
                        break;
                    }
        }

    K.n_ids = 1;
    K.ids[0] = 0;
    memset(K.seen, 0, sizeof K.seen);
    K.n_focusable = 0;
    K.focus_seen = 0;
    K.active_seen = 0;
    K.layer = 0;
    K.n_layers = 0;
    K.n_order = 0;
    K.again = 0;
    K.tip_seen = 0;
    for (int i = 0; i < MAX_SLOTS; i++)
        K.slots[i].alive = 0;
    K.nl = 0;
    kit_area(kit_screen(), 0, 0);
}

int kit_end(void)
{
    while (K.nl > 0)
        K.nl--;
    if (!K.active_seen)
        K.active = 0;
    if (K.active && !K.in.held[K.active_btn])
        K.active = 0;
    /* A press nobody focusable took moves the focus away */
    if (K.down && K.focus && K.press_focus != K.focus && K.down_claimed_focus == 0)
        K.focus = 0;
    K.down_claimed_focus = 0;
    K.press_focus = 0;
    if (!K.focus_seen && K.focus)
        K.focus = 0;
    /* Tab and Shift+Tab move the focus along the focusable widgets */
    for (int i = 0; i < K.n_keys; i++)
        if (K.keys[i].key == KEY_TAB && !K.keys[i].taken && K.n_focusable)
        {
            int at = -1;
            for (int k = 0; k < K.n_focusable; k++)
                if (K.focusable[k] == K.focus)
                    at = k;
            int step = K.keys[i].mods & KMOD_SHIFT ? K.n_focusable - 1 : 1;
            K.focus = K.focusable[at < 0 ? 0 : (at + step) % K.n_focusable];
            K.keys[i].taken = 1;
            kit_repaint();
        }
    /* Popups whose anchor wasn't shown are closed */
    for (int d = 0; d < K.n_open; d++)
        if (!K.open[d].alive)
        {
            close_popups_from(d);
            break;
        }
    for (int d = 0; d < K.n_open; d++)
        K.open[d].alive = 0;

    /* The overlays on top, in the order they were opened */
    surface *w = K.window;
    w->clip = kit_screen();
    for (int k = 0; k < K.n_order; k++)
    {
        overlay *o = &K.slots[K.order[k]];
        if (o->popup && popup_depth(o->id) < 0)
            continue;   /* closed while it was built (an item was picked) */
        if (o->modal)
            gfx_fill(w, kit_screen(), BACKDROP);
        if (o->transparent)
        {
            rect v = rect_cut(o->r, rect_make(o->s->ox, o->s->oy, o->s->w, o->s->h));
            gfx_blend(w, v.x, v.y, o->s->px + (size_t)(v.y - o->s->oy) * (size_t)o->s->stride + (v.x - o->s->ox), v.w, v.h, o->s->stride);
            continue;
        }
        gfx_shadow(w, o->r, S(o->shadow_dx), S(o->shadow_dy), S(o->shadow_blur), COLOR_A(0, o->shadow_alpha));
        gfx_copy(w, o->s, o->r);
    }
    K.n_prev = K.n_order;
    memcpy(K.prev_order, K.order, sizeof K.order);
    for (int i = 0; i < MAX_SLOTS; i++)
        if (!K.slots[i].alive && K.slots[i].s)
        {
            surface_free(K.slots[i].s);
            memset(&K.slots[i], 0, sizeof K.slots[i]);
        }
    tooltip_draw();
    if (K.again)
        kit_repaint();
    return K.again;
}

/* ---- input ---- */

const kit_input *kit_in(void)
{
    return &K.in;
}

const ui_event *kit_events(int *count)
{
    *count = K.n_ev;
    return K.ev;
}

int kit_key(int key, int mods)
{
    for (int i = 0; i < K.n_keys; i++)
    {
        int m = K.keys[i].mods & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT);
        if (!(mods & KMOD_SHIFT) && !(key >= 'A' && key <= 'Z'))
            m &= ~KMOD_SHIFT;
        if (!K.keys[i].taken && K.keys[i].key == key && m == mods)
        {
            K.keys[i].taken = 1;
            return 1;
        }
    }
    return 0;
}

const char *kit_text(void)
{
    return K.text[0] && !K.text_taken ? K.text : NULL;
}

float kit_wheel(rect r, float *wheel_x)
{
    if ((K.wheel_y == 0 && K.wheel_x == 0) || !hover_ok(r))
        return 0;
    float y = K.wheel_y;
    if (wheel_x)
        *wheel_x = K.wheel_x;
    K.wheel_x = K.wheel_y = 0;
    return y;
}

/* ---- ids ---- */

static kit_id mix_id(kit_id h, const char *s, int n)
{
    h ^= 2166136261u;
    for (; s && *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    h = (h ^ (uint32_t)n) * 16777619u;
    return h ? h : 1;
}

kit_id kit_id_of(const char *key)
{
    return mix_id(K.ids[K.n_ids - 1], key, 0);
}

kit_id kit_id_n(const char *key, int n)
{
    return mix_id(K.ids[K.n_ids - 1], key, n + 1);
}

void kit_push_id(const char *key)
{
    if (K.n_ids < MAX_IDS)
    {
        K.ids[K.n_ids] = kit_id_of(key);
        K.n_ids++;
    }
}

void kit_push_n(int n)
{
    if (K.n_ids < MAX_IDS)
    {
        K.ids[K.n_ids] = kit_id_n("#", n);
        K.n_ids++;
    }
}

void kit_pop_id(void)
{
    if (K.n_ids > 1)
        K.n_ids--;
}

/* The same id twice in a frame (two "OK"s) gets told apart by its turn */
static kit_id unique(kit_id id)
{
    for (int turn = 0; turn < 64; turn++)
    {
        kit_id want = turn ? mix_id(id, "dup", turn) : id;
        unsigned h = want % SEEN;
        for (int probe = 0; probe < SEEN; probe++, h = (h + 1) % SEEN)
        {
            if (K.seen[h] == want)
                break;
            if (K.seen[h] == 0)
            {
                K.seen[h] = want;
                return want;
            }
        }
    }
    return id;
}

/* ---- layout ---- */

static lay *top(void)
{
    return &K.lay[K.nl - 1];
}

static void push_lay(rect max, int dir, int gap_px, int cross, int is_area)
{
    if (K.nl == MAX_LAYOUT)
        K.nl--;
    lay *l = &K.lay[K.nl++];
    memset(l, 0, sizeof *l);
    l->max = max;
    l->dir = dir;
    l->gap = gap_px;
    l->cross = cross;
    l->is_area = is_area;
    l->x = dir == DIR_RTL ? max.x + max.w : max.x;
    l->y = max.y;
    l->ex = l->x;
    l->ey = max.y;
}

static void extend(lay *l, rect r)
{
    if (l->dir == DIR_RTL)
    {
        if (r.x < l->ex)
            l->ex = r.x;
    }
    else if (r.x + r.w > l->ex)
        l->ex = r.x + r.w;
    if (r.y + r.h > l->ey)
        l->ey = r.y + r.h;
}

/* Where the next child of l starts (after the gap) */
static void next_origin(lay *l, int *x, int *y)
{
    int gap = l->n ? l->gap : 0;
    *x = l->dir == DIR_ROW ? l->x + gap : l->dir == DIR_RTL ? l->x - gap : l->max.x;
    *y = l->dir == DIR_COL ? l->y + gap : l->max.y;
}

static rect take(lay *l, int w, int h, int at_top)
{
    int x, y;
    next_origin(l, &x, &y);
    rect r;
    if (l->dir == DIR_ROW)
    {
        r = rect_make(x, y + (l->cross && !at_top ? (l->cross - h) / 2 : 0), w, h);
        l->x = x + w;
    }
    else if (l->dir == DIR_RTL)
    {
        r = rect_make(x - w, y + (l->cross && !at_top ? (l->cross - h) / 2 : 0), w, h);
        l->x = x - w;
    }
    else
    {
        r = rect_make(x, y, w, h);
        l->y = y + h;
    }
    l->n++;
    extend(l, r);
    if (l->cross && l->dir != DIR_COL && l->max.y + l->cross > l->ey)
        l->ey = l->max.y + l->cross;
    return r;
}

static void begin_child(int dir, int gap_pt, int cross_pt)
{
    lay *p = top();
    int x, y;
    next_origin(p, &x, &y);
    rect m = rect_make(x, y, p->max.x + p->max.w - x, p->max.y + p->max.h - y);
    if (p->dir == DIR_RTL)
        m = rect_make(p->max.x, y, x - p->max.x, p->max.h);
    push_lay(m, dir, S((float)gap_pt), S((float)cross_pt), 0);
}

void kit_row(int gap_pt)
{
    begin_child(DIR_ROW, gap_pt, 0);
}

void kit_row_centered(int gap_pt, int h_pt)
{
    begin_child(DIR_ROW, gap_pt, h_pt);
}

void kit_row_rtl(int gap_pt, int h_pt)
{
    begin_child(DIR_RTL, gap_pt, h_pt);
}

void kit_col(int gap_pt)
{
    begin_child(DIR_COL, gap_pt, 0);
}

static rect used_of(lay *l)
{
    if (l->dir == DIR_RTL)
        return rect_make(l->ex, l->max.y, l->max.x + l->max.w - l->ex, l->ey - l->max.y);
    return rect_make(l->max.x, l->max.y, l->ex - l->max.x, l->ey - l->max.y);
}

rect kit_end_layout(void)
{
    if (K.nl < 2)
        return rect_make(0, 0, 0, 0);
    lay *l = top();
    rect u = used_of(l);
    int is_area = l->is_area;
    K.nl--;
    if (is_area)
        return u;
    lay *p = top();
    if (p->dir == DIR_RTL)
        return take(p, u.w, u.h, 1);
    return take(p, u.w, u.h, 1);
}

void kit_area(rect r, int row, int gap_pt)
{
    push_lay(r, row ? DIR_ROW : DIR_COL, S((float)gap_pt), 0, 1);
}

rect kit_area_end(void)
{
    lay *l = top();
    rect u = used_of(l);
    if (K.nl > 1)
        K.nl--;
    return u;
}

rect kit_alloc_px(int w, int h)
{
    return take(top(), w < 0 ? 0 : w, h < 0 ? 0 : h, 0);
}

rect kit_alloc(int w_pt, int h_pt)
{
    return kit_alloc_px(S((float)w_pt), S((float)h_pt));
}

void kit_space_px(int px)
{
    lay *l = top();
    if (l->dir == DIR_ROW)
        l->x += px;
    else if (l->dir == DIR_RTL)
        l->x -= px;
    else
        l->y += px;
    if (l->dir == DIR_COL ? l->y > l->ey : 0)
        l->ey = l->y;
    else if (l->dir == DIR_ROW && l->x > l->ex)
        l->ex = l->x;
    else if (l->dir == DIR_RTL && l->x < l->ex)
        l->ex = l->x;
}

void kit_space(int pt)
{
    kit_space_px(S((float)pt));
}

rect kit_avail(void)
{
    lay *l = top();
    int x, y;
    next_origin(l, &x, &y);
    if (l->dir == DIR_ROW)
        return rect_make(x, l->max.y, l->max.x + l->max.w - x, l->max.h);
    if (l->dir == DIR_RTL)
        return rect_make(l->max.x, l->max.y, x - l->max.x, l->max.h);
    return rect_make(l->max.x, y, l->max.w, l->max.y + l->max.h - y);
}

void kit_set_gap(int pt)
{
    top()->gap = S((float)pt);
}

void kit_min_width(int pt)
{
    lay *l = top();
    if (l->dir == DIR_RTL)
    {
        if (l->max.x + l->max.w - S((float)pt) < l->ex)
            l->ex = l->max.x + l->max.w - S((float)pt);
    }
    else if (l->max.x + S((float)pt) > l->ex)
        l->ex = l->max.x + S((float)pt);
}

void kit_min_height(int pt)
{
    lay *l = top();
    if (l->max.y + S((float)pt) > l->ey)
        l->ey = l->max.y + S((float)pt);
}

/* ---- interaction ---- */

int hover_ok(rect r)
{
    return K.in.inside && K.layer == K.hover_layer && rect_has(rect_cut(r, K.dst->clip), K.in.x, K.in.y);
}

kit_resp kit_interact_off(rect r, kit_id id, int sense, int enabled)
{
    kit_resp res;
    memset(&res, 0, sizeof res);
    res.r = r;
    res.id = id = unique(id);
    res.x = K.in.x;
    res.y = K.in.y;
    res.enabled = enabled != 0;
    if (sense & SENSE_FOCUS)
    {
        if (K.n_focusable < MAX_FOCUSABLE && enabled)
            K.focusable[K.n_focusable++] = id;
        if (K.focus == id)
            K.focus_seen = 1;
    }
    rect vis = rect_cut(r, K.dst->clip);
    res.hovered = hover_ok(r) && (!K.active || K.active == id);
    if (!enabled)
    {
        res.hovered = 0;
        return res;
    }
    if (sense && K.down && !K.down_claimed && K.down_layer == K.layer && rect_has(vis, K.down_x, K.down_y))
    {
        K.down_claimed = 1;
        K.active = id;
        K.active_btn = K.down_btn;
        K.active_clicks = K.down_clicks;
        res.drag_started = (sense & SENSE_DRAG) != 0;
        if (sense & SENSE_FOCUS)
        {
            K.focus = id;
            K.focus_seen = 1;
            K.press_focus = id;
            K.down_claimed_focus = 1;
        }
    }
    if (K.active == id)
    {
        K.active_seen = 1;
        res.held = K.in.held[K.active_btn] != 0;
        res.dragging = res.held && (sense & SENSE_DRAG);
        if (K.up && K.up_btn == K.active_btn)
        {
            res.released = 1;
            res.held = 0;
            if (rect_has(vis, K.up_x, K.up_y) || (sense & SENSE_DRAG && K.up_btn == BTN_LEFT && rect_has(vis, K.down_x, K.down_y)))
            {
                res.clicked = K.active_btn == BTN_LEFT;
                res.right_clicked = K.active_btn == BTN_RIGHT;
                res.double_clicked = res.clicked && K.active_clicks >= 2;
            }
            K.active = 0;
        }
    }
    res.focused = K.focus == id;
    return res;
}

kit_resp kit_interact(rect r, kit_id id, int sense)
{
    return kit_interact_off(r, id, sense, 1);
}

void kit_focus(kit_id id)
{
    K.focus = id;
    K.focus_seen = 1;
}

kit_id kit_focused(void)
{
    return K.focus;
}

void kit_hover_cursor(const kit_resp *r, int shape)
{
    if (r->hovered || K.active == r->id)
        K.cursor = shape;
}

/* ---- tooltips ---- */

void kit_tooltip(const kit_resp *r, const char *text)
{
    if (!r->hovered || K.in.held[0] || K.in.held[1] || !text || !*text)
        return;
    K.tip_seen = 1;
    if (K.tip_id != r->id || K.down)
    {
        K.tip_id = r->id;
        K.tip_since = K.now;
        /* Under the widget, or under the pointer for a big one */
        K.tip_x = r->r.h > S(100) ? K.in.x : r->r.x;
        K.tip_y = r->r.h > S(100) ? K.in.y + S(16) : r->r.y + r->r.h;
    }
    double wait = 0.5 - (K.now - K.tip_since);
    if (wait > 0)
    {
        kit_repaint_after(wait);
        return;
    }
    snprintf(K.tip_text, sizeof K.tip_text, "%s", text);
    K.tip_shown = 1;
}

void tooltip_draw(void)
{
    if (!K.tip_seen)
        K.tip_id = 0;
    if (!K.tip_seen || !K.tip_shown)
    {
        K.tip_shown = 0;
        return;
    }
    K.tip_shown = 0;
    font *f = ui_font(FONT_BODY, 0);
    int pad = S(MENU_MARGIN), maxw = S(500);
    int tw = 0;
    /* Each line no wider than it needs */
    for (const char *s = K.tip_text; *s;)
    {
        const char *nl = strchr(s, '\n');
        int n = nl ? (int)(nl - s) : (int)strlen(s);
        int w = text_width(f, s, n);
        if (w > tw)
            tw = w;
        s += n + (nl != NULL);
    }
    if (tw > maxw)
        tw = maxw;
    int th = text_wrap(NULL, f, rect_make(0, 0, tw, 0), ALIGN_LEFT, K.tip_text, 0);
    rect scr = kit_screen();
    rect r = rect_make(K.tip_x, K.tip_y + S(4), tw + 2 * pad + 2, th + 2 * pad + 2);
    if (r.x + r.w > scr.w)
        r.x = scr.w - r.w;
    if (r.y + r.h > scr.h)
        r.y = K.tip_y - S(40) - r.h;
    if (r.x < 0)
        r.x = 0;
    surface *w = K.window;
    gfx_shadow(w, r, S(2), S(3), S(6), COLOR_A(0, 45));
    gfx_fill(w, r, PAL_CARD);
    gfx_frame(w, r, HAIR(), VIS_WINDOW_STROKE);
    text_wrap(w, f, rect_make(r.x + 1 + pad, r.y + 1 + pad, tw, th), ALIGN_LEFT, K.tip_text, PAL_TEXT);
}

/* ---- overlays ---- */

int slot_find(kit_id id)
{
    for (int i = 0; i < MAX_SLOTS; i++)
        if (K.slots[i].id == id && K.slots[i].s)
            return i;
    return -1;
}

static int slot_get(kit_id id)
{
    int s = slot_find(id);
    if (s >= 0)
        return s;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (!K.slots[i].s)
        {
            memset(&K.slots[i], 0, sizeof K.slots[i]);
            K.slots[i].id = id;
            K.slots[i].fresh = 1;
            K.slots[i].s = surface_at(rect_make(0, 0, 1, 1));
            return i;
        }
    return -1;
}

/* Start drawing into overlay slot s, assumed to cover r; the content area is r inset by `inset` */
int layer_begin(int s, rect r, color fill)
{
    overlay *o = &K.slots[s];
    rect scr = kit_screen();
    if (r.w > scr.w)
        r.w = scr.w;
    if (r.h > scr.h)
        r.h = scr.h;
    if (r.x + r.w > scr.w)
        r.x = scr.w - r.w;
    if (r.y + r.h > scr.h)
        r.y = scr.h - r.h;
    if (r.x < 0)
        r.x = 0;
    if (r.y < 0)
        r.y = 0;
    o->assumed = r;
    o->alive = 1;
    surface_move(o->s, r);
    o->transparent = C_ALPHA(fill) == 0;
    if (o->transparent)
        memset(o->s->px, 0, (size_t)r.w * (size_t)r.h * 4);
    else
        gfx_fill(o->s, r, fill);
    if (K.n_layers < MAX_LAYERS)
    {
        K.layer_stack[K.n_layers].layer = K.layer;
        K.layer_stack[K.n_layers].dst = K.dst;
        K.layer_stack[K.n_layers].nl = K.nl;
        K.n_layers++;
    }
    if (K.n_order < MAX_SLOTS)
        K.order[K.n_order++] = s;
    K.layer = s + 1;
    K.dst = o->s;
    return s;
}

/* Finish the overlay: its real rectangle is `final` */
void layer_end(int s, rect final)
{
    overlay *o = &K.slots[s];
    o->r = final;
    rect a = o->assumed;
    if (final.x != a.x || final.y != a.y || final.w > a.w || final.h > a.h)
        K.again = 1;
    o->fresh = 0;
    if (K.n_layers > 0)
    {
        K.n_layers--;
        K.layer = K.layer_stack[K.n_layers].layer;
        K.dst = K.layer_stack[K.n_layers].dst;
        K.nl = K.layer_stack[K.n_layers].nl;
    }
}

static int popup_depth(kit_id id)
{
    for (int d = 0; d < K.n_open; d++)
        if (K.open[d].id == id)
            return d;
    return -1;
}

int kit_popup_open(kit_id anchor)
{
    return popup_depth(anchor) >= 0;
}

/* The popups open inside the layer being built stay; the others at its level go */
static void popup_open_here(kit_id id, rect anchor)
{
    int keep = 0;
    for (int d = 0; d < K.n_open; d++)
    {
        int s = slot_find(K.open[d].id);
        if (s >= 0 && s + 1 == K.layer)
            keep = d + 1;
    }
    close_popups_from(keep);
    if (K.n_open < MAX_OPEN)
    {
        K.open[K.n_open].id = id;
        K.open[K.n_open].anchor = anchor;
        K.open[K.n_open].alive = 1;
        K.n_open++;
    }
}

void kit_popup_toggle(kit_id anchor)
{
    int d = popup_depth(anchor);
    if (d >= 0)
        close_popups_from(d);
    else
        popup_open_here(anchor, rect_make(0, 0, 0, 0));
}

static int popup_begin(kit_id id, rect anchor, int below, int width_pt, int flags)
{
    int d = popup_depth(id);
    if (d < 0)
        return 0;
    K.open[d].alive = 1;
    K.open[d].anchor = anchor;
    int s = slot_get(id);
    if (s < 0 || K.n_popups == MAX_OPEN)
        return 0;
    overlay *o = &K.slots[s];
    int edge = 1 + S(MENU_MARGIN);
    int w = S((float)width_pt) + 2 * edge;
    rect scr = kit_screen();
    int h = o->fresh ? scr.h : o->r.h;
    rect r = rect_make(anchor.x, below ? anchor.y + anchor.h : anchor.y, w, h);
    if (!o->fresh && r.y + h > scr.h && anchor.y - h >= 0)
        r.y = (below ? anchor.y : anchor.y) - h;     /* no room below: above the anchor */
    if (o->fresh)
        r.h = scr.h - r.y > S(40) ? scr.h - r.y : scr.h;
    o->modal = 0;
    o->popup = 1;
    o->shadow_dx = 2;
    o->shadow_dy = 3;
    o->shadow_blur = 6;
    o->shadow_alpha = 45;
    layer_begin(s, r, PAL_CARD);
    r = o->assumed;
    K.popups[K.n_popups].slot = s;
    K.popups[K.n_popups].depth = d;
    K.popups[K.n_popups].flags = flags;
    K.popups[K.n_popups].close = 0;
    K.n_popups++;
    kit_push_n((int)id);
    kit_area(rect_make(r.x + edge, r.y + edge, w - 2 * edge, scr.h), 0, 0);
    return 1;
}

int kit_popup_menu(const kit_resp *anchor, int width_pt, int flags)
{
    if (anchor->clicked)
    {
        if (kit_popup_open(anchor->id))
            close_popups_from(popup_depth(anchor->id));
        else
            popup_open_here(anchor->id, anchor->r);
    }
    return popup_begin(anchor->id, anchor->r, 1, width_pt, flags);
}

int kit_context_menu(const kit_resp *on, int width_pt)
{
    kit_id id = mix_id(on->id, "context", 0);
    if (on->right_clicked)
        popup_open_here(id, rect_make(K.up_x, K.up_y, 0, 0));
    int d = popup_depth(id);
    if (d < 0)
        return 0;
    rect at = K.open[d].anchor;
    return popup_begin(id, at, 1, width_pt, 0);
}

void kit_popup_close(void)
{
    if (K.n_popups)
        K.popups[K.n_popups - 1].close = 1;
}

void kit_popup_end(void)
{
    if (!K.n_popups)
        return;
    popup *p = &K.popups[--K.n_popups];
    overlay *o = &K.slots[p->slot];
    rect used = kit_area_end();
    kit_pop_id();
    int edge = 1 + S(MENU_MARGIN);
    rect r = rect_make(o->assumed.x, o->assumed.y, used.w + 2 * edge, used.h + 2 * edge);
    if (r.w < o->assumed.w && used.w == 0)
        r.w = o->assumed.w;
    gfx_frame(o->s, r, HAIR(), VIS_WINDOW_STROKE);
    /* A click inside closes it, unless it stays open for more */
    int clicked_inside = K.up && rect_has(o->r, K.up_x, K.up_y) && rect_has(o->r, K.down_x, K.down_y) && K.down;
    layer_end(p->slot, r);
    if (p->close || (clicked_inside && !(p->flags & POPUP_STAY_OPEN)))
        close_popups_from(p->depth);
}

void kit_overlay_begin(const char *id, rect r, int frame)
{
    int s = slot_get(kit_id_of(id));
    if (s < 0)
        return;
    overlay *o = &K.slots[s];
    rect at = r;
    if (r.h == 0)
        at.h = o->fresh ? kit_screen().h - r.y : o->r.h;
    o->modal = 0;
    o->shadow_dx = frame ? 2 : 0;
    o->shadow_dy = frame ? 4 : 0;
    o->shadow_blur = frame ? 10 : 0;
    o->shadow_alpha = frame ? 55 : 0;
    o->framed = frame;
    o->fit = r.h == 0;
    layer_begin(s, at, frame ? PAL_CARD : 0);
    at = o->assumed;
    K.overlay_slot = s;
    kit_push_id(id);
    kit_area(frame ? rect_inset(at, 1) : at, 0, 0);
}

rect kit_overlay_end(void)
{
    int s = K.overlay_slot;
    overlay *o = &K.slots[s];
    rect used = kit_area_end();
    kit_pop_id();
    rect r = o->assumed;
    if (o->fit)
        r.h = used.h + (o->framed ? 2 : 0);
    if (o->framed)
        gfx_frame(o->s, r, HAIR(), VIS_WINDOW_STROKE);
    layer_end(s, r);
    return r;
}

/* ---- windows ---- */

static int win_begin(const char *id, const char *title, int w_pt, int h_pt, int flags, int *closed, int dialog)
{
    kit_id wid = kit_id_of(id);
    int s = slot_get(wid);
    if (s < 0)
        return 0;
    overlay *o = &K.slots[s];
    rect scr = kit_screen();
    int bar = S(dialog ? 32 : 28);
    if (o->fresh)
    {
        o->r = rect_make(0, 0, S((float)w_pt), h_pt ? S((float)h_pt) : scr.h / 2);
        o->r.x = (scr.w - o->r.w) / 2;
        o->r.y = (scr.h - o->r.h) / 2;
        if (!(flags & WIN_MODAL))
            o->r.x = scr.w - o->r.w - S(40), o->r.y = S(160);
        o->wanted_w = o->r.w;
        o->wanted_h = h_pt ? o->r.h : 0;
    }
    int dismissed = o->dismiss;
    o->dismiss = 0;
    o->modal = (flags & WIN_MODAL) != 0;
    o->shadow_dx = 0;
    o->shadow_dy = dialog ? 8 : 4;
    o->shadow_blur = dialog ? 28 : 16;
    o->shadow_alpha = dialog ? 70 : 50;
    /* Modal: a press on the dim backdrop dismisses it (not the press that opened it) */
    if (o->modal && !o->fresh && K.down && !rect_has(o->r, K.down_x, K.down_y) && layer_at(K.down_x, K.down_y) == -1
        && K.prev_order[K.n_prev - 1] == s)
        dismissed = 1;
    rect r = o->r;
    if (o->modal)
    {
        r.x = (scr.w - r.w) / 2;
        r.y = (scr.h - r.h) / 2;
    }
    layer_begin(s, r, dialog ? DIALOG_FILL : PAL_CARD);
    r = o->assumed;
    o->dialog = dialog;
    o->flags = flags;
    K.win_slot[K.n_wins < MAX_WINS ? K.n_wins++ : MAX_WINS - 1] = s;
    kit_push_id(id);

    /* The title bar: drag to move (non-modal), and the close button */
    rect titlebar = rect_make(r.x, r.y, r.w, bar);
    rect close_r = rect_make(r.x + r.w - S(46), r.y, S(46), bar);
    kit_resp close_b = kit_interact(close_r, kit_id_of("close"), SENSE_CLICK);
    if (!o->modal)
    {
        kit_resp drag = kit_interact(rect_make(titlebar.x, titlebar.y, titlebar.w - close_r.w, bar), kit_id_of("drag"), SENSE_DRAG);
        if (drag.drag_started)
        {
            o->grab_x = K.in.x - o->r.x;
            o->grab_y = K.in.y - o->r.y;
        }
        if (drag.dragging)
        {
            o->r.x = K.in.x - o->grab_x;
            o->r.y = K.in.y - o->grab_y;
            kit_repaint();
        }
    }
    surface *d = K.dst;
    gfx_fill(d, titlebar, dialog ? PAL_CARD : PAL_CARD);
    text_in(d, ui_font(dialog ? FONT_BODY : FONT_BODY, 0), rect_make(r.x + S(12), r.y, r.w - S(12) - close_r.w, bar), ALIGN_LEFT, title,
            dialog ? COLOR(0x000000) : PAL_TEXT);
    color x_color = COLOR(0x000000);
    if (close_b.hovered)
    {
        gfx_fill(d, close_r, COLOR(0xE81123));
        x_color = COLOR(0xFFFFFF);
    }
    float cx = close_r.x + close_r.w / 2.0f, cy = close_r.y + bar / 2.0f, k = SF(5);
    gfx_line(d, cx - k, cy - k, cx + k, cy + k, SF(1), x_color);
    gfx_line(d, cx - k, cy + k, cx + k, cy - k, SF(1), x_color);
    if (close_b.clicked)
        dismissed = 1;
    if (!dialog)
        hair_h(d, r.x, r.x + r.w - 1, r.y + bar - HAIR(), PAL_RIBBON_BORDER);
    if (closed)
        *closed = dismissed;
    o->bar = bar;
    int ml = S(dialog ? 18 : 8), mt = S(dialog ? 14 : 8);
    int body_w = r.w - 2 * ml;
    int body_h = o->wanted_h ? o->r.h - bar - 2 * mt : scr.h;
    kit_area(rect_make(r.x + ml, r.y + bar + mt, body_w, body_h), 0, dialog ? ITEM_SPACING_Y : ITEM_SPACING_Y);
    return 1;
}

static void win_end(void)
{
    if (!K.n_wins)
        return;
    int s = K.win_slot[--K.n_wins];
    overlay *o = &K.slots[s];
    rect used = kit_area_end();
    rect r = o->assumed;
    int ml = S(o->dialog ? 18 : 8), mt = S(o->dialog ? 14 : 8), mb = S(o->dialog ? 16 : 8);
    /* The body may be wider than asked: the window follows it */
    if (used.w + 2 * ml > r.w)
        r.w = used.w + 2 * ml;
    if (!o->wanted_h)
        r.h = o->bar + mt + used.h + mb;
    if (o->modal)
    {
        /* Centred: if the size changed, so did the place, and the frame is built again */
        rect scr = kit_screen();
        r.x = (scr.w - r.w) / 2;
        r.y = (scr.h - r.h) / 2;
    }
    if (o->flags & WIN_RESIZABLE)
    {
        rect grip = rect_make(r.x + r.w - S(12), r.y + r.h - S(12), S(12), S(12));
        kit_resp g = kit_interact(grip, kit_id_of("resize"), SENSE_DRAG);
        kit_hover_cursor(&g, CURSOR_SIZE_NWSE);
        if (g.drag_started)
        {
            o->grab_x = o->r.x + o->r.w - K.in.x;
            o->grab_y = o->r.y + o->r.h - K.in.y;
        }
        if (g.dragging)
        {
            o->r.w = K.in.x + o->grab_x - o->r.x;
            o->r.h = K.in.y + o->grab_y - o->r.y;
            if (o->r.w < S(120))
                o->r.w = S(120);
            if (o->r.h < S(80))
                o->r.h = S(80);
            o->wanted_h = o->r.h;
            kit_repaint();
        }
        for (int i = 1; i <= 3; i++)
            gfx_line(K.dst, r.x + r.w - SF(3 * i + 1), r.y + r.h - 2.0f, r.x + r.w - 2.0f, r.y + r.h - SF(3 * i + 1), SF(1), COLOR(0xA0A0A0));
    }
    gfx_frame(K.dst, r, HAIR(), o->dialog ? DIALOG_STROKE : VIS_WINDOW_STROKE);
    kit_pop_id();
    layer_end(s, r);
    o->r.w = r.w;
    o->r.h = r.h;
    if (!o->modal)
        o->r = r;
}

int kit_window_begin(const char *id, const char *title, int w_pt, int h_pt, int flags, int *closed)
{
    return win_begin(id, title, w_pt, h_pt, flags, closed, 0);
}

void kit_window_end(void)
{
    win_end();
}

int kit_dialog_begin(const char *id, const char *title, int w_pt, int *dismissed)
{
    return win_begin(id, title, w_pt, 0, WIN_MODAL, dismissed, 1);
}

void kit_dialog_end(void)
{
    win_end();
}

/* ---- toasts ---- */

void kit_toast(const char *text, double seconds)
{
    if (K.n_toasts == MAX_TOASTS)
    {
        memmove(&K.toasts[0], &K.toasts[1], sizeof K.toasts[0] * (MAX_TOASTS - 1));
        K.n_toasts--;
    }
    snprintf(K.toasts[K.n_toasts].text, sizeof K.toasts[0].text, "%s", text);
    K.toasts[K.n_toasts].until = K.now + seconds;
    K.n_toasts++;
    kit_repaint();
}

void kit_draw_toasts(int cx, int bottom)
{
    int keep = 0;
    for (int i = 0; i < K.n_toasts; i++)
        if (K.toasts[i].until > K.now)
            K.toasts[keep++] = K.toasts[i];
    K.n_toasts = keep;
    if (!keep)
        return;
    font *f = ui_font(13, 0);
    for (int i = K.n_toasts - 1; i >= 0; i--)
    {
        double fade = (K.toasts[i].until - K.now) / 0.5;
        fade = fade < 0 ? 0 : fade > 1 ? 1 : fade;
        int w = text_width(f, K.toasts[i].text, -1) + S(28), h = font_height(f) + S(12);
        rect pill = rect_make(cx - w / 2, bottom - S(14) - h / 2, w, h);
        gfx_round(K.dst, pill, S(14), COLOR_A(0x282830, (int)(215 * fade)));
        text_in(K.dst, f, pill, ALIGN_CENTER, K.toasts[i].text, COLOR_A(0xFFFFFF, (int)(255 * fade)));
        bottom -= h + S(6);
    }
    kit_repaint_after(0.05);
}
