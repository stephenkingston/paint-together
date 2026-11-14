/*
 * Paint (C port) - the kit's general widgets: labels, buttons, check boxes,
 * radio buttons, text fields, numbers, combo boxes, scroll areas, group boxes
 * (Rust: the egui widgets app.rs uses, in theme.rs's look)
 *
 * Only the focused text field has editing state (caret, selection, scroll),
 * kept in the kit; the text itself is the app's buffer.
 */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ui/kit_internal.h"

static lay *top(void)
{
    return &K.lay[K.nl - 1];
}

static font *body(void)
{
    return ui_font(FONT_BODY, 0);
}

/* ---- labels ---- */

static int lines_width(font *f, const char *text, int *lines)
{
    int w = 0, n = 0;
    for (const char *s = text;; n++)
    {
        const char *nl = strchr(s, '\n');
        int len = nl ? (int)(nl - s) : (int)strlen(s);
        int lw = text_width(f, s, len);
        if (lw > w)
            w = lw;
        if (!nl)
            break;
        s = nl + 1;
    }
    *lines = n + 1;
    return w;
}

static rect label_in(const char *text, font *f, color c, int wrap)
{
    int lines;
    int w = lines_width(f, text, &lines), h = lines * font_height(f);
    rect avail = kit_avail();
    if (wrap && top()->dir == DIR_COL && w > avail.w)
    {
        w = avail.w;
        h = text_wrap(NULL, f, rect_make(0, 0, w, 0), ALIGN_LEFT, text, c);
        rect r = kit_alloc_px(w, h);
        text_wrap(K.dst, f, r, ALIGN_LEFT, text, c);
        return r;
    }
    rect r = kit_alloc_px(w, h);
    text_wrap(K.dst, f, rect_make(r.x, r.y, w + 1, h), ALIGN_LEFT, text, c);
    return r;
}

rect kit_label(const char *text)
{
    return label_in(text, body(), PAL_TEXT, 1);
}

rect kit_label_ex(const char *text, float size_pt, int style, color c)
{
    return label_in(text, ui_font(size_pt, style), c, 1);
}

rect kit_label_wrap(const char *text, float size_pt, color c)
{
    return label_in(text, ui_font(size_pt, 0), c, 1);
}

rect kit_label_w(const char *text, int w_px)
{
    font *f = body();
    rect r = kit_alloc_px(w_px, font_height(f));
    text_in(K.dst, f, rect_make(r.x, r.y, w_px + S(40), r.h), ALIGN_LEFT, text, PAL_TEXT);
    return r;
}

/* ---- buttons ---- */

/* egui's widget frame colours for a state */
static void widget_colors(const kit_resp *r, int open, color *fill, color *stroke)
{
    if (r->held || open)
        *fill = VIS_ACTIVE_BG, *stroke = VIS_ACTIVE_STROKE;
    else if (r->hovered)
        *fill = VIS_HOVER_BG, *stroke = VIS_HOVER_STROKE;
    else
        *fill = VIS_INACTIVE_WEAK, *stroke = VIS_INACTIVE_STROKE;
}

kit_resp kit_button(const char *label)
{
    font *f = body();
    int w = text_width(f, label, -1) + 2 * S(BUTTON_PAD_X), h = font_height(f) + 2 * S(BUTTON_PAD_Y);
    if (h < S(INTERACT_H))
        h = S(INTERACT_H);
    rect r = kit_alloc_px(w, h);
    kit_resp res = kit_interact(r, kit_id_of(label), SENSE_CLICK);
    color fill, stroke;
    widget_colors(&res, 0, &fill, &stroke);
    gfx_fill(K.dst, r, fill);
    gfx_frame(K.dst, r, HAIR(), stroke);
    text_in(K.dst, f, r, ALIGN_CENTER, label, PAL_TEXT);
    return res;
}

