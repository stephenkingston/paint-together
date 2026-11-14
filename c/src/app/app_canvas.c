/*
 * Paint (C port) - the canvas in screen coordinates: scrolling, zoom about
 * the pointer, the resize handles and the pointer shapes (Rust: app.rs's
 * canvas, canvas_input and hover_cursor)
 *
 * The picture sits in a scroll area, APP_MARGIN points from its content's
 * top-left corner. Screen positions are device pixels; one picture pixel is
 * zoom * ppp of them.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "app/app_internal.h"
#include "ui/icons.h"

/* The picture's screen rectangle, unrounded */
static frect crect(const app *a)
{
    float m = APP_MARGIN * a->ppp, eff = a->zoom * a->ppp;
    float x = (float)a->view.x + m - a->scroll_x, y = (float)a->view.y + m - a->scroll_y;
    frect r = { x, y, x + (float)a->img->w * eff, y + (float)a->img->h * eff };
    return r;
}

static int frect_in(frect r, float x, float y)
{
    return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
}

rect app_canvas_rect(const app *a)
{
    frect c = crect(a);
    rect r = { (int)floorf(c.x0 + 0.5f), (int)floorf(c.y0 + 0.5f), (int)floorf(c.x1 - c.x0 + 0.5f), (int)floorf(c.y1 - c.y0 + 0.5f) };
    return r;
}

void app_content_size(const app *a, float *w, float *h)
{
    float eff = a->zoom * a->ppp, extra = (APP_MARGIN * 2.0f + 8.0f) * a->ppp;
    *w = fmaxf((float)a->img->w * eff + extra, (float)a->view.w);
    *h = fmaxf((float)a->img->h * eff + extra, (float)a->view.h);
}

void app_scroll_to(app *a, float x, float y)
{
    float w, h;
    app_content_size(a, &w, &h);
    a->scroll_x = fmaxf(0.0f, fminf(x, w - (float)a->view.w));
    a->scroll_y = fmaxf(0.0f, fminf(y, h - (float)a->view.h));
}

void app_set_view(app *a, rect view)
{
    a->view = view;
    if (a->fit_pending && view.w / a->ppp > 64.0f)
    {
        a->fit_pending = 0;
        if ((float)a->img->w + 2.0f * APP_MARGIN > view.w / a->ppp || (float)a->img->h + 2.0f * APP_MARGIN > view.h / a->ppp)
            app_zoom_fit(a);
    }
    app_scroll_to(a, a->scroll_x, a->scroll_y);
}

int app_grid_step(float zoom)
{
    if (zoom >= 4.0f)
        return 1;
    int step = 8;
    while ((float)step * zoom < 8.0f)
        step *= 2;
    return step;
}

pt app_tool_tip(int icon)
{
    switch (icon)
    {
    case ICON_PENCIL:
    case ICON_PICKER:
        return pt_make(2.5f, 17.5f);
    case ICON_BRUSHES:
        return pt_make(3.0f, 18.4f);
    default:
        return pt_make(17.9f, 18.6f);   /* the drop of paint pouring from the bucket */
    }
}

/* ---- the canvas resize handles ---- */

static int hit_handle(const app *a, frect c, float x, float y)
{
    float s = 3.5f * a->ppp + 3.0f * a->ppp, off = 4.0f * a->ppp;
    pt centre[3] = { { c.x1 + off, (c.y0 + c.y1) * 0.5f }, { (c.x0 + c.x1) * 0.5f, c.y1 + off }, { c.x1 + off, c.y1 + off } };
    for (int h = 0; h < 3; h++)
        if (fabsf(x - centre[h].x) <= s && fabsf(y - centre[h].y) <= s)
            return h;
    return HANDLE_NONE;
}

static int handle_cursor(int h)
{
    return h == HANDLE_RIGHT ? CURSOR_SIZE_EW : h == HANDLE_BOTTOM ? CURSOR_SIZE_NS : CURSOR_SIZE_NWSE;
}

static int text_handle_cursor(int h)
{
    return h % 2 == 0 ? CURSOR_SIZE_NWSE : CURSOR_SIZE_NESW;
}

static int shape_cursor(int c)
{
    static const int map[] = { CURSOR_CROSS, CURSOR_SIZE_NWSE, CURSOR_SIZE_NESW, CURSOR_SIZE_NS, CURSOR_SIZE_EW };
    return map[c];
}

