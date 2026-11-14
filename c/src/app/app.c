/*
 * Paint (C port) - the app's state and every tool's behaviour (Rust: app.rs, PaintApp)
 *
 * Drawing tools never paint straight onto the picture: an operation keeps
 * what it needs to redraw itself from scratch (a snapshot, a coverage mask,
 * the live shape's or text box's own snapshot) until it is finished, and is
 * then recorded as one undo step. Those buffers are freed as soon as the
 * operation ends.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/app_internal.h"
#include "collab/names.h"
#include "paint/transform.h"
#include "ui/icons.h"

const uint8_t APP_PALETTE[20][3] = {
    { 0, 0, 0 },       { 127, 127, 127 }, { 136, 0, 21 },    { 237, 28, 36 },   { 255, 127, 39 },
    { 255, 242, 0 },   { 34, 177, 76 },   { 0, 162, 232 },   { 63, 72, 204 },   { 163, 73, 164 },
    { 255, 255, 255 }, { 195, 195, 195 }, { 185, 122, 87 },  { 255, 174, 201 }, { 255, 201, 14 },
    { 239, 228, 176 }, { 181, 230, 29 },  { 153, 217, 234 }, { 112, 146, 190 }, { 200, 191, 231 },
};
const char *const APP_PALETTE_NAMES[20] = {
    "Black", "Gray-50%", "Dark red", "Red", "Orange", "Yellow", "Green", "Turquoise", "Indigo", "Purple",
    "White", "Gray-25%", "Brown", "Rose", "Gold", "Light yellow", "Lime", "Light turquoise", "Blue-gray", "Lavender",
};
/* The basic colours of the Windows colour dialog */
const uint32_t APP_BASIC_COLORS[48] = {
    0xFF8080, 0xFFFF80, 0x80FF80, 0x00FF80, 0x80FFFF, 0x0080FF, 0xFF80C0, 0xFF80FF,
    0xFF0000, 0xFFFF00, 0x80FF00, 0x00FF40, 0x00FFFF, 0x0080C0, 0x8080C0, 0xFF00FF,
    0x804040, 0xFF8040, 0x00FF00, 0x008080, 0x004080, 0x8080FF, 0x800040, 0xFF0080,
    0x800000, 0xFF8000, 0x008000, 0x008040, 0x0000FF, 0x0000A0, 0x800080, 0x8000FF,
    0x400000, 0x804000, 0x004000, 0x004040, 0x000080, 0x000040, 0x400040, 0x400080,
    0x000000, 0x808000, 0x808040, 0x808080, 0x408080, 0xC0C0C0, 0x400040, 0xFFFFFF,
};
const float APP_TEXT_SIZES[16] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 };
static const float ZOOM_STEPS[16] = { 0.125f, 0.25f, 0.33f, 0.5f, 0.66f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f };
static const rgba WHITE = { 255, 255, 255, 255 }, BLACK = { 0, 0, 0, 255 };

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(malloc(n), s, n);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int is_selection(app_tool t)
{
    return t == TOOL_SELECT || t == TOOL_FREE_SELECT;
}

void pending_set(pending *p, int kind, const char *arg)
{
    free(p->arg);
    p->kind = kind;
    p->arg = arg ? dup_str(arg) : NULL;
}

void pending_clear(pending *p)
{
    pending_set(p, PENDING_NONE, NULL);
}

void app_set_error(app *a, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    free(a->error);
    a->error = dup_str(buf);
}

/* ---------------------------------------------------------------- lifetime */

app *app_new(void)
{
    app *a = calloc(1, sizeof *a);
    a->ppp = 1.0f;
    a->img = img_new(APP_DEFAULT_W, APP_DEFAULT_H, WHITE);
    a->history = history_new();
    a->tool = a->prev_tool = TOOL_PENCIL;
    a->select_mode = TOOL_SELECT;
    a->brush = BRUSH_BRUSH;
    a->shape = SHAPE_RECTANGLE;
    a->shape_outline = 1;
    a->size_pencil = 1.0f;
    a->size_brush = 6.0f;
    a->size_eraser = 12.0f;
    a->size_shape = 3.0f;
    a->fill_tolerance = 10.0f;
    a->primary = BLACK;
    a->secondary = WHITE;
    a->text_style = (text_style){ -1, 14.0f, 0, 0, 0, 0, 0 };
    a->rng = 0x9E3779B97F4A7C15ull;
    a->zoom = 1.0f;
    a->show_status = 1;
    a->dirty = a->preview_dirty = IRECT_EMPTY;
    a->dirty_full = 1;
    a->cursor = CURSOR_ARROW;
    a->tool_icon = -1;
    a->last_share = -1e300;
    a->handover.spare = OS_BAD_SOCK;
    memcpy(a->me_color, KID_COLORS[0], 3);
    app_load_recent(a);
    return a;
}

void app_free(app *a)
{
    if (!a)
        return;
    app_op_clear(a);
    app_selection_free(a->sel);
    app_text_box_free(a->text);
    live_shape_free(a->live);
    link_free(a->link);
    session_free(a->session);
    discovery_free(a->discovery);
    app_handover_clear(a);
    history_free(a->history);
    img_free(a->img);
    free(a->path);
    for (int i = 0; i < a->n_recent; i++)
        free(a->recent[i]);
    pending_clear(&a->confirm);
    pending_clear(&a->confirm_everyone);
    pending_clear(&a->ask_then);
    free(a->resize);
    free(a->error);
    free(a->join_dialog);
    free(a->asking);
    free(a->marks);
    free(a);
}

void app_set_scale(app *a, float ppp)
{
    a->ppp = ppp > 0.0f ? ppp : 1.0f;
}

void app_text_family(app *a)
{
    if (a->text_style.family < 0)
        a->text_style.family = fonts_default_family();
}

/* ---------------------------------------------------------------- document */

void app_mark(app *a, irect r)
{
    a->dirty = irect_union(a->dirty, r);
}

void app_record(app *a, change *c)
{
    if (!c)
        return;
    session *s = a->session;
    if (!s)
    {
        history_push(a->history, c);
        return;
    }
    int replace = c->kind == CHANGE_REPLACE;
    edit e;
    if (edit_from_change(c, &e))
        session_commit(s, e);
    history_push(a->history, c);
    if (replace)
        app_mark_set(a, history_state_id(a->history), s->foreign);
}

void app_push_patch(app *a, const img *before, irect r)
{
    app_record(a, change_patch(before, a->img, r));
}

/* A new picture replaced the old one as one undo step */
static void record_replace(app *a, img *before)
{
    app_record(a, change_replace(before, a->img));
    a->dirty_full = 1;
}

static int blank_text(const char *t)
{
    for (; *t; t++)
        if (*t != ' ' && *t != '\t' && *t != '\n' && *t != '\r')
            return 0;
    return 1;
}

int app_is_dirty(const app *a)
{
    int changed = a->session ? a->session->version != a->saved_version : history_state_id(a->history) != a->saved_state;
    return changed || (a->sel && a->sel->fl) || (a->text && !blank_text(a->text->text)) || a->live;
}

const char *app_doc_name(const app *a, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s", a->path ? os_path_base(a->path) : "Untitled");
    return buf;
}

const char *app_title(app *a)
{
    char name[256];
    snprintf(a->title_, sizeof a->title_, "%s%s - Paint%s", app_is_dirty(a) ? "*" : "", app_doc_name(a, name, sizeof name),
             a->session ? " (drawing together)" : "");
    return a->title_;
}

static void reset_view(app *a)
{
    a->dirty_full = 1;
    a->zoom = 1.0f;
    a->fit_pending = 1;
    a->scroll_x = a->scroll_y = 0.0f;
}