kit_resp kit_dialog_button(const char *label, int is_default, int enabled, int min_w_pt)
{
    font *f = body();
    int w = text_width(f, label, -1) + 2 * S(BUTTON_PAD_X), h = S(24);
    if (w < S((float)(min_w_pt ? min_w_pt : 80)))
        w = S((float)(min_w_pt ? min_w_pt : 80));
    rect r = kit_alloc_px(w, h);
    kit_resp res = kit_interact_off(r, kit_id_of(label), SENSE_CLICK, enabled);
    color fill = DIALOG_BUTTON, edge = is_default ? PAL_ACCENT : DIALOG_BUTTON_EDGE, text = PAL_TEXT;
    if (res.held)
        fill = COLOR(0xCCE4F7), edge = COLOR(0x005499);
    else if (res.hovered)
        fill = COLOR(0xE5F1FB), edge = PAL_ACCENT;
    if (!enabled)
    {
        /* egui fades a disabled widget to half */
        fill = color_mix(fill, DIALOG_FILL, 128);
        edge = color_mix(edge, DIALOG_FILL, 128);
        text = color_mix(text, DIALOG_FILL, 128);
    }
    gfx_fill(K.dst, r, fill);
    gfx_rrect_ring(K.dst, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 0, is_default ? SF(1.5f) : SF(1), edge);
    text_in(K.dst, f, r, ALIGN_CENTER, label, text);
    return res;
}

/* ---- check boxes and radio buttons ---- */

/* The box or circle and the label beside it; returns the response */
static kit_resp toggle(const char *label, int round, int on, int enabled)
{
    font *f = body();
    int lines, tw = lines_width(f, label, &lines);
    int icon = S(14), gap = S(4), h = lines * font_height(f);
    if (h < S(INTERACT_H))
        h = S(INTERACT_H);
    rect r = kit_alloc_px(icon + gap + tw, h);
    kit_resp res = kit_interact_off(r, kit_id_of(label), SENSE_CLICK, enabled);
    color stroke = res.held ? VIS_ACTIVE_STROKE : res.hovered ? VIS_HOVER_STROKE : VIS_INACTIVE_STROKE;
    color fill = res.held ? VIS_ACTIVE_BG : res.hovered ? VIS_HOVER_BG : VIS_INACTIVE_BG;
    color ink = enabled ? PAL_TEXT : DISABLED_TEXT;
    if (!enabled)
        stroke = color_mix(stroke, PAL_RIBBON, 128);
    rect b = rect_make(r.x, r.y + (r.h - icon) / 2, icon, icon);
    if (round)
    {
        float cx = b.x + icon / 2.0f, cy = b.y + icon / 2.0f;
        gfx_circle(K.dst, cx, cy, icon / 2.0f, stroke);
        gfx_circle(K.dst, cx, cy, icon / 2.0f - SF(1), fill);
        if (on)
            gfx_circle(K.dst, cx, cy, SF(4), ink);
    }
    else
    {
        gfx_fill(K.dst, b, fill);
        gfx_frame(K.dst, b, HAIR(), stroke);
        if (on)
        {
            /* egui's tick in the inner 8 x 8 */
            float x0 = b.x + (icon - SF(8)) / 2.0f, y0 = b.y + (icon - SF(8)) / 2.0f, s = SF(8);
            float pts[6] = { x0, y0 + s / 2, x0 + s / 2 - SF(1), y0 + s - SF(1), x0 + s, y0 + SF(1) };
            gfx_stroke(K.dst, pts, 3, SF(1.25f), ink, GFX_MITER);
        }
    }
    int th = lines * font_height(f);
    text_wrap(K.dst, f, rect_make(r.x + icon + gap, r.y + (r.h - th) / 2, tw + 1, th), ALIGN_LEFT, label, ink);
    return res;
}

int kit_checkbox_off(const char *label, int *on, int enabled)
{
    kit_resp r = toggle(label, 0, *on, enabled);
    if (r.clicked)
        *on = !*on;
    return r.clicked;
}

int kit_checkbox(const char *label, int *on)
{
    return kit_checkbox_off(label, on, 1);
}

int kit_radio(const char *label, int selected, int enabled)
{
    return toggle(label, 1, selected, enabled).clicked;
}

/* ---- lines ---- */

void kit_separator(void)
{
    lay *l = top();
    if (l->dir == DIR_COL)
    {
        rect r = kit_alloc_px(kit_avail().w, S(6));
        hair_h(K.dst, r.x, r.x + r.w - 1, r.y + r.h / 2, PAL_RIBBON_BORDER);
    }
    else
    {
        rect r = kit_alloc_px(S(6), l->cross ? l->cross : kit_avail().h);
        hair_v(K.dst, r.x + r.w / 2, r.y, r.y + r.h - 1, PAL_RIBBON_BORDER);
    }
}