/* The pointer while nothing is being dragged (Rust's hover_cursor) */
static int hover_cursor(app *a, float x, float y)
{
    frect c = crect(a);
    float eff = a->zoom * a->ppp, tol = 5.0f / a->zoom;
    pt p = pt_make((x - c.x0) / eff, (y - c.y0) / eff);
    if (a->tool == TOOL_TEXT && a->text)
    {
        int h = app_text_handle_at(a, p);
        if (h >= 0)
            return text_handle_cursor(h);
        frect b = a->text->bounds;
        if (p.x >= b.x0 - tol && p.x <= b.x1 + tol && p.y >= b.y0 - tol && p.y <= b.y1 + tol)
            return CURSOR_MOVE;
    }
    if (a->tool == TOOL_SHAPE && a->live)
    {
        int h = live_shape_handle_at(a->live, p, tol);
        if (h >= 0)
            return shape_cursor(live_shape_handle_cursor(live_shape_is_line(a->live), h));
        if (live_shape_contains(a->live, p, tol))
            return CURSOR_MOVE;
    }
    int handle = hit_handle(a, c, x, y);
    if (handle != HANDLE_NONE)
        return handle_cursor(handle);
    if (!frect_in(c, x, y))
        return CURSOR_ARROW;
    int icon = app_tool_pointer(a->tool);
    if (icon >= 0)
    {
        a->tool_icon = icon;
        a->tool_x = x;
        a->tool_y = y;
        return CURSOR_NONE;
    }
    /* The eraser's footprint (drawn with the canvas) is the pointer */
    if (a->tool == TOOL_ERASER)
        return CURSOR_NONE;
    if ((a->tool == TOOL_SELECT || a->tool == TOOL_FREE_SELECT) && a->sel && irect_contains(a->sel->rect, p.x, p.y))
        return CURSOR_MOVE;
    return a->tool == TOOL_TEXT ? CURSOR_TEXT : CURSOR_CROSS;  /* the magnifier too: there is no zoom pointer */
}

/* The pointer while dragging */
static int drag_cursor(app *a, float x, float y)
{
    if (a->tool_icon >= 0)
        return CURSOR_NONE;
    switch (a->op.kind)
    {
    case OP_STROKE:
        return a->op.pen == PEN_ERASER && frect_in(crect(a), x, y) ? CURSOR_NONE : CURSOR_CROSS;
    case OP_MOVING_SEL:
    case OP_SHAPE_MOVE:
    case OP_TEXT_MOVE:
    case OP_PAN:
        return CURSOR_MOVE;
    case OP_SHAPE_HANDLE:
        return shape_cursor(live_shape_handle_cursor(a->shape == SHAPE_LINE, a->op.handle));
    case OP_TEXT_SCALE:
        return text_handle_cursor(a->op.handle);
    case OP_RESIZE_CANVAS:
        return handle_cursor(a->op.handle);
    default:
        return CURSOR_CROSS;
    }
}