void app_load_image(app *a, img *im)
{
    if (a->session)
    {
        /* Drawing together: the new picture replaces the shared one, for everyone */
        app_settle(a);
        app_selection_free(a->sel);
        a->sel = NULL;
        img *before = a->img;
        a->img = im;
        record_replace(a, before);
        reset_view(a);
        return;
    }
    app_op_clear(a);
    app_selection_free(a->sel);
    a->sel = NULL;
    app_text_box_free(a->text);
    a->text = NULL;
    live_shape_free(a->live);
    a->live = NULL;
    img_free(a->img);
    a->img = im;
    history_clear(a->history);
    a->saved_state = history_state_id(a->history);
    reset_view(a);
}

void app_undo(app *a)
{
    if (a->session)
    {
        app_step_mine(a, 1);
        return;
    }
    app_commit_all(a);
    if (history_undo(a->history, &a->img))
        a->dirty_full = 1;
}

void app_redo(app *a)
{
    if (a->session)
    {
        app_step_mine(a, 0);
        return;
    }
    app_commit_all(a);
    if (history_redo(a->history, &a->img))
        a->dirty_full = 1;
}

void app_settle(app *a)
{
    app_end(a);
    app_commit_all(a);
}

/* ---------------------------------------------------------------- tools & colours */

void app_set_tool(app *a, app_tool tool)
{
    if (tool == a->tool)
        return;
    if (a->tool == TOOL_TEXT)
        app_commit_text(a);
    if (is_selection(a->tool) && !is_selection(tool))
        app_commit_selection(a);
    if (a->tool == TOOL_SHAPE)
        app_commit_shape(a);
    if (tool == TOOL_PICKER)
        a->prev_tool = a->tool;
    a->tool = tool;
    if (tool == TOOL_TEXT)
    {
        app_text_family(a);
        a->tab = TAB_TEXT;
    }
    else if (a->tab == TAB_TEXT)
        a->tab = TAB_HOME;
}

void app_pick_brush(app *a, brush_kind k)
{
    a->brush = k;
    app_set_tool(a, TOOL_BRUSH);
}

void app_set_select_mode(app *a, app_tool mode)
{
    a->select_mode = mode;
    app_set_tool(a, mode);
}

void app_set_shape_outline(app *a, int on)
{
    a->shape_outline = on;
    app_render_live_shape(a);
}

void app_set_shape_fill(app *a, int on)
{
    a->shape_fill = on;
    app_render_live_shape(a);
}

float *app_tool_size(app *a, const float **presets, float *max)
{
    static const float pencil[4] = { 1, 2, 3, 4 }, eraser[4] = { 4, 6, 8, 10 }, other[4] = { 1, 3, 5, 8 };
    switch (a->tool)
    {
    case TOOL_FILL:
        *presets = NULL;
        *max = 100.0f;
        return NULL;
    case TOOL_PENCIL:
        *presets = pencil;
        *max = 20.0f;
        return &a->size_pencil;
    case TOOL_ERASER:
        *presets = eraser;
        *max = 100.0f;
        return &a->size_eraser;
    case TOOL_SHAPE:
        *presets = other;
        *max = 50.0f;
        return &a->size_shape;
    default:
        *presets = other;
        *max = 100.0f;
        return &a->size_brush;
    }
}

rgba app_active_color(const app *a)
{
    return a->editing_secondary ? a->secondary : a->primary;
}

void app_set_active_color(app *a, rgba c)
{
    if (a->editing_secondary)
        a->secondary = c;
    else
        a->primary = c;
}

void app_set_text_style(app *a, text_style st)
{
    a->text_style = st;
    app_update_text_preview(a);
}

void app_commit_all(app *a)
{
    app_commit_text(a);
    app_commit_selection(a);
    app_commit_shape(a);
}

int app_tool_pointer(app_tool tool)
{
    switch (tool)
    {
    case TOOL_PENCIL:
        return ICON_PENCIL;
    case TOOL_FILL:
        return ICON_FILL;
    case TOOL_BRUSH:
        return ICON_BRUSHES;
    case TOOL_PICKER:
        return ICON_PICKER;
    default:
        return -1;
    }
}

/* ---------------------------------------------------------------- live shape */

static shape_style style_of(const app *a, int swapped)
{
    shape_style st = { a->shape_outline, a->shape_fill, a->size_shape, a->primary, a->secondary };
    if (swapped)
    {
        st.line_color = a->secondary;
        st.fill_color = a->primary;
    }
    return st;
}

void app_render_live_shape(app *a)
{
    if (a->live)
        app_mark(a, live_shape_render(a->live, a->img, style_of(a, a->live->swapped)));
}

/* Stamp the live shape onto the picture as one undo step */
void app_commit_shape(app *a)
{
    app_render_live_shape(a);
    live_shape *live = a->live;
    if (!live)
        return;
    a->live = NULL;
    app_push_patch(a, live->snapshot, live_shape_touched(live));
    live_shape_free(live);
}

void app_pick_shape(app *a, shape_kind k)
{
    app_commit_shape(a);
    a->shape = k;
    app_set_tool(a, TOOL_SHAPE);
}

void app_discard_shape(app *a)
{
    if (!a->live)
        return;
    app_mark(a, live_shape_erase(a->live, a->img));
    live_shape_free(a->live);
    a->live = NULL;
}

int app_shows_shape_handles(const app *a)
{
    return a->live && a->tool == TOOL_SHAPE;
}

/* ---------------------------------------------------------------- text box */

void app_text_box_free(text_box *t)
{
    if (!t)
        return;
    free(t->text);
    img_free(t->snapshot);
    mask_free(t->mask);
    text_cursor_free(&t->cursor);
    caret_layout_free(&t->layout);
    free(t->preedit);
    free(t->r_text);
    free(t);
}

static int style_eq(const text_style *x, const text_style *y)
{
    return x->family == y->family && x->size_pt == y->size_pt && x->bold == y->bold && x->italic == y->italic
        && x->underline == y->underline && x->strike == y->strike && x->opaque == y->opaque;
}

static frect frect_xywh(float x, float y, float w, float h)
{
    frect r = { x, y, x + w, y + h };
    return r;
}

void app_update_text_preview(app *a)
{
    text_box *tb = a->text;
    if (!tb)
        return;
    app_text_family(a);
    text_style st = a->text_style;
    rgba fg = a->primary, bg = a->secondary;
    char *shown;
    if (!*tb->preedit)
        shown = dup_str(tb->text);
    else
    {
        size_t at = utf8_byte(tb->text, tb->cursor.cursor), n = strlen(tb->text), p = strlen(tb->preedit);
        shown = malloc(n + p + 1);
        memcpy(shown, tb->text, at);
        memcpy(shown + at, tb->preedit, p);
        memcpy(shown + at + p, tb->text + at, n - at + 1);
    }
    if (tb->rendered && !strcmp(tb->r_text, shown) && style_eq(&tb->r_style, &st) && rgba_eq(tb->r_fg, fg) && rgba_eq(tb->r_bg, bg)
        && tb->r_pos.x == tb->pos.x && tb->r_pos.y == tb->pos.y)
    {
        free(shown);
        return;
    }
    paint_font font;
    if (fonts_load(st.family, st.bold, st.italic, &font) != 0)
    {
        free(shown);
        return;
    }
    float px = text_style_px(&st);
    text_size size = ptext_measure(&font, px, shown);
    caret_layout_build(&tb->layout, &font, px, shown);
    tb->bounds = frect_xywh(tb->pos.x, tb->pos.y, fmaxf(size.width, px * 0.5f), size.height);
    mask_clear(tb->mask, tb->dirty);
    irect r = ptext_render(tb->mask, &font, px, shown, tb->pos, st.underline, st.strike);
    int opaque = st.opaque && *shown;
    irect bg_rect = irect_around(tb->bounds.x0, tb->bounds.y0, tb->bounds.x1, tb->bounds.y1, 1.0f);
    if (opaque)
        r = irect_union(r, bg_rect);
    irect region = irect_union(tb->dirty, r);
    raster_copy_rect(a->img, tb->snapshot, region);
    if (opaque)
        raster_fill_rect(a->img, bg_rect, bg);
    raster_blend_mask(a->img, tb->mask, fg, 1.0f, r);
    tb->dirty = r;
    free(tb->r_text);
    tb->r_text = shown;
    tb->rendered = 1;
    tb->r_style = st;
    tb->r_fg = fg;
    tb->r_bg = bg;
    tb->r_pos = tb->pos;
    app_mark(a, region);
}