void kit_rule(int vertical)
{
    if (vertical)
    {
        rect r = kit_alloc_px(1, top()->cross ? top()->cross : kit_avail().h);
        hair_v(K.dst, r.x, r.y, r.y + r.h - 1, PAL_RIBBON_BORDER);
    }
    else
    {
        rect r = kit_alloc_px(kit_avail().w, 1);
        hair_h(K.dst, r.x, r.x + r.w - 1, r.y, PAL_RIBBON_BORDER);
    }
}

/* ---- text editing ---- */

static int is_word(char c)
{
    return isalnum((unsigned char)c) || (c & 0x80);
}

static int prev_char(const char *s, int i)
{
    while (i > 0 && (s[--i] & 0xC0) == 0x80)
        ;
    return i;
}

static int next_char(const char *s, int i)
{
    if (!s[i])
        return i;
    while (s[++i] && (s[i] & 0xC0) == 0x80)
        ;
    return i;
}

static int count_chars(const char *s)
{
    int n = 0;
    for (; *s; s++)
        n += (*s & 0xC0) != 0x80;
    return n;
}

/* The caret position nearest to x pixels into the text */
static int index_at(font *f, const char *s, int x)
{
    int best = 0, best_d = abs(x);
    for (int i = next_char(s, 0), prev = 0; prev < (int)strlen(s); prev = i, i = next_char(s, i))
    {
        int d = abs(text_width(f, s, i) - x);
        if (d < best_d)
            best = i, best_d = d;
    }
    return best;
}

static void erase(char *buf, int a, int b)
{
    memmove(buf + a, buf + b, strlen(buf + b) + 1);
}

static int insert(char *buf, int cap, int max_chars, int at, const char *text, int digits)
{
    int added = 0;
    for (const char *s = text; *s;)
    {
        int n = next_char(s, 0);
        unsigned char c = (unsigned char)*s;
        int ok = c >= 0x20 && c != 0x7F && (!digits || (c >= '0' && c <= '9'));
        if (ok && (max_chars <= 0 || count_chars(buf) < max_chars) && (int)strlen(buf) + n < cap)
        {
            memmove(buf + at + n, buf + at, strlen(buf + at) + 1);
            memcpy(buf + at, s, (size_t)n);
            at += n;
            added += n;
        }
        s += n;
    }
    return added;
}

static void delete_selection(char *buf)
{
    int a = K.te_anchor < K.te_caret ? K.te_anchor : K.te_caret, b = K.te_anchor < K.te_caret ? K.te_caret : K.te_anchor;
    erase(buf, a, b);
    K.te_caret = K.te_anchor = a;
}

