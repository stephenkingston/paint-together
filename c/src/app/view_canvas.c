/*
 * Paint (C port) - the picture area (Rust: app.rs's canvas, paint_canvas, paint_rulers,
 * paint_friends, paint_caret, selection_menu, ants and paint_tool_pointer)
 *
 * The picture is drawn straight from the app's image with gfx_canvas every
 * frame (no zoomed copy is kept). Only while friends are drawing is a merged
 * copy kept (their unfinished drawing over the picture), updated where it
 * changed, and freed when they stop.
 *
 * Geometry: everything is in device pixels; `zs` is device pixels per
 * picture pixel (the zoom times the display scale). The Rust's sizes are
 * points, turned into pixels with S().
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/view.h"
#include "paint/live_shape.h"

/* ---- the picture as shown ---- */

static img *merged;     /* the picture with friends' drawing in progress, while there is any */

const img *view_picture(app *a)
{
    irect r;
    int changed = app_take_dirty(a, &r);
    const img *ov = app_friends_overlay(a);
    if (!ov)
    {
        img_free(merged);
        merged = NULL;
        return a->img;
    }
    if (!merged || merged->w != a->img->w || merged->h != a->img->h)
    {
        img_free(merged);
        merged = img_new(a->img->w, a->img->h, (rgba){ 0, 0, 0, 0 });
        if (!merged)
            return a->img;
        r = irect_of(a->img);
        changed = 1;
    }
    if (changed)
    {
        r = irect_intersect(r, irect_of(a->img));
        for (int y = r.y0; y < r.y1; y++)
            app_with_friends(a->img, ov, irect_make(r.x0, y, r.x1, y + 1), img_at(merged, r.x0, y));
    }
    return merged;
}

void view_free_picture(void)
{
    img_free(merged);
    merged = NULL;
}

/* ---- input: the pointer over the picture goes to the app ---- */

void view_canvas_input(app *a)
{
    int n;
    const ui_event *ev = kit_events(&n);
    for (int i = 0; i < n; i++)
    {
        const ui_event *e = &ev[i];
        int over = V.view.w > 0 && rect_has(V.view, e->x, e->y) && kit_free_at(e->x, e->y);
        int send = 0;
        switch (e->type)
        {
        case EV_MOVE:
            if (V.canvas_btns || over)
                send = 1, V.canvas_hover = 1;
            else if (V.canvas_hover)
            {
                ui_event leave = *e;
                leave.type = EV_LEAVE;
                app_canvas_event(a, &leave);
                V.canvas_hover = 0;
            }
            break;
        case EV_DOWN:
            if (over || V.canvas_btns)
                send = 1, V.canvas_btns |= 1 << e->button, V.canvas_hover = 1;
            break;
        case EV_UP:
            if (V.canvas_btns & 1 << e->button)
                send = 1, V.canvas_btns &= ~(1 << e->button);
            break;
        case EV_WHEEL:
            send = over;
            break;
        case EV_LEAVE:
            if (V.canvas_hover && !V.canvas_btns)
                send = 1, V.canvas_hover = 0;
            break;
        case EV_FOCUS:
            if (!e->button && V.canvas_btns)
            {
                /* The window lost the pointer mid-drag: finish it as if released */
                for (int b = 0; b < 3; b++)
                    if (V.canvas_btns & 1 << b)
                    {
                        ui_event up = *e;
                        up.type = EV_UP;
                        up.button = b;
                        up.x = kit_in()->x;
                        up.y = kit_in()->y;
                        app_canvas_event(a, &up);
                    }
                V.canvas_btns = 0;
            }
            break;
        }
        if (send)
            app_canvas_event(a, e);
    }
}

/* ---- drawing helpers ---- */