void app_discard_text(app *a)
{
    text_box *tb = a->text;
    if (!tb)
        return;
    a->text = NULL;
    raster_copy_rect(a->img, tb->snapshot, tb->dirty);
    app_mark(a, tb->dirty);
    app_text_box_free(tb);
}

void app_commit_text(app *a)
{
    app_update_text_preview(a);
    text_box *tb = a->text;
    if (!tb)
        return;
    a->text = NULL;
    if (blank_text(tb->text))
    {
        raster_copy_rect(a->img, tb->snapshot, tb->dirty);
        app_mark(a, tb->dirty);
    }
    else
        app_push_patch(a, tb->snapshot, tb->dirty);
    app_text_box_free(tb);
}

/* The text box's corner handles (in canvas pixels), sitting just outside its bounds */
static int text_handle_at(frect b, pt p, float tol, float zoom)
{
    float e = 3.0f / zoom;
    pt c[4] = { { b.x0 - e, b.y0 - e }, { b.x1 + e, b.y0 - e }, { b.x1 + e, b.y1 + e }, { b.x0 - e, b.y1 + e } };
    for (int i = 0; i < 4; i++)
        if (fmaxf(fabsf(p.x - c[i].x), fabsf(p.y - c[i].y)) <= tol)
            return i;
    return -1;
}

static int frect_has(frect r, float pad, pt p)
{
    return p.x >= r.x0 - pad && p.x <= r.x1 + pad && p.y >= r.y0 - pad && p.y <= r.y1 + pad;
}

/* Whether a press at canvas point p lands on the text box or one of its handles */
int app_text_hit(const app *a, pt p)
{
    float tol = 5.0f / a->zoom;
    return a->tool == TOOL_TEXT && a->text && (frect_has(a->text->bounds, tol, p) || text_handle_at(a->text->bounds, p, tol, a->zoom) >= 0);
}

int app_text_handle_at(const app *a, pt p)
{
    return a->text ? text_handle_at(a->text->bounds, p, 5.0f / a->zoom, a->zoom) : -1;
}

int app_live_shape_hit(const app *a, pt p)
{
    float tol = 5.0f / a->zoom;
    return a->tool == TOOL_SHAPE && a->live && (live_shape_handle_at(a->live, p, tol) >= 0 || live_shape_contains(a->live, p, tol));
}

/* ---------------------------------------------------------------- selection */

static void float_free(floating *f)
{
    if (!f)
        return;
    img_free(f->pixels);
    img_free(f->base);
    img_free(f->before);
    img_free(f->lifted);
    patch_free(&f->hole);
    free(f);
}

void app_selection_free(selection *s)
{
    if (!s)
        return;
    free(s->lasso);
    float_free(s->fl);
    free(s);
}

static selection *selection_new(irect r)
{
    selection *s = calloc(1, sizeof *s);
    s->rect = r;
    return s;
}

/* The canvas before the pixels were lifted (taken out of f) */
static img *float_take_before(floating *f)
{
    img *b = f->before;
    if (b)
    {
        f->before = NULL;
        return b;
    }
    b = img_dup(f->base);
    if (b && f->lifted)
        raster_replace(b, f->lifted, f->origin.x0, f->origin.y0);
    return b;
}

static mask *lasso_mask(const pt *poly, int n, int w, int h)
{
    mask *m = mask_new(w, h);
    if (m)
        mask_fill_polygon(m, poly, n, 1.0f);
    return m;
}

typedef struct
{
    const img *before;
    const mask *inside;
    irect r;
} hole_ctx;

static int hole_pick(int x, int y, uint8_t px[4], void *user)
{
    hole_ctx *h = user;
    if (h->inside && mask_get(h->inside, x - h->r.x0, y - h->r.y0) < 0.5f)
        return 0;
    memcpy(px, img_at(h->before, x, y), 4);
    return 1;
}

/* Pick the selected pixels up so they can move, leaving Color 2 behind */
static void lift(app *a)
{
    selection *sel = a->sel;
    if (!sel || sel->fl)
        return;
    irect r = irect_intersect(sel->rect, irect_of(a->img));
    if (irect_empty(r))
        return;
    rgba secondary = a->secondary;
    floating *f = calloc(1, sizeof *f);
    f->pixels = raster_crop(a->img, r);
    f->base = img_dup(a->img);
    f->lifted = img_crop(a->img, r.x0, r.y0, irect_w(r), irect_h(r));
    mask *inside = sel->lasso ? lasso_mask(sel->lasso, sel->n_lasso, irect_w(r), irect_h(r)) : NULL;
    if (inside)
    {
        for (int y = 0; y < f->pixels->h; y++)
            for (int x = 0; x < f->pixels->w; x++)
                if (mask_get(inside, x, y) < 0.5f)
                    img_at(f->pixels, x, y)[3] = 0;
                else
                    memcpy(img_at(f->base, r.x0 + x, r.y0 + y), &secondary, 4);
    }
    else
        raster_fill_rect(f->base, r, secondary);
    /* Remember the lifted pixels, to redraw the hole if a friend draws under the selection */
    hole_ctx h = { a->img, inside, r };
    f->has_hole = patch_build(r, hole_pick, &h, &f->hole);
    f->hole_color = secondary;
    mask_free(inside);
    f->origin = r;
    sel->rect = r;
    sel->fl = f;
}

static void move_float_to(app *a, int x, int y)
{
    selection *sel = a->sel;
    if (!sel || !sel->fl)
        return;
    floating *f = sel->fl;
    uint8_t key[3] = { a->secondary.r, a->secondary.g, a->secondary.b };
    irect old = sel->rect;
    raster_copy_rect(a->img, f->base, old);
    raster_blit_over(a->img, f->pixels, x, y, a->transparent_selection ? key : NULL);
    sel->rect = irect_make(x, y, x + f->pixels->w, y + f->pixels->h);
    app_mark(a, irect_union(old, sel->rect));
}

void app_refresh_float(app *a)
{
    if (a->sel && a->sel->fl)
        move_float_to(a, a->sel->rect.x0, a->sel->rect.y0);
}

void app_toggle_transparent_selection(app *a)
{
    a->transparent_selection = !a->transparent_selection;
    app_refresh_float(a);
}

void app_select_all(app *a)
{
    app_commit_all(a);
    app_selection_free(a->sel);
    a->sel = selection_new(irect_of(a->img));
    a->tool = TOOL_SELECT;
}

/* Drop the selection, recording any move/paste as one undo step */
void app_commit_selection(app *a)
{
    selection *sel = a->sel;
    if (!sel)
        return;
    a->sel = NULL;
    floating *f = sel->fl;
    if (f)
    {
        img *before = float_take_before(f);
        if (f->canvas_changed)
            app_record(a, change_replace(before, a->img));
        else
        {
            app_push_patch(a, before, irect_union(f->origin, sel->rect));
            img_free(before);
        }
    }
    app_selection_free(sel);
}