/* Keyboard editing of the focused field; returns KIT_* bits */
static int edit_keys(char *buf, int cap, int max_chars, int digits)
{
    int out = 0, len;
    for (int i = 0; i < K.n_keys; i++)
    {
        if (K.keys[i].taken)
            continue;
        int key = K.keys[i].key, shift = K.keys[i].mods & KMOD_SHIFT, ctrl = K.keys[i].mods & KMOD_CTRL;
        int sel = K.te_anchor != K.te_caret, c = K.te_caret, taken = 1;
        len = (int)strlen(buf);
        if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_HOME || key == KEY_END)
        {
            if (key == KEY_HOME)
                c = 0;
            else if (key == KEY_END)
                c = len;
            else if (!shift && sel)
                c = key == KEY_LEFT ? (K.te_anchor < c ? K.te_anchor : c) : (K.te_anchor > c ? K.te_anchor : c);
            else if (key == KEY_LEFT)
            {
                c = prev_char(buf, c);
                while (ctrl && c > 0 && !(is_word(buf[c]) && !is_word(buf[c - 1])))
                    c = prev_char(buf, c);
            }
            else
            {
                c = next_char(buf, c);
                while (ctrl && c < len && !(!is_word(buf[c]) && is_word(buf[c - 1])))
                    c = next_char(buf, c);
            }
            K.te_caret = c;
            if (!shift)
                K.te_anchor = c;
        }
        else if (key == KEY_BACKSPACE || key == KEY_DELETE)
        {
            if (!sel)
                K.te_anchor = key == KEY_BACKSPACE ? prev_char(buf, c) : next_char(buf, c);
            if (K.te_anchor != K.te_caret)
                out |= KIT_CHANGED;
            delete_selection(buf);
        }
        else if (key == KEY_ENTER)
            out |= KIT_SUBMITTED;
        else if (key == KEY_ESCAPE)
            out |= KIT_LOST_FOCUS;
        else if (ctrl && key == 'A')
        {
            K.te_anchor = 0;
            K.te_caret = len;
        }
        else if (ctrl && (key == 'C' || key == 'X') && sel)
        {
            int a = K.te_anchor < c ? K.te_anchor : c, b = K.te_anchor < c ? c : K.te_anchor;
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%.*s", b - a, buf + a);
            host_clip_set_text(tmp);
            if (key == 'X')
            {
                delete_selection(buf);
                out |= KIT_CHANGED;
            }
        }
        else if (ctrl && key == 'V')
        {
            char *clip = host_clip_text();
            if (clip)
            {
                for (char *p = clip; *p; p++)
                    if (*p == '\n' || *p == '\r' || *p == '\t')
                        *p = ' ';
                if (sel)
                    delete_selection(buf);
                K.te_caret += insert(buf, cap, max_chars, K.te_caret, clip, digits);
                K.te_anchor = K.te_caret;
                out |= KIT_CHANGED;
                free(clip);
            }
        }
        else if (key == KEY_TAB || (key >= KEY_F1 && key <= KEY_F12) || (ctrl && key != KEY_SHIFT && key != KEY_CTRL))
            taken = 0;  /* focus moves, shortcuts stay the app's */
        K.keys[i].taken = taken;
    }
    if (K.text[0] && !K.text_taken)
    {
        K.text_taken = 1;
        if (K.te_anchor != K.te_caret)
            delete_selection(buf);
        int n = insert(buf, cap, max_chars, K.te_caret, K.text, digits);
        K.te_caret += n;
        K.te_anchor = K.te_caret;
        out |= KIT_CHANGED;
    }
    if (out)
        K.te_blink = K.now;
    return out;
}