/* Paint-style marching ants along pts: a white line with dark dashes over it */
static void ants(surface *d, const float *xy, int n, int closed)
{
    if (n < 2)
        return;
    float w = (float)HAIR(), dash = SF(4);
    gfx_stroke(d, xy, n, w, COLOR(0xFFFFFF), closed ? GFX_CLOSED | GFX_MITER : GFX_BUTT);
    float along = 0;
    for (int i = 0; i < n - !closed; i++)
    {
        float x0 = xy[2 * i], y0 = xy[2 * i + 1], x1 = xy[2 * ((i + 1) % n)], y1 = xy[2 * ((i + 1) % n) + 1];
        float len = hypotf(x1 - x0, y1 - y0);
        if (len <= 0)
            continue;
        /* the dashes run on across corners, as egui's dashed_line does */
        for (float t = 0; t < len;)
        {
            float phase = fmodf(along + t, 2 * dash), run = phase < dash ? dash - phase : 2 * dash - phase;
            float t1 = t + run < len ? t + run : len;
            if (phase < dash)
            {
                float seg[4] = { x0 + (x1 - x0) * t / len, y0 + (y1 - y0) * t / len, x0 + (x1 - x0) * t1 / len, y0 + (y1 - y0) * t1 / len };
                gfx_stroke(d, seg, 2, w, COLOR(0x141414), GFX_BUTT);
            }
            t = t1;
        }
        along += len;
    }
}