void app_delete_selection(app *a)
{
    selection *sel = a->sel;
    if (!sel)
        return;
    a->sel = NULL;
    floating *f = sel->fl;
    if (f)
    {
        img *before = float_take_before(f);
        img_free(a->img);
        a->img = f->base;
        f->base = NULL;
        if (f->canvas_changed)
            record_replace(a, before);
        else
        {
            irect region = irect_union(f->origin, sel->rect);
            app_mark(a, region);
            app_push_patch(a, before, region);
            img_free(before);
        }
    }
    else
    {
        img *before = img_dup(a->img);
        irect r = sel->rect;
        if (sel->lasso)
        {
            mask *inside = lasso_mask(sel->lasso, sel->n_lasso, irect_w(r), irect_h(r));
            irect bounds = irect_of(a->img);
            for (int y = r.y0; y < r.y1; y++)
                for (int x = r.x0; x < r.x1; x++)
                    if (inside && mask_get(inside, x - r.x0, y - r.y0) >= 0.5f && irect_contains(bounds, (float)x, (float)y))
                        memcpy(img_at(a->img, x, y), &a->secondary, 4);
            mask_free(inside);
        }
        else
            raster_fill_rect(a->img, r, a->secondary);
        app_mark(a, r);
        app_push_patch(a, before, r);
        img_free(before);
    }
    app_selection_free(sel);
}

static img *selection_pixels(const app *a)
{
    const selection *sel = a->sel;
    if (!sel)
        return NULL;
    if (sel->fl)
        return img_dup(sel->fl->pixels);
    img *px = raster_crop(a->img, sel->rect);
    if (sel->lasso)
    {
        mask *inside = lasso_mask(sel->lasso, sel->n_lasso, px->w, px->h);
        for (int y = 0; y < px->h; y++)
            for (int x = 0; x < px->w; x++)
                if (!inside || mask_get(inside, x, y) < 0.5f)
                    img_at(px, x, y)[3] = 0;
        mask_free(inside);
    }
    return px;
}

void app_copy(app *a)
{
    app_commit_text(a);
    img *im = selection_pixels(a);
    host_clip_set_image(im ? im : a->img);
    img_free(im);
}

void app_cut(app *a)
{
    if (a->sel)
    {
        app_copy(a);
        app_delete_selection(a);
    }
}

void app_paste(app *a)
{
    img *im = host_clip_image();
    if (im)
        app_paste_image(a, im);
}

/* Paste as a floating selection at the top-left of the visible canvas,
 * enlarging the canvas if the image doesn't fit */
void app_paste_image(app *a, img *pixels)
{
    app_commit_all(a);
    int cw = a->img->w, ch = a->img->h, pw = pixels->w, ph = pixels->h;
    int canvas_changed = pw > cw || ph > ch;
    img *before = NULL;
    if (canvas_changed)
    {
        img *grown = raster_resize_canvas(a->img, cw > pw ? cw : pw, ch > ph ? ch : ph, a->secondary);
        if (!grown)
        {
            img_free(pixels);
            app_set_error(a, "Paint doesn't have enough memory to paste this picture.");
            return;
        }
        before = a->img;
        a->img = grown;
        a->dirty_full = 1;
    }
    cw = a->img->w;
    ch = a->img->h;
    int vx = (int)fmaxf((a->scroll_x / a->ppp - APP_MARGIN) / a->zoom, 0.0f);
    int vy = (int)fmaxf((a->scroll_y / a->ppp - APP_MARGIN) / a->zoom, 0.0f);
    int x = vx < cw - pw ? vx : cw - pw, y = vy < ch - ph ? vy : ch - ph;
    x = x > 0 ? x : 0;
    y = y > 0 ? y : 0;
    floating *f = calloc(1, sizeof *f);
    f->base = img_dup(a->img);
    f->before = before;
    f->origin = IRECT_EMPTY;
    f->canvas_changed = canvas_changed;
    f->pixels = pixels;
    uint8_t key[3] = { a->secondary.r, a->secondary.g, a->secondary.b };
    app_mark(a, raster_blit_over(a->img, pixels, x, y, a->transparent_selection ? key : NULL));
    app_selection_free(a->sel);
    a->sel = selection_new(irect_make(x, y, x + pw, y + ph));
    a->sel->fl = f;
    a->tool = TOOL_SELECT;
}

/* ---------------------------------------------------------------- image operations */

void app_transform(app *a, img *(*f)(const img *src, void *user), void *user)
{
    app_commit_text(a);
    if (a->sel)
    {
        lift(a);
        selection *sel = a->sel;
        if (!sel || !sel->fl)
            return;
        img *out = f(sel->fl->pixels, user);
        if (!out)
            return;
        img_free(sel->fl->pixels);
        sel->fl->pixels = out;
        free(sel->lasso);
        sel->lasso = NULL;
        sel->n_lasso = 0;
        app_refresh_float(a);
        return;
    }
    img *out = f(a->img, user);
    if (!out)
    {
        app_set_error(a, "Paint doesn't have enough memory for that.");
        return;
    }
    img *before = a->img;
    a->img = out;
    record_replace(a, before);
}

static img *rotate_with(const img *src, void *user)
{
    switch (*(int *)user)
    {
    case ROTATE_RIGHT:
        return img_rotate90(src);
    case ROTATE_LEFT:
        return img_rotate270(src);
    case ROTATE_180:
        return img_rotate180(src);
    case FLIP_VERTICAL:
        return img_flip_vertical(src);
    default:
        return img_flip_horizontal(src);
    }
}

void app_rotate(app *a, int how)
{
    app_transform(a, rotate_with, &how);
}

static img *invert_with(const img *src, void *user)
{
    return raster_invert(src);
}

void app_invert(app *a)
{
    app_transform(a, invert_with, NULL);
}

void app_crop(app *a)
{
    app_commit_text(a);
    if (!a->sel)
        return;
    irect r = irect_intersect(a->sel->rect, irect_of(a->img));
    app_commit_selection(a);
    if (irect_empty(r))
        return;
    img *before = a->img;
    a->img = raster_crop(before, r);
    record_replace(a, before);
    a->scroll_x = a->scroll_y = 0.0f;
}

void app_clear_image(app *a)
{
    app_commit_all(a);
    img *before = img_dup(a->img);
    irect all = irect_of(a->img);
    raster_fill_rect(a->img, all, a->secondary);
    app_mark(a, all);
    app_push_patch(a, before, all);
    img_free(before);
}

/* The canvas grows or shrinks to w x h (new area Color 2), as one undo step */
static void resize_canvas(app *a, int w, int h)
{
    img *out = raster_resize_canvas(a->img, w, h, a->secondary);
    if (!out)
    {
        app_set_error(a, "Paint doesn't have enough memory for a picture that big.");
        return;
    }
    img *before = a->img;
    a->img = out;
    record_replace(a, before);
}

void app_open_resize(app *a)
{
    free(a->resize);
    a->resize = calloc(1, sizeof *a->resize);
    a->resize->percent = 1;
    a->resize->w = a->resize->h = 100.0f;
    a->resize->keep_aspect = 1;
    a->resize->base_w = a->sel ? irect_w(a->sel->rect) : a->img->w;
    a->resize->base_h = a->sel ? irect_h(a->sel->rect) : a->img->h;
}

void app_resize_set_mode(app *a, int percent)
{
    resize_dialog *d = a->resize;
    if (!d)
        return;
    d->percent = percent;
    d->w = percent ? 100.0f : (float)d->base_w;
    d->h = percent ? 100.0f : (float)d->base_h;
}

void app_resize_set_w(app *a, float w)
{
    resize_dialog *d = a->resize;
    if (!d)
        return;
    float aspect = (float)d->base_h / (float)(d->base_w > 1 ? d->base_w : 1);
    d->w = clampf(w, 1.0f, d->percent ? 1000.0f : 30000.0f);
    if (d->keep_aspect)
        d->h = d->percent ? d->w : fmaxf(roundf(d->w * aspect), 1.0f);
}

void app_resize_set_h(app *a, float h)
{
    resize_dialog *d = a->resize;
    if (!d)
        return;
    float aspect = (float)d->base_h / (float)(d->base_w > 1 ? d->base_w : 1);
    d->h = clampf(h, 1.0f, d->percent ? 1000.0f : 30000.0f);
    if (d->keep_aspect)
        d->w = d->percent ? d->h : fmaxf(roundf(d->h / aspect), 1.0f);
}

static img *resize_with(const img *src, void *user)
{
    int *wh = user;
    return img_resize(src, wh[0], wh[1], RESIZE_TRIANGLE);
}