/* A text field's box, editing and drawing; `select_all` when it gains focus */
static int field(kit_id id, rect box, char *buf, int cap, int max_chars, const char *hint, int flags, int select_all)
{
    int enabled = !(flags & FIELD_DISABLED), out = 0;
    font *f = body();
    kit_resp r = kit_interact_off(box, id, SENSE_DRAG | SENSE_FOCUS, enabled);
    kit_hover_cursor(&r, CURSOR_TEXT);
    int had = K.te_id == id, has = K.focus == id && enabled;
    rect inner = rect_make(box.x + S(4), box.y + S(2), box.w - S(8), box.h - S(4));
    if (has && !had)
    {
        K.te_id = id;
        K.te_caret = (int)strlen(buf);
        K.te_anchor = select_all ? 0 : K.te_caret;
        K.te_scroll = 0;
        K.te_blink = K.now;
        out |= KIT_GAINED_FOCUS;
    }
    else if (had && !has)
    {
        K.te_id = 0;
        out |= KIT_LOST_FOCUS;
    }
    if (has)
    {
        if ((int)strlen(buf) < K.te_caret)
            K.te_caret = K.te_anchor = (int)strlen(buf);
        int at = index_at(f, buf, K.in.x - inner.x + K.te_scroll);
        if (r.drag_started && !(out & KIT_GAINED_FOCUS && select_all))
        {
            K.te_caret = at;
            if (!(K.in.mods & KMOD_SHIFT))
                K.te_anchor = at;
            if (K.down_clicks >= 2)
            {
                /* A double click selects the word */
                int a = at, b = at;
                while (a > 0 && is_word(buf[a - 1]))
                    a--;
                while (buf[b] && is_word(buf[b]))
                    b++;
                K.te_anchor = a;
                K.te_caret = b;
            }
            K.te_blink = K.now;
        }
        else if (r.dragging)
            K.te_caret = at;
        out |= edit_keys(buf, cap, max_chars, flags & FIELD_DIGITS);
        if (out & (KIT_SUBMITTED | KIT_LOST_FOCUS))
        {
            if (K.focus == id)
                K.focus = 0;
            K.te_id = 0;
            out |= KIT_LOST_FOCUS;
            has = 0;
        }
    }

    /* The box, egui's TextEdit frame */
    color stroke = has ? PAL_ACCENT : r.hovered ? VIS_HOVER_STROKE : VIS_INACTIVE_STROKE;
    gfx_fill(K.dst, box, enabled ? VIS_INACTIVE_BG : COLOR(0xF7F7F7));
    gfx_frame(K.dst, box, HAIR(), enabled ? stroke : color_mix(stroke, PAL_RIBBON, 128));
    rect old = gfx_clip(K.dst, inner);
    int ty = inner.y + (inner.h - font_height(f) + 1) / 2;
    if (has)
    {
        int cx = text_width(f, buf, K.te_caret);
        if (cx - K.te_scroll > inner.w - 1)
            K.te_scroll = cx - inner.w + 1;
        if (cx < K.te_scroll)
            K.te_scroll = cx;
        if (K.te_anchor != K.te_caret)
        {
            int ax = text_width(f, buf, K.te_anchor);
            int x0 = (ax < cx ? ax : cx) - K.te_scroll, x1 = (ax < cx ? cx : ax) - K.te_scroll;
            gfx_fill(K.dst, rect_make(inner.x + x0, ty, x1 - x0, font_height(f)), VIS_SELECTION);
        }
    }
    int scroll = has ? K.te_scroll : 0;
    if (buf[0])
        text_draw(K.dst, f, inner.x - scroll, ty, buf, -1, enabled ? PAL_TEXT : DISABLED_TEXT);
    else if (hint)
        text_draw(K.dst, f, inner.x, ty, hint, -1, COLOR(0x8C8C8C));
    if (has)
    {
        /* The caret blinks: half a second on, half off */
        double t = fmod(K.now - K.te_blink, 1.0);
        if (t < 0.5)
            gfx_fill(K.dst, rect_make(inner.x + text_width(f, buf, K.te_caret) - scroll, ty, HAIR(), font_height(f)), PAL_TEXT);
        kit_repaint_after(t < 0.5 ? 0.5 - t : 1.0 - t);
    }
    K.dst->clip = old;
    return out;
}

int kit_text_field(const char *id, char *buf, int cap, int max_chars, int width_pt, const char *hint, int flags)
{
    font *f = body();
    int w = width_pt > 0 ? S((float)width_pt) : kit_avail().w;
    rect box = kit_alloc_px(w, font_height(f) + S(4) + 2);
    int out = field(kit_id_of(id), box, buf, cap, max_chars, hint, flags, 0);
    return out;
}

int kit_number(const char *id, float *value, float min, float max, int width_pt)
{
    kit_id nid = kit_id_of(id);
    font *f = body();
    rect box = kit_alloc_px(S((float)width_pt), S(INTERACT_H));
    int changed = 0;
    if (K.te_id == nid || K.focus == nid)
    {
        if (K.te_id != nid)
            snprintf(K.num_buf, sizeof K.num_buf, "%.0f", *value);
        int out = field(nid, box, K.num_buf, sizeof K.num_buf, 9, NULL, FIELD_DIGITS, 1);
        if (out & KIT_LOST_FOCUS)
        {
            float v = K.num_buf[0] ? (float)atof(K.num_buf) : *value;
            v = v < min ? min : v > max ? max : v;
            changed = v != *value;
            *value = v;
        }
        return changed;
    }
    /* Not editing: drag across to change it, click to type */
    kit_resp r = kit_interact(box, nid, SENSE_DRAG);
    kit_hover_cursor(&r, CURSOR_SIZE_EW);
    if (r.drag_started)
    {
        K.drag_start_value = *value;
        K.drag_moved = 0;
        K.drag_x0 = K.in.x;
    }
    if (r.dragging || r.released)
    {
        int dx = K.in.x - K.drag_x0;
        if (abs(dx) > S(2))
            K.drag_moved = 1;
        if (K.drag_moved)
        {
            float v = roundf(K.drag_start_value + dx / SF(1));
            v = v < min ? min : v > max ? max : v;
            changed = v != *value;
            *value = v;
        }
    }
    if (r.clicked && !K.drag_moved)
        kit_focus(nid);
    color fill, stroke;
    widget_colors(&r, 0, &fill, &stroke);
    gfx_fill(K.dst, box, fill == VIS_INACTIVE_WEAK ? VIS_INACTIVE_BG : fill);
    gfx_frame(K.dst, box, HAIR(), stroke);
    char num[32];
    snprintf(num, sizeof num, "%.0f", *value);
    text_in(K.dst, f, box, ALIGN_CENTER, num, PAL_TEXT);
    return changed;
}