/* The ants round a box (screen corners a and b, either way round), on pixel centres */
static void ants_box(surface *d, float ax, float ay, float bx, float by)
{
    float x0 = fminf(ax, bx), x1 = fmaxf(ax, bx), y0 = fminf(ay, by), y1 = fmaxf(ay, by), h = HAIR() / 2.0f;
    x0 = floorf(x0) + h, y0 = floorf(y0) + h, x1 = floorf(x1) + h, y1 = floorf(y1) + h;
    float pts[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
    ants(d, pts, 4, 1);
}

/* A small square handle centred on (x, y) */
static void handle(surface *d, float x, float y)
{
    int s = S(7);
    rect r = rect_make((int)floorf(x - s / 2.0f + 0.5f), (int)floorf(y - s / 2.0f + 0.5f), s, s);
    gfx_fill(d, r, COLOR(0xFFFFFF));
    gfx_frame(d, r, HAIR(), COLOR(0x555A64));
}

/* The canvas resize handles: right, bottom, corner (Rust: handle_rects) */
static rect resize_handle(rect c, int which)
{
    int s = S(7), cx = which == HANDLE_BOTTOM ? c.x + c.w / 2 : c.x + c.w + S(4), cy = which == HANDLE_RIGHT ? c.y + c.h / 2 : c.y + c.h + S(4);
    return rect_make(cx - s / 2, cy - s / 2, s, s);
}

/* A tool drawn as the pointer, its tip on (x, y) (Rust: paint_tool_pointer) */
static void tool_pointer(surface *d, int icon, float x, float y)
{
    pt tip = app_tool_tip(icon);
    float k = SF(24) / 20.0f;
    icon_halo(d, icon, x - tip.x * k + SF(12), y - tip.y * k + SF(12), S(24));
}

/* Text turned a quarter left (reading upwards) with its top-left corner at (x, y) going up:
 * the left ruler's numbers. The ruler is white, so the text is drawn on white and copied. */
static void text_up(surface *d, font *f, int x, int y, const char *s, color c)
{
    static surface *tmp;
    int w = text_width(f, s, -1), h = font_height(f);
    if (!tmp)
        tmp = surface_new(w, h);
    else
        surface_resize(tmp, w, h);
    gfx_fill(tmp, rect_make(0, 0, w, h), COLOR(0xFFFFFF));
    text_draw(tmp, f, 0, 0, s, -1, c);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
        {
            int dx = x + j, dy = y - i;
            if (rect_has(d->clip, dx, dy))
                d->px[(size_t)(dy - d->oy) * d->stride + (dx - d->ox)] = tmp->px[(size_t)j * tmp->stride + i];
        }
}

/* ---- the rulers (Rust: paint_rulers) ---- */

static void rulers(app *a, rect full, rect panel, rect crect)
{
    surface *d = kit_dst();
    rect top = rect_make(panel.x, full.y, panel.w, panel.y - full.y), left = rect_make(full.x, panel.y, panel.x - full.x, panel.h);
    gfx_fill(d, top, COLOR(0xFFFFFF));
    gfx_fill(d, left, COLOR(0xFFFFFF));
    gfx_fill(d, rect_make(full.x, full.y, left.w, top.h), PAL_RIBBON);
    hair_h(d, top.x, top.x + top.w - 1, top.y + top.h - HAIR(), COLOR(0xA0A0A0));
    hair_v(d, left.x + left.w - HAIR(), left.y, left.y + left.h - 1, COLOR(0xA0A0A0));

    float z = a->zoom, zs = a->zoom * a->ppp;
    static const float majors[14] = { 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000 };
    float major = 10000;
    for (int i = 0; i < 14; i++)
        if (majors[i] * z >= 64)
        {
            major = majors[i];
            break;
        }
    float minors[4] = { major / 10, major / 5, major / 2, major }, minor = major;
    for (int i = 0; i < 4; i++)
        if (minors[i] >= 1 && minors[i] * z >= 5)
        {
            minor = minors[i];
            break;
        }
    font *f = ui_font(9.5f, 0);
    color tick = COLOR(0x6A6A6A);
    char num[24];
    rect old = gfx_clip(d, rect_union(top, left));
    for (int axis = 0; axis < 2; axis++)
    {
        rect r = axis ? left : top;
        float lo = axis ? r.y : r.x, hi = axis ? r.y + r.h : r.x + r.w, o = axis ? crect.y : crect.x;
        long long first = (long long)floorf((lo - o) / zs / minor), last = (long long)ceilf((hi - o) / zs / minor);
        for (long long i = first; i <= last; i++)
        {
            float v = i * minor, at = o + v * zs;
            if (at < lo || at > hi)
                continue;
            int is_major = fabsf(roundf(v / major) * major - v) < 1e-3f, is_half = fabsf(roundf(v / (major / 2)) * (major / 2) - v) < 1e-3f;
            int len = S(is_major ? APP_RULER - 3 : is_half ? 7 : 4), p = (int)floorf(at);
            snprintf(num, sizeof num, "%lld", (long long)v);
            if (!axis)
            {
                gfx_fill(d, rect_make(p, r.y + r.h - len, HAIR(), len), tick);
                if (is_major)
                    text_draw(d, f, (int)(at + SF(3)), r.y + S(1), num, -1, PAL_TEXT_WEAK);
            }
            else
            {
                gfx_fill(d, rect_make(r.x + r.w - len, p, len, HAIR()), tick);
                if (is_major)
                    text_up(d, f, r.x + S(1), (int)(at - SF(3)), num, PAL_TEXT_WEAK);
            }
        }
    }
    if (a->has_cursor_px)
    {
        int x = (int)floorf(crect.x + (a->cursor_px_x + 0.5f) * zs), y = (int)floorf(crect.y + (a->cursor_px_y + 0.5f) * zs);
        gfx_fill(d, rect_make(x, top.y, HAIR(), top.h), PAL_ACCENT);
        gfx_fill(d, rect_make(left.x, y, left.w, HAIR()), PAL_ACCENT);
    }
    d->clip = old;
}

/* ---- the canvas (Rust: paint_canvas) ---- */

static void paint_caret(app *a, surface *d, const text_box *tb, rect crect, float zs)
{
    const caret_layout *lay = &tb->layout;
    float line_h = lay->line_h * zs;
#define SX(px) (crect.x + (tb->pos.x + (px)) * zs)
#define SY(py) (crect.y + (tb->pos.y + (py)) * zs)
    if (text_cursor_has_selection(&tb->cursor))
    {
        int s0 = text_cursor_start(&tb->cursor), s1 = text_cursor_end(&tb->cursor), l0, l1, c;
        caret_line_col(lay, s0, &l0, &c);
        caret_line_col(lay, s1, &l1, &c);
        for (int l = l0; l <= l1; l++)
        {
            int from = l == l0 ? s0 : caret_index(lay, l, 0), to = l == l1 ? s1 : caret_index(lay, l, caret_line_len(lay, l) - 1);
            pt pa = caret_pos(lay, from), pb = caret_pos(lay, to);
            float x0 = SX(pa.x), y0 = SY(pa.y), x1 = SX(pb.x) + (l == l1 ? 0 : SF(4)), y1 = SY(pb.y) + line_h;
            gfx_fill(d, rect_make((int)roundf(x0), (int)roundf(y0), (int)roundf(x1 - x0), (int)roundf(y1 - y0)), COLOR_A(0x0078D7, 70));
        }
    }
    int index = tb->cursor.cursor + (tb->preedit ? utf8_count(tb->preedit) : 0);
    pt p = caret_pos(lay, index);
    if (app_caret_on(a))
        gfx_rrect(d, SX(p.x) - SF(0.5f), SY(p.y), SF(1.5f), line_h, 0, COLOR(0x000000));
#undef SX
#undef SY
    /* The caret blinks every 0.53 s */
    double since = os_now() - tb->blink;
    kit_repaint_after(0.53 - fmod(since < 0 ? 0 : since, 0.53) + 0.005);
}

/* Friends' pointers: their tool (or an arrow) in their colour, with their name (Rust: paint_friends) */
static void paint_friends(app *a, surface *d, rect crect, float zs)
{
    session *s = a->session;
    if (!s)
        return;
    font *f = ui_font(11.5f, 0);
    for (int i = 0; i < s->n_pointers; i++)
    {
        const pointer *p = &s->pointers[i];
        const kid *k = app_find_kid(a, p->id);
        if (!p->p.has_at || !k || a->now - p->at > 10)
            continue;
        float x = crect.x + p->p.x * zs, y = crect.y + p->p.y * zs;
        color c = RGB3(k->color[0], k->color[1], k->color[2]);
        switch (p->p.tool)
        {
        case HELD_PENCIL:
            tool_pointer(d, ICON_PENCIL, x, y);
            break;
        case HELD_FILL:
            tool_pointer(d, ICON_FILL, x, y);
            break;
        case HELD_BRUSH:
            tool_pointer(d, ICON_BRUSHES, x, y);
            break;
        case HELD_PICKER:
            tool_pointer(d, ICON_PICKER, x, y);
            break;
        case HELD_ERASER:
        {
            /* Their eraser's footprint, filled with what it paints, outlined in their colour */
            float side = fmaxf(p->p.size * zs, SF(4));
            rect r = rect_make((int)roundf(x - side / 2), (int)roundf(y - side / 2), (int)roundf(side), (int)roundf(side));
            gfx_fill(d, r, RGB3(p->p.color[0], p->p.color[1], p->p.color[2]));
            gfx_frame(d, rect_inset(r, -S(2)), S(2), c);
            break;
        }
        default:
        {
            static const float arrow[14] = { 0, 0, 0, 16, 4.2f, 12.2f, 7, 18, 9.4f, 17, 6.6f, 11.2f, 12, 11.2f };
            float pts[14];
            for (int j = 0; j < 7; j++)
                pts[2 * j] = x + SF(arrow[2 * j]), pts[2 * j + 1] = y + SF(arrow[2 * j + 1]);
            gfx_polygon(d, pts, 7, c);
            gfx_stroke(d, pts, 7, SF(1.2f), COLOR(0xFFFFFF), GFX_CLOSED);
            break;
        }
        }
        int tw = text_width(f, k->name, -1), th = font_height(f);
        rect tag = rect_make((int)roundf(x + SF(10)), (int)roundf(y + SF(16)), tw + S(12), th + S(5));
        gfx_round(d, tag, S(7), c);
        text_draw(d, f, tag.x + S(6), tag.y + (tag.h - th) / 2, k->name, -1, COLOR(0xFFFFFF));
    }
}

static void paint_canvas(app *a, surface *d, rect view, rect crect)
{
    float zs = a->zoom * a->ppp;
    gfx_shadow(d, crect, 0, S(4), S(22), PAL_CANVAS_SHADOW);
    rect vis = rect_cut(crect, view);
    if (!rect_empty(vis))
        gfx_canvas(d, vis, view_picture(a), (vis.x - crect.x) / (double)zs, (vis.y - crect.y) / (double)zs, zs);

    if (a->show_grid && !rect_empty(vis))
    {
        /* One device pixel wide, so the lines stay sharp at any display scale */
        float gap = app_grid_step(a->zoom) * zs;
        color gc = COLOR_A(0x808080, 110);
        for (int k = (int)floorf((vis.x - crect.x) / gap); crect.x + k * gap <= vis.x + vis.w; k++)
            gfx_fill(d, rect_make((int)floorf(crect.x + k * gap), vis.y, 1, vis.h), gc);
        for (int k = (int)floorf((vis.y - crect.y) / gap); crect.y + k * gap <= vis.y + vis.h; k++)
            gfx_fill(d, rect_make(vis.x, (int)floorf(crect.y + k * gap), vis.w, 1), gc);
    }

#define TX(px) (crect.x + (px) * zs)
#define TY(py) (crect.y + (py) * zs)
    if (a->sel)
    {
        selection *s = a->sel;
        if (s->lasso && s->n_lasso > 1)
        {
            float *pts = malloc(sizeof(float) * 2 * (size_t)s->n_lasso);
            for (int i = 0; i < s->n_lasso; i++)
                pts[2 * i] = TX(s->lasso[i].x + s->rect.x0), pts[2 * i + 1] = TY(s->lasso[i].y + s->rect.y0);
            ants(d, pts, s->n_lasso, 1);
            free(pts);
        }
        else
            ants_box(d, TX(s->rect.x0), TY(s->rect.y0), TX(s->rect.x1), TY(s->rect.y1));
    }
    const app_op *op = &a->op;
    if (op->kind == OP_SELECTING)
    {
        if (a->tool == TOOL_FREE_SELECT)
        {
            if (op->n_lasso > 1)
            {
                float *pts = malloc(sizeof(float) * 2 * (size_t)op->n_lasso);
                for (int i = 0; i < op->n_lasso; i++)
                    pts[2 * i] = TX(op->lasso[i].x), pts[2 * i + 1] = TY(op->lasso[i].y);
                ants(d, pts, op->n_lasso, 0);
                free(pts);
            }
        }
        else
            ants_box(d, TX(op->sel_start.x), TY(op->sel_start.y), TX(op->sel_end.x), TY(op->sel_end.y));
    }
    else if (op->kind == OP_RESIZE_CANVAS)
        ants_box(d, crect.x, crect.y, TX(op->size_w), TY(op->size_h));
    if (a->live && a->tool == TOOL_SHAPE)
    {
        if (!live_shape_is_line(a->live))
        {
            frect r = live_shape_rect(a->live);
            ants_box(d, TX(r.x0), TY(r.y0), TX(r.x1), TY(r.y1));
        }
        if (app_shows_shape_handles(a))
        {
            pt hs[8];
            int n = live_shape_handles(a->live, hs);
            for (int i = 0; i < n; i++)
                handle(d, TX(hs[i].x), TY(hs[i].y));
        }
    }
    if (a->text)
    {
        const text_box *tb = a->text;
        float e = SF(3), x0 = TX(tb->bounds.x0) - e, y0 = TY(tb->bounds.y0) - e, x1 = TX(tb->bounds.x1) + e, y1 = TY(tb->bounds.y1) + e;
        if (tb->editing)
            paint_caret(a, d, tb, crect, zs);
        ants_box(d, x0, y0, x1, y1);
        handle(d, x0, y0);
        handle(d, x1, y0);
        handle(d, x1, y1);
        handle(d, x0, y1);
    }
#undef TX
#undef TY

    /* The canvas resize handles, lit when hovered or dragged */
    for (int h = HANDLE_RIGHT; h <= HANDLE_CORNER; h++)
    {
        rect r = resize_handle(crect, h);
        int active = (op->kind == OP_RESIZE_CANVAS && op->handle == h) ||
                     (a->hover && op->kind == OP_IDLE && rect_has(rect_inset(r, -S(3)), (int)a->hover_x, (int)a->hover_y));
        gfx_round(d, r, S(2), COLOR(0xFFFFFF));
        gfx_round_frame(d, r, S(2), active ? (int)fmaxf(1, roundf(SF(1.5f))) : HAIR(), active ? PAL_ACCENT : COLOR(0x6E6E6E));
    }

    /* The brush's or eraser's footprint under the pointer */
    if (a->hover && rect_has(crect, (int)a->hover_x, (int)a->hover_y))
    {
        float hx = a->hover_x, hy = a->hover_y;
        if (a->tool == TOOL_BRUSH && a->size_brush * a->zoom >= 4)
        {
            float r = a->size_brush * zs * 0.5f;
            gfx_ring(d, hx, hy, r, SF(1), COLOR_A(0x000000, 160));
            gfx_ring(d, hx, hy, r + SF(1), SF(1), COLOR_A(0xFFFFFF, 160));
        }
        else if (a->tool == TOOL_ERASER)
        {
            float s = fmaxf(a->size_eraser * zs, SF(3));
            rect r = rect_make((int)roundf(hx - s / 2), (int)roundf(hy - s / 2), (int)roundf(s), (int)roundf(s));
            gfx_fill(d, r, view_rgba(a->secondary));
            gfx_frame(d, rect_inset(r, -HAIR()), HAIR(), COLOR_A(0x000000, 160));
        }
    }
    paint_friends(a, d, crect, zs);
    if (a->tool_icon >= 0)
        tool_pointer(d, a->tool_icon, a->tool_x, a->tool_y);
}

/* ---- the right-click menu of the selection tools (Rust: selection_menu) ---- */

static void selection_menu(app *a)
{
    int has_sel = a->sel != NULL;
    if (rb_menu_item(ICON_CUT, 0, "Cut", 0, has_sel).clicked)
        app_cut(a), kit_popup_close();
    if (rb_menu_item(ICON_COPY, 0, "Copy", 0, 1).clicked)
        app_copy(a), kit_popup_close();
    if (rb_menu_item(ICON_PASTE, 0, "Paste", 0, 1).clicked)
        app_paste(a), kit_popup_close();
    kit_separator();
    if (rb_menu_item(ICON_SELECT_ALL, 0, "Select all", 0, 1).clicked)
        app_select_all(a), kit_popup_close();
    if (rb_menu_item(ICON_CROP, 0, "Crop", 0, has_sel).clicked)
        app_crop(a), kit_popup_close();
    if (rb_menu_item(ICON_DELETE, 0, "Delete", 0, has_sel).clicked)
        app_delete_selection(a), kit_popup_close();
    kit_separator();
    kit_resp rotate = rb_menu_item(-1, 0, "Rotate  \xE2\x96\xB8", 0, 1);
    if (kit_popup_menu(&rotate, 200, 0))
    {
        view_rotate_items(a);
        kit_popup_end();
    }
    if (rb_menu_item(ICON_RESIZE, 0, "Resize", 0, 1).clicked)
        app_open_resize(a), kit_popup_close();
    if (rb_menu_item(-1, 0, "Invert color", 0, 1).clicked)
        app_invert(a), kit_popup_close();
}

/* ---- the scroll bars (egui's solid ones, 12 points) ---- */

static void scroll_bar(app *a, rect track, int vertical, float view, float content)
{
    static int grab;
    float offset = vertical ? a->scroll_y : a->scroll_x, range = content - view;
    int len = vertical ? track.h : track.w;
    int thumb = content > 0 ? (int)(len * view / content) : len;
    thumb = thumb < S(16) ? S(16) : thumb > len ? len : thumb;
    int room = len - thumb, at = range > 0 ? (int)(room * offset / range) : 0;
    rect t = vertical ? rect_make(track.x + 2, track.y + at, track.w - 4, thumb) : rect_make(track.x + at, track.y + 2, thumb, track.h - 4);
    surface *d = kit_dst();
    gfx_fill(d, track, COLOR(0xF0F0F0));
    kit_resp r = kit_interact(t, kit_id_of(vertical ? "canvas-vbar" : "canvas-hbar"), SENSE_DRAG);
    kit_resp tr = kit_interact(track, kit_id_of(vertical ? "canvas-vtrack" : "canvas-htrack"), SENSE_CLICK);
    const kit_input *in = kit_in();
    float want = offset;
    if (r.drag_started)
        grab = (vertical ? in->y : in->x) - at;
    if (r.dragging && room > 0)
    {
        int pos = (vertical ? in->y : in->x) - grab;
        want = range * (pos < 0 ? 0 : pos > room ? room : pos) / room;
    }
    if (tr.clicked)
        want += (vertical ? tr.y < t.y : tr.x < t.x) ? -view * 0.9f : view * 0.9f;
    if (want != offset)
    {
        app_scroll_to(a, vertical ? a->scroll_x : want, vertical ? want : a->scroll_y);
        kit_repaint();
    }
    gfx_fill(d, t, r.held ? COLOR(0x787878) : r.hovered ? COLOR(0xA8A8A8) : COLOR(0xC1C1C1));
}

/* ---- the whole area (Rust: canvas) ---- */

void view_canvas(app *a, rect full)
{
    surface *d = kit_dst();
    gfx_fill(d, full, PAL_WORKSPACE);
    int ruler = a->show_rulers ? S(APP_RULER) : 0, bw = S(SCROLL_BAR_W);
    rect panel = rect_make(full.x + ruler, full.y + ruler, full.w - ruler, full.h - ruler);
    if (panel.w < 1 || panel.h < 1)
        return;
    /* The scroll bars take room from the view when the picture doesn't fit */
    app_set_view(a, panel);
    float cw, ch;
    app_content_size(a, &cw, &ch);
    int bar_h = cw > panel.w, bar_v = ch > panel.h - (bar_h ? bw : 0);
    bar_h = cw > panel.w - (bar_v ? bw : 0);
    rect view = rect_make(panel.x, panel.y, panel.w - (bar_v ? bw : 0), panel.h - (bar_h ? bw : 0));
    app_set_view(a, view);
    app_content_size(a, &cw, &ch);
    V.view = view;
    if (bar_v)
        scroll_bar(a, rect_make(view.x + view.w, view.y, bw, view.h), 1, view.h, ch);
    if (bar_h)
        scroll_bar(a, rect_make(view.x, view.y + view.h, view.w, bw), 0, view.w, cw);
    if (bar_v && bar_h)
        gfx_fill(d, rect_make(view.x + view.w, view.y + view.h, bw, bw), COLOR(0xF0F0F0));

    kit_resp cr = kit_interact(view, kit_id_of("canvas"), SENSE_DRAG);
    if (V.canvas_hover || V.canvas_btns)
        kit_set_cursor(a->cursor);

    app_render_live_shape(a);
    app_update_text_preview(a);
    rect crect = app_canvas_rect(a);
    rect old = gfx_clip(d, view);
    paint_canvas(a, d, view, crect);
    d->clip = old;
    if (a->show_rulers)
        rulers(a, full, panel, crect);

    /* The selection tools' right-click menu, which the app asks for on the release */
    cr.right_clicked = a->menu_open;
    a->menu_open = 0;
    if (kit_context_menu(&cr, 200))
    {
        selection_menu(a);
        kit_popup_end();
    }

    float cx, cy, chh;
    if (a->win && app_caret_rect(a, &cx, &cy, &chh))
        host_ime_spot(a->win, (int)cx, (int)cy, (int)chh);
}

/* ---- View > Full screen: just the picture; a click returns (Rust: full_screen_view) ---- */

void view_full_screen(app *a, rect full)
{
    surface *d = kit_dst();
    gfx_fill(d, full, PAL_WORKSPACE);
    const img *pic = view_picture(a);
    double zs = fmin(fmin(full.w / (double)pic->w, full.h / (double)pic->h), a->ppp);
    int w = (int)(pic->w * zs), h = (int)(pic->h * zs);
    rect r = rect_make(full.x + (full.w - w) / 2, full.y + (full.h - h) / 2, w, h);
    gfx_canvas(d, r, pic, 0, 0, zs);
    if (kit_interact(full, kit_id_of("full-screen"), SENSE_CLICK).clicked)
        app_set_full_screen(a, 0);
}