void app_resize_ok(app *a)
{
    resize_dialog *d = a->resize;
    if (!d)
        return;
    float w = d->percent ? (float)d->base_w * d->w / 100.0f : d->w;
    float h = d->percent ? (float)d->base_h * d->h / 100.0f : d->h;
    int wh[2] = { (int)fmaxf(roundf(w), 1.0f), (int)fmaxf(roundf(h), 1.0f) };
    free(a->resize);
    a->resize = NULL;
    app_transform(a, resize_with, wh);
}

void app_resize_cancel(app *a)
{
    free(a->resize);
    a->resize = NULL;
}

void app_open_properties(app *a)
{
    a->properties = 1;
    a->prop_w = (float)a->img->w;
    a->prop_h = (float)a->img->h;
}

void app_properties_ok(app *a)
{
    a->properties = 0;
    int w = (int)fmaxf(roundf(clampf(a->prop_w, 1.0f, 30000.0f)), 1.0f), h = (int)fmaxf(roundf(clampf(a->prop_h, 1.0f, 30000.0f)), 1.0f);
    if (w != a->img->w || h != a->img->h)
    {
        app_commit_all(a);
        resize_canvas(a, w, h);
    }
}

void app_properties_cancel(app *a)
{
    a->properties = 0;
}

/* ---------------------------------------------------------------- colours */

rgba hsl_to_rgb(const float hsl[3])
{
    float h = hsl[0], s = hsl[1], l = hsl[2];
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float hp = (h - floorf(h)) * 6.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r, g, b;
    switch ((int)hp)
    {
    case 0:
        r = c, g = x, b = 0;
        break;
    case 1:
        r = x, g = c, b = 0;
        break;
    case 2:
        r = 0, g = c, b = x;
        break;
    case 3:
        r = 0, g = x, b = c;
        break;
    case 4:
        r = x, g = 0, b = c;
        break;
    default:
        r = c, g = 0, b = x;
        break;
    }
    float m = l - c / 2.0f;
    rgba out = { (uint8_t)clampf(roundf((r + m) * 255.0f), 0, 255), (uint8_t)clampf(roundf((g + m) * 255.0f), 0, 255),
                 (uint8_t)clampf(roundf((b + m) * 255.0f), 0, 255), 255 };
    return out;
}

void rgb_to_hsl(rgba c, float hsl[3])
{
    float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
    float max = fmaxf(fmaxf(r, g), b), min = fminf(fminf(r, g), b);
    float l = (max + min) / 2.0f, d = max - min;
    if (d == 0.0f)
    {
        hsl[0] = hsl[1] = 0.0f;
        hsl[2] = l;
        return;
    }
    float s = d / (1.0f - fabsf(2.0f * l - 1.0f)), h;
    if (max == r)
    {
        h = fmodf((g - b) / d, 6.0f);
        if (h < 0.0f)
            h += 6.0f;
    }
    else if (max == g)
        h = (b - r) / d + 2.0f;
    else
        h = (r - g) / d + 4.0f;
    hsl[0] = h / 6.0f;
    hsl[1] = clampf(s, 0.0f, 1.0f);
    hsl[2] = l;
}

void app_open_edit_colors(app *a)
{
    a->edit_colors = 1;
    rgb_to_hsl(app_active_color(a), a->hsl);
}

void app_edit_colors_ok(app *a)
{
    app_set_active_color(a, hsl_to_rgb(a->hsl));
    a->edit_colors = 0;
}

void app_edit_colors_cancel(app *a)
{
    a->edit_colors = 0;
}

void app_add_custom_color(app *a, rgba c)
{
    for (int i = 0; i < 10; i++)
        if (a->has_custom[i] && rgba_eq(a->custom[i], c))
            return;
    memmove(a->custom + 1, a->custom, 9 * sizeof *a->custom);
    memmove(a->has_custom + 1, a->has_custom, 9 * sizeof *a->has_custom);
    a->custom[0] = c;
    a->has_custom[0] = 1;
}

/* ---------------------------------------------------------------- zoom */

void app_zoom_at(app *a, float z, const pt *anchor)
{
    z = clampf(z, 0.05f, 64.0f);
    if (fabsf(z - a->zoom) < 1e-4f)
        return;
    /* Keep the canvas point under the anchor (view coordinates) in place */
    float ax = anchor ? anchor->x : a->view.w * 0.5f, ay = anchor ? anchor->y : a->view.h * 0.5f;
    float m = APP_MARGIN * a->ppp, eff = a->zoom * a->ppp;
    float px = (a->scroll_x + ax - m) / eff, py = (a->scroll_y + ay - m) / eff;
    a->zoom = z;
    a->scroll_x = fmaxf(m + px * z * a->ppp - ax, 0.0f);
    a->scroll_y = fmaxf(m + py * z * a->ppp - ay, 0.0f);
}

void app_zoom_step(app *a, int up, const pt *anchor)
{
    float z = up ? 64.0f : 0.05f;
    if (up)
    {
        for (int i = 0; i < 16; i++)
            if (ZOOM_STEPS[i] > a->zoom + 1e-3f)
            {
                z = ZOOM_STEPS[i];
                break;
            }
    }
    else
        for (int i = 15; i >= 0; i--)
            if (ZOOM_STEPS[i] < a->zoom - 1e-3f)
            {
                z = ZOOM_STEPS[i];
                break;
            }
    app_zoom_at(a, z, anchor);
}

void app_zoom_fit(app *a)
{
    float aw = a->view.w / a->ppp - (APP_MARGIN * 2.0f + 12.0f), ah = a->view.h / a->ppp - (APP_MARGIN * 2.0f + 12.0f);
    a->zoom = clampf(fminf(aw / (float)a->img->w, ah / (float)a->img->h), 0.05f, 64.0f);
    a->scroll_x = a->scroll_y = 0.0f;
}

float app_zoom_slider(const app *a)
{
    float lo = logf(0.125f), hi = logf(8.0f);
    return clampf((logf(a->zoom) - lo) / (hi - lo), 0.0f, 1.0f);
}

float app_zoom_slider_tick(void)
{
    float lo = logf(0.125f), hi = logf(8.0f);
    return -lo / (hi - lo);
}

void app_set_zoom_slider(app *a, float t)
{
    float lo = logf(0.125f), hi = logf(8.0f);
    app_zoom_at(a, expf(lo + t * (hi - lo)), NULL);
}

void app_set_full_screen(app *a, int on)
{
    a->full_screen = on;
    if (a->win)
        host_fullscreen(a->win, on);
}

int app_selection_size(const app *a, int *w, int *h)
{
    if (a->op.kind == OP_SELECTING && a->tool == TOOL_SELECT)
    {
        *w = (int)roundf(fabsf(a->op.sel_end.x - a->op.sel_start.x));
        *h = (int)roundf(fabsf(a->op.sel_end.y - a->op.sel_start.y));
        return 1;
    }
    if (!a->sel)
        return 0;
    *w = irect_w(a->sel->rect);
    *h = irect_h(a->sel->rect);
    return 1;
}

void app_canvas_size(const app *a, int *w, int *h)
{
    *w = a->op.kind == OP_RESIZE_CANVAS ? a->op.size_w : a->img->w;
    *h = a->op.kind == OP_RESIZE_CANVAS ? a->op.size_h : a->img->h;
}

/* ---------------------------------------------------------------- canvas operations */

void app_op_clear(app *a)
{
    img_free(a->op.snapshot);
    mask_free(a->op.mask);
    free(a->op.lasso);
    memset(&a->op, 0, sizeof a->op);
}

static pt clamp_to(pt p, int w, int h)
{
    return pt_make(clampf(p.x, 0.0f, (float)w), clampf(p.y, 0.0f, (float)h));
}