/* ---- scroll areas ---- */

static scroll_state *scroll_get(kit_id id)
{
    scroll_state *spare = NULL;
    for (int i = 0; i < MAX_SCROLLS; i++)
    {
        if (K.scrolls[i].id == id)
            return &K.scrolls[i];
        if (!spare && (!K.scrolls[i].id || !K.scrolls[i].alive))
            spare = &K.scrolls[i];
    }
    if (!spare)
        spare = &K.scrolls[0];
    memset(spare, 0, sizeof *spare);
    spare->id = id;
    return spare;
}

void kit_scroll_to(const char *id, int x, int y)
{
    scroll_state *s = scroll_get(kit_id_of(id));
    s->x = x;
    s->y = y;
}

void kit_scroll_offset(const char *id, int *x, int *y)
{
    scroll_state *s = scroll_get(kit_id_of(id));
    *x = s->x;
    *y = s->y;
}

static void scroll_begin_id(kit_id id, int w, int h, int flags)
{
    scroll_state *s = scroll_get(id);
    s->alive = 1;
    rect view = kit_alloc_px(w, h);
    int bw = S(SCROLL_BAR_W);
    int bar_v = (flags & SCROLL_V) && !(flags & SCROLL_HIDE_BARS) && s->ch > view.h;
    int bar_h = (flags & SCROLL_H) && !(flags & SCROLL_HIDE_BARS) && s->cw > view.w;
    rect inner = rect_make(view.x, view.y, view.w - (bar_v ? bw : 0), view.h - (bar_h ? bw : 0));
    float wx = 0, wy = kit_wheel(inner, &wx);
    if (K.in.mods & KMOD_SHIFT && !wx)
        wx = wy, wy = 0;
    if (flags & SCROLL_V)
        s->y -= (int)(wy * S(48));
    if (flags & SCROLL_H)
        s->x -= (int)(wx * S(48));
    if (wy || wx)
        kit_repaint();
    int max_y = s->ch - inner.h, max_x = s->cw - inner.w;
    if (s->y > max_y)
        s->y = max_y;
    if (s->y < 0 || !(flags & SCROLL_V))
        s->y = 0;
    if (s->x > max_x)
        s->x = max_x;
    if (s->x < 0 || !(flags & SCROLL_H))
        s->x = 0;
    if (K.n_sf < 4)
    {
        scroll_frame *f = &K.sf[K.n_sf++];
        f->id = id;
        f->view = view;
        f->flags = flags;
        f->slot = (int)(s - K.scrolls);
        f->bar_v = bar_v;
        f->bar_h = bar_h;
        f->saved_clip = gfx_clip(K.dst, inner);
    }
    kit_area(rect_make(inner.x - s->x, inner.y - s->y, flags & SCROLL_H ? 1 << 20 : inner.w, 1 << 20), 0, 0);
}

void kit_scroll_begin(const char *id, int w_pt, int h_pt, int flags)
{
    rect a = kit_avail();
    scroll_begin_id(kit_id_of(id), w_pt ? S((float)w_pt) : a.w, h_pt ? S((float)h_pt) : a.h, flags);
}