int app_canvas_event(app *a, const ui_event *ev)
{
    if (ev->time > 0.0)
        a->now = ev->time;
    float x = (float)ev->x, y = (float)ev->y;
    frect c = crect(a);
    float eff = a->zoom * a->ppp;
    pt p = pt_make((x - c.x0) / eff, (y - c.y0) / eff);
    int w = a->img->w, h = a->img->h;
    int in_view = x >= a->view.x && y >= a->view.y && x < a->view.x + a->view.w && y < a->view.y + a->view.h;
    a->tool_icon = -1;

    if (ev->type == EV_LEAVE)
    {
        a->hover = a->has_cursor_px = 0;
        if (a->op.kind == OP_IDLE)
            a->has_pointer = 0;
        a->cursor = CURSOR_ARROW;
        return 1;
    }
    if (ev->type == EV_WHEEL)
    {
        if (ev->mods & KMOD_CTRL)
        {
            /* Ctrl+wheel (or a pinch) zooms around the pointer */
            pt anchor = pt_make(x - (float)a->view.x, y - (float)a->view.y);
            if (in_view)
                app_zoom_at(a, a->zoom * expf((ev->wheel_y + ev->wheel_x) * 40.0f / 200.0f), &anchor);
        }
        else
        {
            float dx = ev->wheel_x * 40.0f * a->ppp, dy = ev->wheel_y * 40.0f * a->ppp;
            if ((ev->mods & KMOD_SHIFT) && dx == 0.0f)
            {
                dx = dy;
                dy = 0.0f;
            }
            app_scroll_to(a, a->scroll_x - dx, a->scroll_y - dy);
        }
        return 1;
    }
    if (ev->type != EV_MOVE && ev->type != EV_DOWN && ev->type != EV_UP)
        return 0;

    /* Where the pointer is, for the status bar and for friends */
    int busy = a->op.kind != OP_IDLE;
    a->hover = in_view;
    a->hover_x = x;
    a->hover_y = y;
    a->has_pointer = (in_view || busy) && p.x >= 0.0f && p.y >= 0.0f && p.x <= (float)w && p.y <= (float)h;
    a->pointer = p;
    a->has_cursor_px = in_view && p.x >= 0.0f && p.y >= 0.0f && p.x < (float)w && p.y < (float)h;
    a->cursor_px_x = (int)p.x;
    a->cursor_px_y = (int)p.y;

    if (!busy)
    {
        a->cursor = in_view ? hover_cursor(a, x, y) : CURSOR_ARROW;
        if (ev->type == EV_UP)
        {
            /* The selection tools' right-click menu opens on the release */
            if (a->right_menu_ && ev->button == BTN_RIGHT)
            {
                a->menu_open = 1;
                a->menu_x = ev->x;
                a->menu_y = ev->y;
            }
            a->right_menu_ = 0;
            return 1;
        }
        if (ev->type != EV_DOWN || !in_view)
            return 1;
        pt anchor = pt_make(x - (float)a->view.x, y - (float)a->view.y);
        int selecting = a->tool == TOOL_SELECT || a->tool == TOOL_FREE_SELECT;
        int handle;
        if (ev->button == BTN_MIDDLE)
        {
            app_op_clear(a);
            a->op.kind = OP_PAN;
            a->op.button = BTN_MIDDLE;
            a->op.pan_x = x;
            a->op.pan_y = y;
        }
        else if (app_live_shape_hit(a, p) || app_text_hit(a, p))
            app_begin(a, p, ev->button, anchor);
        else if ((handle = hit_handle(a, c, x, y)) != HANDLE_NONE)
        {
            app_commit_all(a);
            app_op_clear(a);
            a->op.kind = OP_RESIZE_CANVAS;
            a->op.button = ev->button;
            a->op.handle = handle;
            a->op.size_w = a->img->w;
            a->op.size_h = a->img->h;
        }
        else if (selecting && ev->button == BTN_RIGHT)
            a->right_menu_ = 1;
        else if (frect_in(c, x, y) || selecting)
            app_begin(a, p, ev->button, anchor);
        a->has_last_pt_ = 1;
        a->last_pt_ = p;
        a->last_spray_ = a->now;
        if (a->op.kind != OP_IDLE)
            a->cursor = drag_cursor(a, x, y);
        return 1;
    }

    if (a->op.kind == OP_PAN)
    {
        app_scroll_to(a, a->scroll_x - (x - a->op.pan_x), a->scroll_y - (y - a->op.pan_y));
        a->op.pan_x = x;
        a->op.pan_y = y;
    }
    else if (ev->type != EV_DOWN)
    {
        float dt = (float)(a->now - a->last_spray_);
        dt = dt < 0.0f ? 0.0f : dt > 0.1f ? 0.1f : dt;
        a->last_spray_ = a->now;
        a->has_last_pt_ = 1;
        a->last_pt_ = p;
        app_update(a, &p, 1, (ev->mods & KMOD_SHIFT) != 0, dt);
    }
    if (a->op.kind == OP_STROKE && app_tool_pointer(a->tool) >= 0)
    {
        a->tool_icon = app_tool_pointer(a->tool);
        a->tool_x = x;
        a->tool_y = y;
    }
    a->cursor = drag_cursor(a, x, y);
    if (ev->type == EV_UP && ev->button == a->op.button)
    {
        app_end(a);
        a->has_last_pt_ = 0;
        a->tool_icon = -1;
        a->cursor = in_view ? hover_cursor(a, x, y) : CURSOR_ARROW;
    }
    return 1;
}

int app_take_dirty(app *a, irect *out)
{
    irect all = irect_of(a->img);
    a->preview_dirty = a->dirty_full ? all : irect_union(a->preview_dirty, a->dirty);
    /* Friends' drawings in progress are part of the picture on screen */
    if (a->session)
    {
        a->dirty = irect_union(a->dirty, a->session->overlay_dirty);
        a->session->overlay_dirty = IRECT_EMPTY;
    }
    *out = a->dirty_full ? all : irect_intersect(a->dirty, all);
    a->dirty_full = 0;
    a->dirty = IRECT_EMPTY;
    return !irect_empty(*out);
}

const img *app_friends_overlay(const app *a)
{
    const session *s = a->session;
    if (!s || !session_has_previews(s) || !s->overlay || s->overlay->w != a->img->w || s->overlay->h != a->img->h)
        return NULL;
    return s->overlay;
}

void app_with_friends(const img *im, const img *overlay, irect r, uint8_t *out)
{
    int rw = irect_w(r);
    for (int y = r.y0; y < r.y1; y++, out += (size_t)rw * 4)
    {
        memcpy(out, img_at(im, r.x0, y), (size_t)rw * 4);
        if (overlay && overlay->w == im->w && overlay->h == im->h)
            for (int x = 0; x < rw; x++)
            {
                const uint8_t *o = img_at(overlay, r.x0 + x, y);
                if (o[3])
                    memcpy(out + x * 4, o, 4);
            }
    }
}

int app_caret_rect(const app *a, float *x, float *y, float *h)
{
    const text_box *tb = a->text;
    if (!tb || !tb->editing)
        return 0;
    frect c = crect(a);
    float eff = a->zoom * a->ppp;
    pt at = caret_pos(&tb->layout, tb->cursor.cursor + utf8_count(tb->preedit));
    *x = c.x0 + (tb->pos.x + at.x) * eff;
    *y = c.y0 + (tb->pos.y + at.y) * eff;
    *h = tb->layout.line_h * eff;
    return 1;
}