static void lasso_push(app_op *op, pt q)
{
    if (op->n_lasso == op->cap_lasso)
    {
        op->cap_lasso = op->cap_lasso ? op->cap_lasso * 2 : 64;
        op->lasso = realloc(op->lasso, (size_t)op->cap_lasso * sizeof *op->lasso);
    }
    op->lasso[op->n_lasso++] = q;
}

static void stroke_to(app *a, const pt *pts, int n, int first)
{
    app_op *op = &a->op;
    if (op->kind != OP_STROKE)
        return;
    irect r = IRECT_EMPTY;
    for (int i = 0; i < n; i++)
    {
        pt p = pts[i];
        if (!first && p.x == op->last.x && p.y == op->last.y)
            continue;
        irect seg = op->pen == PEN_BRUSH ? brush_segment(op->brush, op->mask, op->last, p, op->size, &a->rng)
                                         : mask_square_line(op->mask, op->last, p, (int)roundf(op->size));
        r = irect_union(r, seg);
        op->last = p;
    }
    if (irect_empty(r))
        return;
    raster_copy_rect(a->img, op->snapshot, r);
    raster_blend_mask(a->img, op->mask, op->color, op->opacity, r);
    op->dirty = irect_union(op->dirty, r);
    app_mark(a, r);
}

/* The airbrush keeps spraying while the button is held, even without moving */
void app_spray_frame(app *a, float dt)
{
    app_op *op = &a->op;
    if (op->kind != OP_STROKE || op->pen != PEN_BRUSH || op->brush != BRUSH_AIRBRUSH)
        return;
    int n = (int)clampf(op->size * op->size * 0.12f * dt * 60.0f, 2.0f, 2000.0f);
    irect r = brush_spray(op->mask, op->last, op->size, &a->rng, n);
    raster_copy_rect(a->img, op->snapshot, r);
    raster_blend_mask(a->img, op->mask, op->color, op->opacity, r);
    op->dirty = irect_union(op->dirty, r);
    app_mark(a, r);
}

void app_begin(app *a, pt p, int button, pt anchor)
{
    int secondary = button == BTN_RIGHT;
    rgba c1 = secondary ? a->secondary : a->primary;
    int w = a->img->w, h = a->img->h;
    int inside = p.x >= 0.0f && p.y >= 0.0f && p.x < (float)w && p.y < (float)h;
    app_op *op = &a->op;
    switch (a->tool)
    {
    case TOOL_PENCIL:
    case TOOL_BRUSH:
    case TOOL_ERASER:
    {
        app_commit_all(a);
        app_op_clear(a);
        op->button = button;
        op->color = c1;
        op->opacity = 1.0f;
        if (a->tool == TOOL_PENCIL)
        {
            op->pen = PEN_PENCIL;
            op->size = a->size_pencil;
        }
        else if (a->tool == TOOL_ERASER)
        {
            op->pen = PEN_ERASER;
            op->color = a->secondary;
            op->size = a->size_eraser;
        }
        else
        {
            op->pen = PEN_BRUSH;
            op->brush = a->brush;
            op->opacity = brush_opacity(a->brush);
            op->size = a->size_brush;
        }
        op->snapshot = img_dup(a->img);
        op->mask = mask_new(w, h);
        if (!op->snapshot || !op->mask)
        {
            app_op_clear(a);
            app_set_error(a, "Paint doesn't have enough memory to draw.");
            return;
        }
        op->kind = OP_STROKE;
        op->last = p;
        op->dirty = IRECT_EMPTY;
        a->last_spray_ = a->now;
        stroke_to(a, &p, 1, 1);
        break;
    }
    case TOOL_FILL:
    {
        if (!inside)
            return;
        app_commit_all(a);
        img *before = img_dup(a->img);
        uint8_t tolerance = (uint8_t)roundf(a->fill_tolerance / 100.0f * 255.0f);
        irect r = raster_flood_fill(a->img, (int)p.x, (int)p.y, c1, tolerance);
        app_mark(a, r);
        app_push_patch(a, before, r);
        img_free(before);
        break;
    }
    case TOOL_PICKER:
    {
        if (!inside)
            return;
        rgba c;
        memcpy(&c, img_at(a->img, (int)p.x, (int)p.y), 4);
        if (secondary)
            a->secondary = c;
        else
            a->primary = c;
        a->tool = a->prev_tool;
        break;
    }
    case TOOL_MAGNIFIER:
        app_zoom_step(a, !secondary, &anchor);
        break;
    case TOOL_TEXT:
    {
        float tol = 5.0f / a->zoom;
        if (a->text)
        {
            int handle = text_handle_at(a->text->bounds, p, tol, a->zoom);
            if (handle >= 0)
            {
                app_op_clear(a);
                op->kind = OP_TEXT_SCALE;
                op->button = button;
                op->handle = handle;
                op->start_size = a->text_style.size_pt;
                op->start_bounds = a->text->bounds;
                return;
            }
            if (frect_has(a->text->bounds, tol, p))
            {
                app_op_clear(a);
                op->kind = OP_TEXT_MOVE;
                op->button = button;
                op->grab = p;
                op->start_a = a->text->pos;
                return;
            }
        }
        app_commit_all(a);
        if (!inside)
            return;
        app_text_family(a);
        text_box *tb = calloc(1, sizeof *tb);
        tb->pos = pt_make(floorf(p.x), floorf(p.y));
        tb->text = dup_str("");
        tb->preedit = dup_str("");
        tb->snapshot = img_dup(a->img);
        tb->mask = mask_new(w, h);
        tb->dirty = IRECT_EMPTY;
        tb->bounds = frect_xywh(p.x, p.y, 0, 0);
        tb->editing = 1;
        text_cursor_init(&tb->cursor);
        caret_layout_init(&tb->layout);
        tb->blink = a->now;
        if (!tb->snapshot || !tb->mask)
        {
            app_text_box_free(tb);
            app_set_error(a, "Paint doesn't have enough memory for text.");
            return;
        }
        a->text = tb;
        break;
    }
    case TOOL_SHAPE:
    {
        float tol = 5.0f / a->zoom;
        if (a->live)
        {
            int handle = live_shape_handle_at(a->live, p, tol);
            if (handle >= 0 || live_shape_contains(a->live, p, tol))
            {
                app_op_clear(a);
                op->kind = handle >= 0 ? OP_SHAPE_HANDLE : OP_SHAPE_MOVE;
                op->button = button;
                op->handle = handle;
                op->grab = p;
                op->start_a = a->live->a;
                op->start_b = a->live->b;
                return;
            }
        }
        /* Clicking away from the live shape stamps it and starts a new one */
        app_commit_all(a);
        a->live = live_shape_new(a->shape, p, secondary, img_dup(a->img));
        if (!a->live)
        {
            app_set_error(a, "Paint doesn't have enough memory to draw.");
            return;
        }
        app_op_clear(a);
        op->kind = OP_SHAPE_CREATE;
        op->button = button;
        break;
    }
    case TOOL_SELECT:
    case TOOL_FREE_SELECT:
    {
        app_commit_text(a);
        if (a->sel && irect_contains(a->sel->rect, p.x, p.y))
        {
            irect r = a->sel->rect;
            lift(a);
            app_op_clear(a);
            op->kind = OP_MOVING_SEL;
            op->button = button;
            op->grab = p;
            op->origin_x = r.x0;
            op->origin_y = r.y0;
            return;
        }
        app_commit_selection(a);
        pt q = clamp_to(p, w, h);
        app_op_clear(a);
        op->kind = OP_SELECTING;
        op->button = button;
        op->sel_start = op->sel_end = q;
        lasso_push(op, q);
        break;
    }
    }
}