/* One bar: the track along `track`, its thumb showing view out of content at offset */
static void scroll_bar(kit_id id, rect track, int vertical, int view, int content, int *offset)
{
    int len = vertical ? track.h : track.w;
    int thumb = content > 0 ? len * view / content : len;
    if (thumb < S(16))
        thumb = S(16);
    if (thumb > len)
        thumb = len;
    int range = content - view, room = len - thumb;
    int at = range > 0 ? room * *offset / range : 0;
    rect t = vertical ? rect_make(track.x + 2, track.y + at, track.w - 4, thumb) : rect_make(track.x + at, track.y + 2, thumb, track.h - 4);
    gfx_fill(K.dst, track, COLOR(0xF0F0F0));
    kit_resp r = kit_interact(t, id, SENSE_DRAG);
    if (r.drag_started)
        K.scroll_grab = (vertical ? K.in.y : K.in.x) - at;
    if (r.dragging && room > 0)
    {
        int pos = (vertical ? K.in.y : K.in.x) - K.scroll_grab;
        *offset = range * (pos < 0 ? 0 : pos > room ? room : pos) / room;
        kit_repaint();
    }
    /* A press on the track pages towards it */
    kit_resp tr = kit_interact(track, id + 1, SENSE_CLICK);
    if (tr.clicked)
    {
        int p = vertical ? tr.y : tr.x, page = view * 9 / 10;
        *offset += p < (vertical ? t.y : t.x) ? -page : page;
        *offset = *offset < 0 ? 0 : *offset > range ? range : *offset;
    }
    gfx_fill(K.dst, t, r.held ? COLOR(0x787878) : r.hovered ? COLOR(0xA8A8A8) : COLOR(0xC1C1C1));
}

void kit_scroll_end(void)
{
    if (!K.n_sf)
        return;
    scroll_frame *f = &K.sf[--K.n_sf];
    scroll_state *s = &K.scrolls[f->slot];
    rect used = kit_area_end();
    K.dst->clip = f->saved_clip;
    int bw = S(SCROLL_BAR_W);
    rect inner = rect_make(f->view.x, f->view.y, f->view.w - (f->bar_v ? bw : 0), f->view.h - (f->bar_h ? bw : 0));
    s->cw = used.w;
    s->ch = used.h;
    int want_v = (f->flags & SCROLL_V) && !(f->flags & SCROLL_HIDE_BARS) && s->ch > f->view.h;
    if (want_v != f->bar_v)
        K.again = 1;
    if (f->bar_v)
        scroll_bar(f->id ^ 0x5151, rect_make(inner.x + inner.w, inner.y, bw, inner.h), 1, inner.h, s->ch, &s->y);
    if (f->bar_h)
        scroll_bar(f->id ^ 0x7373, rect_make(inner.x, inner.y + inner.h, inner.w, bw), 0, inner.w, s->cw, &s->x);
}

/* ---- combo boxes ---- */

int kit_combo(const char *id, const char *shown, int width_pt, int list_h_pt)
{
    kit_id cid = kit_id_of(id);
    font *f = body();
    rect box = kit_alloc(width_pt, INTERACT_H);
    kit_resp r = kit_interact(box, cid, SENSE_CLICK);
    int open = kit_popup_open(cid);
    color fill, stroke;
    widget_colors(&r, open, &fill, &stroke);
    gfx_fill(K.dst, box, fill == VIS_INACTIVE_WEAK ? VIS_INACTIVE_BG : fill);
    gfx_frame(K.dst, box, HAIR(), stroke);
    text_in(K.dst, f, rect_make(box.x + S(7), box.y, box.w - S(28), box.h), ALIGN_LEFT, shown, PAL_TEXT);
    /* egui's triangle */
    float cx = box.x + box.w - SF(15), cy = box.y + box.h / 2.0f;
    float tri[6] = { cx - SF(5), cy - SF(3), cx + SF(5), cy - SF(3), cx, cy + SF(3) };
    gfx_polygon(K.dst, tri, 3, PAL_TEXT);
    int edge = MENU_MARGIN + 1;
    if (!kit_popup_menu(&r, width_pt - 2 * edge, 0))
    {
        if (K.combo_id == cid)
            K.combo_id = 0;
        return 0;
    }
    if (K.combo_id != cid)
    {
        K.combo_id = cid;
        K.combo_hot = -1;
        K.find[0] = 0;
        K.combo_reveal = 1;
    }
    K.combo_item = 0;
    K.combo_found = 0;
    /* Typing finds an item; the arrows and Enter pick one */
    if (K.text[0] && !K.text_taken)
    {
        K.text_taken = 1;
        if (K.now - K.find_at > 1.0)
            K.find[0] = 0;
        if (strlen(K.find) + strlen(K.text) < sizeof K.find)
            strcat(K.find, K.text);
        K.find_at = K.now;
        K.find_pending = 1;
    }
    if (kit_key(KEY_DOWN, 0))
        K.combo_hot++;
    if (kit_key(KEY_UP, 0) && K.combo_hot > 0)
        K.combo_hot--;
    kit_id sid = cid ^ 0xC0B0u;
    scroll_state *s = scroll_get(sid);
    int h = S((float)list_h_pt);
    if (s->ch > 0 && s->ch < h)
        h = s->ch;
    scroll_begin_id(sid, kit_avail().w, h, SCROLL_V);
    return 1;
}