static void update_op(app *a, const pt *pts, int n, int shift, float dt)
{
    if (n <= 0)
        return;
    pt p = pts[n - 1];
    app_op *op = &a->op;
    switch (op->kind)
    {
    case OP_STROKE:
        stroke_to(a, pts, n, 0);
        app_spray_frame(a, dt);
        break;
    case OP_SHAPE_CREATE:
        if (a->live)
            a->live->b = shape_constrain(a->live->kind, a->live->a, p, shift);
        break;
    case OP_SHAPE_HANDLE:
        if (a->live)
            live_shape_drag_handle(a->live, op->handle, op->start_a, op->start_b, p, shift);
        break;
    case OP_SHAPE_MOVE:
        if (a->live)
            live_shape_translate(a->live, op->start_a, op->start_b, p.x - op->grab.x, p.y - op->grab.y);
        break;
    case OP_TEXT_MOVE:
        if (a->text)
        {
            float dx = p.x - op->grab.x, dy = p.y - op->grab.y;
            op->moved |= sqrtf(dx * dx + dy * dy) * a->zoom > 2.0f;
            if (op->moved)
                a->text->pos = pt_make(roundf(op->start_a.x + dx), roundf(op->start_a.y + dy));
        }
        break;
    case OP_TEXT_SCALE:
    {
        /* Scale about the opposite corner, following the larger of the two stretches */
        frect sb = op->start_bounds;
        pt corners[4] = { { sb.x0, sb.y0 }, { sb.x1, sb.y0 }, { sb.x1, sb.y1 }, { sb.x0, sb.y1 } };
        pt anchor = corners[(op->handle + 2) % 4];
        float bw = sb.x1 - sb.x0, bh = sb.y1 - sb.y0;
        float ratio = fmaxf(fabsf(p.x - anchor.x) / fmaxf(bw, 1.0f), fabsf(p.y - anchor.y) / fmaxf(bh, 1.0f));
        float size = clampf(roundf(op->start_size * ratio), 4.0f, 400.0f);
        a->text_style.size_pt = size;
        float k = size / op->start_size, w = bw * k, h = bh * k;
        int left = op->handle == 0 || op->handle == 3, top = op->handle == 0 || op->handle == 1;
        if (a->text)
            a->text->pos = pt_make(roundf(left ? anchor.x - w : anchor.x), roundf(top ? anchor.y - h : anchor.y));
        break;
    }
    case OP_SELECTING:
        for (int i = 0; i < n; i++)
        {
            pt q = clamp_to(pts[i], a->img->w, a->img->h);
            op->sel_end = q;
            pt l = op->lasso[op->n_lasso - 1];
            if (hypotf(l.x - q.x, l.y - q.y) >= 1.0f)
                lasso_push(op, q);
        }
        break;
    case OP_MOVING_SEL:
        move_float_to(a, op->origin_x + (int)roundf(p.x - op->grab.x), op->origin_y + (int)roundf(p.y - op->grab.y));
        break;
    case OP_RESIZE_CANVAS:
        op->size_w = op->handle == HANDLE_BOTTOM ? a->img->w : (int)clampf(roundf(p.x), 1.0f, 30000.0f);
        op->size_h = op->handle == HANDLE_RIGHT ? a->img->h : (int)clampf(roundf(p.y), 1.0f, 30000.0f);
        break;
    }
}

void app_update(app *a, const pt *pts, int n, int shift, float dt)
{
    update_op(a, pts, n, shift, dt);
    app_render_live_shape(a);
    app_update_text_preview(a);
}

void app_end(app *a)
{
    app_op *op = &a->op;
    switch (op->kind)
    {
    case OP_STROKE:
        app_push_patch(a, op->snapshot, op->dirty);
        break;
    case OP_SHAPE_CREATE:
        /* A click without a drag draws nothing */
        if (a->live && live_shape_is_empty(a->live))
            app_discard_shape(a);
        break;
    case OP_TEXT_MOVE:
        /* A click (no drag) puts the caret where it landed and resumes typing */
        if (!op->moved && a->text)
        {
            text_box *tb = a->text;
            text_cursor_move_to(&tb->cursor, caret_index_at(&tb->layout, op->grab.x - tb->pos.x, op->grab.y - tb->pos.y), 0);
            tb->editing = 1;
            tb->blink = a->now;
        }
        break;
    case OP_SELECTING:
        if (a->tool == TOOL_FREE_SELECT)
        {
            pt lo = op->lasso[0], hi = op->lasso[0];
            for (int i = 0; i < op->n_lasso; i++)
            {
                lo = pt_make(fminf(lo.x, op->lasso[i].x), fminf(lo.y, op->lasso[i].y));
                hi = pt_make(fmaxf(hi.x, op->lasso[i].x), fmaxf(hi.y, op->lasso[i].y));
            }
            irect r = irect_make((int)floorf(lo.x), (int)floorf(lo.y), (int)ceilf(hi.x), (int)ceilf(hi.y));
            if (op->n_lasso >= 3 && irect_w(r) >= 2 && irect_h(r) >= 2)
            {
                selection *s = selection_new(r);
                s->lasso = op->lasso;
                s->n_lasso = op->n_lasso;
                op->lasso = NULL;
                for (int i = 0; i < s->n_lasso; i++)
                    s->lasso[i] = pt_make(s->lasso[i].x - (float)r.x0, s->lasso[i].y - (float)r.y0);
                app_selection_free(a->sel);
                a->sel = s;
            }
        }
        else
        {
            pt s = op->sel_start, e = op->sel_end;
            irect r = irect_make((int)roundf(fminf(s.x, e.x)), (int)roundf(fminf(s.y, e.y)), (int)roundf(fmaxf(s.x, e.x)), (int)roundf(fmaxf(s.y, e.y)));
            if (!irect_empty(r))
            {
                app_selection_free(a->sel);
                a->sel = selection_new(r);
            }
        }
        break;
    case OP_RESIZE_CANVAS:
        if (op->size_w != a->img->w || op->size_h != a->img->h)
            resize_canvas(a, op->size_w, op->size_h);
        break;
    }
    app_op_clear(a);
}

/* ---------------------------------------------------------------- keyboard */

#ifdef __APPLE__
#define WORD_MOD KMOD_ALT
#define LINE_MOD KMOD_CTRL
#else
#define WORD_MOD KMOD_CTRL
#define LINE_MOD 0
#endif

/* Typing into the text box on the canvas. Returns whether the key was used. */
static int text_key(app *a, int key, int mods)
{
    text_box *tb = a->text;
    if (!tb || !tb->editing)
        return 0;
    int shift = (mods & KMOD_SHIFT) != 0, cmd = (mods & KMOD_CTRL) != 0;
    /* On a Mac Option moves by word and Command by line; elsewhere Ctrl moves by word */
    int word = (mods & WORD_MOD) != 0, line = (mods & LINE_MOD) != 0;
    text_cursor *cur = &tb->cursor;
    const caret_layout *layout = &tb->layout;
    if (cmd && (key == 'C' || key == 'X'))
    {
        if (text_cursor_has_selection(cur))
        {
            char *t = text_cursor_selected(cur, tb->text);
            host_clip_set_text(t);
            free(t);
            if (key == 'X')
                text_cursor_delete_selection(cur, &tb->text);
        }
    }
    else if (cmd && key == 'V')
    {
        char *t = host_clip_text();
        if (t)
        {
            /* The host already ends lines with '\n'; any stray '\r' goes */
            char *w = t;
            for (char *r = t; *r; r++)
                if (*r != '\r')
                    *w++ = *r;
            *w = 0;
            text_cursor_insert(cur, &tb->text, t);
            free(t);
        }
    }
    else
        switch (key)
        {
        case KEY_BACKSPACE:
            text_cursor_backspace(cur, &tb->text, layout, word, line);
            break;
        case KEY_DELETE:
            text_cursor_delete(cur, &tb->text, word);
            break;
        case KEY_ENTER:
            text_cursor_insert(cur, &tb->text, "\n");
            break;
        case KEY_LEFT:
            if (line)
                text_cursor_line_start(cur, layout, shift);
            else
                text_cursor_left(cur, tb->text, shift, word);
            break;
        case KEY_RIGHT:
            if (line)
                text_cursor_line_end(cur, layout, shift);
            else
                text_cursor_right(cur, tb->text, shift, word);
            break;
        case KEY_UP:
            text_cursor_vertical(cur, tb->text, layout, -1, shift);
            break;
        case KEY_DOWN:
            text_cursor_vertical(cur, tb->text, layout, 1, shift);
            break;
        case KEY_HOME:
            text_cursor_line_start(cur, layout, shift);
            break;
        case KEY_END:
            text_cursor_line_end(cur, layout, shift);
            break;
        case KEY_ESCAPE:
            tb->editing = 0;
            break;
        case 'A':
            if (!cmd)
                return 0;
            text_cursor_select_all(cur, tb->text);
            break;
        case 'Y':
            if (!cmd)
                return 0;
            text_cursor_redo(cur, &tb->text);
            break;
        case 'Z':
            if (!cmd)
                return 0;
            if (shift)
                text_cursor_redo(cur, &tb->text);
            else
                text_cursor_undo(cur, &tb->text);
            break;
        default:
            /* Anything else (e.g. Ctrl+S) goes on to the app */
            return 0;
        }
    tb->blink = a->now;
    return 1;
}

static void nudge(app *a, int key)
{
    int dx = key == KEY_LEFT ? -1 : key == KEY_RIGHT ? 1 : 0, dy = key == KEY_UP ? -1 : key == KEY_DOWN ? 1 : 0;
    if (a->text)
        a->text->pos = pt_make(a->text->pos.x + (float)dx, a->text->pos.y + (float)dy);
    else if (a->live)
        live_shape_translate(a->live, a->live->a, a->live->b, (float)dx, (float)dy);
    else if (a->sel)
    {
        lift(a);
        if (a->sel)
            move_float_to(a, a->sel->rect.x0 + dx, a->sel->rect.y0 + dy);
    }
}

/* Paint's shortcuts (Rust's key) */
static int shortcut_key(app *a, int key, int mods)
{
    int cmd = (mods & KMOD_CTRL) != 0, shift = (mods & KMOD_SHIFT) != 0;
    int arrow = key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN;
    int erase = (key == KEY_DELETE || key == KEY_BACKSPACE) && !cmd;
    if (cmd)
        switch (key)
        {
        case 'Z':
            if (shift)
                app_redo(a);
            else
                app_undo(a);
            return 1;
        case 'Y':
            app_redo(a);
            return 1;
        case 'N':
            if (shift)
                app_clear_image(a);
            else
                app_request(a, PENDING_NEW, NULL);
            return 1;
        case 'O':
            app_request(a, PENDING_OPEN, NULL);
            return 1;
        case 'S':
            if (shift)
                app_save_as(a);
            else
                app_save(a);
            return 1;
        case 'A':
            app_select_all(a);
            return 1;
        case 'X':
            if (shift)
            {
                app_crop(a);
                return 1;
            }
            app_cut(a);
            return 1;
        case 'C':
            app_copy(a);
            return 1;
        case 'V':
            app_paste(a);
            return 1;
        case 'I':
            if (!shift)
                break;
            app_invert(a);
            return 1;
        case 'W':
            app_open_resize(a);
            return 1;
        case 'G':
            a->show_grid = !a->show_grid;
            return 1;
        case 'R':
            a->show_rulers = !a->show_rulers;
            return 1;
        case 'E':
            app_open_properties(a);
            return 1;
        case '=':
        case KEY_KP_ADD:
            app_zoom_step(a, 1, NULL);
            return 1;
        case '-':
        case KEY_KP_SUBTRACT:
            app_zoom_step(a, 0, NULL);
            return 1;
        case '0':
            app_zoom_fit(a);
            return 1;
        case '1':
            app_zoom_at(a, 1.0f, NULL);
            return 1;
        }
    if (key == KEY_F11)
        app_set_full_screen(a, !a->full_screen);
    else if (key == KEY_ESCAPE && a->full_screen)
        app_set_full_screen(a, 0);
    else if (erase && a->live)
        app_discard_shape(a);
    else if (erase && a->text)
        app_discard_text(a);
    else if (key == KEY_ENTER && a->text)
        app_commit_text(a);
    else if (erase)
        app_delete_selection(a);
    else if ((key == KEY_ESCAPE || key == KEY_ENTER) && a->live)
        app_commit_shape(a);
    else if (key == KEY_ESCAPE)
    {
        if (a->text)
            app_commit_text(a);
        else
            app_commit_selection(a);
    }
    else if (arrow && (a->text || a->live || a->sel))
        nudge(a, key);
    else
        return 0;
    return 1;
}

/* Shift pressed or let go mid-drag: shapes snap (or stop snapping) at once */
static int reshift(app *a, int shift)
{
    if ((a->op.kind != OP_SHAPE_CREATE && a->op.kind != OP_SHAPE_HANDLE) || !a->has_last_pt_)
        return 0;
    app_update(a, &a->last_pt_, 1, shift, 0.0f);
    return 1;
}

int app_key(app *a, int key, int mods)
{
    if (app_busy_dialog(a))
        return 0;
    a->last_text_[0] = 0;   /* a new keystroke: its text may come as typed text and as an IME commit */
    if (key == KEY_SHIFT)
        return reshift(a, 1);
    if (text_key(a, key, mods))
    {
        app_update_text_preview(a);
        return 1;
    }
    int used = shortcut_key(a, key, mods);
    if (used)
    {
        app_render_live_shape(a);
        app_update_text_preview(a);
    }
    return used;
}

void app_key_up(app *a, int key, int mods)
{
    if (key == KEY_SHIFT)
        reshift(a, 0);
}

static void insert_typed(app *a, const char *utf8)
{
    text_box *tb = a->text;
    text_cursor_insert(&tb->cursor, &tb->text, utf8);
    tb->blink = a->now;
    app_update_text_preview(a);
}

void app_text(app *a, const char *utf8)
{
    if (!a->text || !a->text->editing)
        return;
    snprintf(a->last_text_, sizeof a->last_text_, "%s", utf8);
    /* Control characters never go into the text */
    size_t n = strlen(utf8);
    char *t = malloc(n + 1), *w = t;
    for (const char *r = utf8; *r; r++)
        if ((unsigned char)*r >= 0x20 && *r != 0x7F)
            *w++ = *r;
    *w = 0;
    insert_typed(a, t);
    free(t);
}

void app_ime_preedit(app *a, const char *utf8)
{
    if (!a->text || !a->text->editing)
        return;
    free(a->text->preedit);
    a->text->preedit = dup_str(utf8);
    a->text->blink = a->now;
    app_update_text_preview(a);
}

void app_ime_commit(app *a, const char *utf8)
{
    /* The same keystroke can arrive as both typed text and an input-method commit */
    if (!a->text || !a->text->editing || !strcmp(a->last_text_, utf8))
        return;
    a->text->preedit[0] = 0;
    insert_typed(a, utf8);
}

const char *app_shortcut(int mods, int key, char *buf, size_t cap)
{
    char k[8];
    if (key >= KEY_F1 && key <= KEY_F12)
        snprintf(k, sizeof k, "F%d", key - KEY_F1 + 1);
    else
        snprintf(k, sizeof k, "%c", key);
#ifdef __APPLE__
    snprintf(buf, cap, "%s%s%s%s", mods & KMOD_SHIFT ? "\xE2\x87\xA7" : "", mods & KMOD_ALT ? "\xE2\x8C\xA5" : "",
             mods & KMOD_CTRL ? "\xE2\x8C\x98" : "", k);
#else
    snprintf(buf, cap, "%s%s%s%s", mods & KMOD_CTRL ? "Ctrl+" : "", mods & KMOD_ALT ? "Alt+" : "", mods & KMOD_SHIFT ? "Shift+" : "", k);
#endif
    return buf;
}

int app_caret_on(const app *a)
{
    return a->text && a->text->editing && fmod(a->now - a->text->blink, 1.06) < 0.53;
}