static int starts_with(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return 0;
    return 1;
}

int kit_combo_item(const char *text, int selected)
{
    font *f = body();
    int idx = K.combo_item++;
    rect row = kit_alloc_px(kit_avail().w, S(20));
    kit_resp r = kit_interact(row, kit_id_n("item", idx), SENSE_CLICK);
    int reveal = (K.find_pending && !K.combo_found && starts_with(text, K.find)) || (K.combo_reveal && selected);
    if (reveal && K.n_sf)
    {
        /* Bring it into view */
        scroll_frame *sf = &K.sf[K.n_sf - 1];
        scroll_state *s = &K.scrolls[sf->slot];
        int y = row.y - (sf->view.y - s->y);
        if (y < s->y || y + row.h > s->y + sf->view.h)
        {
            s->y = y - (sf->view.h - row.h) / 2;
            K.again = 1;
        }
        if (K.find_pending)
        {
            K.combo_found = 1;
            K.combo_hot = idx;
        }
    }
    if (r.hovered && (K.in.x != K.combo_px || K.in.y != K.combo_py))
        K.combo_hot = -1;
    int hot = K.combo_hot == idx;
    if (selected)
        gfx_fill(K.dst, row, VIS_SELECTION);
    if (r.hovered || hot)
    {
        gfx_fill(K.dst, row, VIS_HOVER_BG);
        gfx_frame(K.dst, row, HAIR(), VIS_HOVER_STROKE);
    }
    text_in(K.dst, f, rect_make(row.x + S(4), row.y, row.w - S(8), row.h), ALIGN_LEFT, text, PAL_TEXT);
    int picked = r.clicked || (hot && kit_key(KEY_ENTER, 0));
    if (picked)
    {
        kit_popup_close();
        K.combo_id = 0;
    }
    return picked;
}

void kit_combo_end(void)
{
    kit_scroll_end();
    K.find_pending = 0;
    K.combo_reveal = 0;
    K.combo_px = K.in.x;
    K.combo_py = K.in.y;
    kit_popup_end();
}

/* ---- group boxes ---- */

void kit_group_box_begin(const char *title, int w_pt)
{
    kit_col(ITEM_SPACING_Y);
    kit_label_ex(title, FONT_BODY, 0, DIALOG_HEADING);
    rect at = kit_avail();
    int m = S(8);
    if (K.n_boxes < 4)
    {
        K.boxes[K.n_boxes].start_x = at.x;
        K.boxes[K.n_boxes].start_y = at.y;
        K.boxes[K.n_boxes].w = S((float)w_pt);
        K.n_boxes++;
    }
    kit_area(rect_make(at.x + m + 1, at.y + m + 1, S((float)w_pt) - 2 * m - 2, at.h - 2 * m - 2), 0, ITEM_SPACING_Y);
}

void kit_group_box_end(void)
{
    rect used = kit_area_end();
    if (!K.n_boxes)
        return;
    K.n_boxes--;
    int m = S(8);
    rect frame = rect_make(K.boxes[K.n_boxes].start_x, K.boxes[K.n_boxes].start_y, K.boxes[K.n_boxes].w, used.h + 2 * m + 2);
    gfx_frame(K.dst, frame, HAIR(), COLOR(0xDCDCDC));
    kit_alloc_px(frame.w, frame.h);
    kit_end_layout();
}
