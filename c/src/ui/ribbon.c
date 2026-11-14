/*
 * Paint (C port) - the controls of the Windows "Scenic Ribbon" (Rust: ribbon.rs,
 * and app.rs's shape gallery, size rows and drawing-together badges)
 *
 * Each control takes its rectangle from the kit's layout, asks the kit what
 * the pointer did, and draws itself, as the Rust ones do in an egui Ui.
 */
#include <math.h>
#include <string.h>
#include "ui/font.h"
#include "ui/kit_internal.h"
#include "ui/ribbon.h"

#define HIGHLIGHT_TEXT_PT 12.5f

static font *label_font(void)
{
    return ui_font(LABEL_PT, 0);
}

static color text_color(int enabled)
{
    return enabled ? PAL_TEXT : DISABLED_TEXT;
}

/* A rectangle with rounded corners and a 1 pt border inside it */
static void highlight(surface *dst, rect r, color fill, color border)
{
    gfx_round(dst, r, S(HIGHLIGHT_RADIUS), fill);
    gfx_rrect_ring(dst, (float)r.x, (float)r.y, (float)r.w, (float)r.h, SF(HIGHLIGHT_RADIUS), SF(1), border);
}

void rb_frame(surface *dst, rect r, int hovered, int pressed, int checked)
{
    if (pressed)
        highlight(dst, r, PAL_PRESSED, PAL_PRESSED_BORDER);
    else if (checked && hovered)
        highlight(dst, r, CHECKED_HOVER, PAL_PRESSED_BORDER);
    else if (checked)
        highlight(dst, r, PAL_CHECKED, PAL_CHECKED_BORDER);
    else if (hovered)
        highlight(dst, r, PAL_HOVER, PAL_HOVER_BORDER);
}

/* Rounded at the top only (tabs) */
static void round_top(surface *dst, rect r, int radius, color c)
{
    rect old = gfx_clip(dst, r);
    gfx_round(dst, rect_make(r.x, r.y, r.w, r.h + radius), radius, c);
    dst->clip = old;
}

/* ---- buttons ---- */

/* Width needed for a big button's label, with room for the arrow */
static int label_width(const char *label, int dropdown)
{
    font *f = label_font();
    int w = 0;
    for (const char *s = label;;)
    {
        const char *nl = strchr(s, '\n');
        int n = nl ? (int)(nl - s) : (int)strlen(s);
        int lw = text_width(f, s, n) + (dropdown && !nl ? S(11) : 0);
        if (lw > w)
            w = lw;
        if (!nl)
            break;
        s = nl + 1;
    }
    w += S(10);
    return w < S(44) ? S(44) : w;
}

/* A big button's label lines from y, the last one ending with the arrow for dropdowns */
static void draw_label(float cx, int y, const char *label, int dropdown, int enabled)
{
    font *f = label_font();
    color c = text_color(enabled);
    const char *s = label;
    int lines = 1;
    for (const char *p = label; *p; p++)
        lines += *p == '\n';
    int total_lines = lines + (dropdown && lines == 1);
    for (int i = 0; i < total_lines; i++)
    {
        const char *nl = s ? strchr(s, '\n') : NULL;
        int n = s ? (nl ? (int)(nl - s) : (int)strlen(s)) : 0;
        int arrow = dropdown && i == total_lines - 1;
        int tw = n ? text_width(f, s, n) : 0;
        float gap = arrow && tw > 0 ? SF(2) : 0;
        float total = tw + gap + (arrow ? SF(9) : 0);
        int x0 = (int)roundf(cx - total / 2);
        if (n)
            text_draw(K.dst, f, x0, y, s, n, c);
        if (arrow)
            icon_at(K.dst, ICON_DROPDOWN, 0, x0 + tw + gap + SF(4.5f), y + SF(7.5f), S(12), enabled);
        y += S(13);
        s = nl ? nl + 1 : NULL;
    }
}

static kit_resp big_frame(const char *label, int dropdown, int checked, int enabled)
{
    rect r = kit_alloc_px(label_width(label, dropdown), S(CONTENT_H));
    kit_resp res = kit_interact_off(r, kit_id_of(label), SENSE_CLICK, enabled);
    rb_frame(K.dst, r, res.hovered, res.held, checked);
    draw_label(r.x + r.w / 2.0f, r.y + S(39), label, dropdown, enabled);
    return res;
}

kit_resp rb_big_button(int icon, uint32_t arg, const char *label, int dropdown, int checked, int enabled)
{
    kit_resp res = big_frame(label, dropdown, checked, enabled);
    icon_at(K.dst, icon, arg, res.r.x + res.r.w / 2.0f, res.r.y + SF(21), S(32), enabled);
    return res;
}

kit_resp rb_big_button_custom(const char *label, int dropdown, int checked, int enabled, rect *picture)
{
    kit_resp res = big_frame(label, dropdown, checked, enabled);
    int side = S(32);
    *picture = rect_make((int)roundf(res.r.x + res.r.w / 2.0f - side / 2.0f), (int)roundf(res.r.y + SF(21) - side / 2.0f), side, side);
    return res;
}

kit_resp rb_big_split(int icon, uint32_t arg, const char *label, int checked, int enabled, kit_resp *menu)
{
    rect r = kit_alloc_px(label_width(label, 1), S(CONTENT_H));
    rect top = rect_make(r.x, r.y, r.w, S(38));
    rect bottom = rect_make(r.x, top.y + top.h, r.w, r.h - top.h);
    kit_push_id(label);
    kit_resp main = kit_interact_off(top, kit_id_of("main"), SENSE_CLICK, enabled);
    *menu = kit_interact_off(bottom, kit_id_of("menu"), SENSE_CLICK, enabled);
    kit_pop_id();
    /* As in app.rs, an open menu doesn't keep the button lit: only hover and press do */
    int main_h = main.hovered, main_p = main.held, menu_h = menu->hovered, menu_p = menu->held;
    surface *d = K.dst;
    int rad = S(HIGHLIGHT_RADIUS);
    if (checked)
        round_top(d, top, rad, main_h ? CHECKED_HOVER : PAL_CHECKED);
    if (main_h || menu_h)
    {
        color lighter = COLOR(0xF2F7FC);
        if (!checked)
            round_top(d, top, rad, main_p ? PAL_PRESSED : main_h ? PAL_HOVER : lighter);
        /* rounded at the bottom only */
        rect old = gfx_clip(d, bottom);
        gfx_round(d, rect_make(bottom.x, bottom.y - rad, bottom.w, bottom.h + rad), rad, menu_p ? PAL_PRESSED : menu_h ? PAL_HOVER : lighter);
        d->clip = old;
        hair_h(d, r.x, r.x + r.w - 1, top.y + top.h, PAL_HOVER_BORDER);
    }
    if (main_h || menu_h || checked)
        gfx_rrect_ring(d, (float)r.x, (float)r.y, (float)r.w, (float)r.h, SF(HIGHLIGHT_RADIUS), SF(1), checked ? PAL_CHECKED_BORDER : PAL_HOVER_BORDER);
    icon_at(d, icon, arg, top.x + top.w / 2.0f, top.y + top.h / 2.0f + SF(1), S(32), enabled);
    draw_label(r.x + r.w / 2.0f, bottom.y + S(1), label, 1, enabled);
    return main;
}

kit_resp rb_small_button(int icon, uint32_t arg, const char *label, int dropdown, int checked, int enabled)
{
    font *f = label_font();
    int tw = text_width(f, label, -1), icon_w = icon >= 0 ? S(20) : 0;
    rect r = kit_alloc_px(S(4) + icon_w + tw + (dropdown ? S(14) : S(6)), S(SMALL_H));
    kit_resp res = kit_interact_off(r, kit_id_of(label), SENSE_CLICK, enabled);
    rb_frame(K.dst, r, res.hovered, res.held, checked);
    float cy = r.y + r.h / 2.0f;
    if (icon >= 0)
        icon_at(K.dst, icon, arg, r.x + SF(12), cy, S(16), enabled);
    int tx = r.x + S(4) + icon_w;
    text_draw(K.dst, f, tx, (int)roundf(cy - font_height(f) / 2.0f), label, -1, text_color(enabled));
    if (dropdown)
        icon_at(K.dst, ICON_DROPDOWN, 0, tx + tw + SF(7), cy, S(12), enabled);
    return res;
}

kit_resp rb_icon_button(int icon, uint32_t arg, int checked, int enabled, const char *tooltip)
{
    rect r = kit_alloc_px(S(23), S(SMALL_H));
    kit_resp res = kit_interact_off(r, kit_id_n(tooltip ? tooltip : "icon", icon), SENSE_CLICK, enabled);
    rb_frame(K.dst, r, res.hovered, res.held, checked);
    icon_at(K.dst, icon, arg, r.x + r.w / 2.0f, r.y + r.h / 2.0f, S(16), enabled);
    kit_tooltip(&res, tooltip);
    return res;
}

/* ---- groups ---- */

static struct
{
    const char *caption;
} groups[8];
static int n_groups;

void rb_group_begin(const char *caption, int content_h)
{
    groups[n_groups < 8 ? n_groups++ : 7].caption = caption;
    kit_col(0);
    kit_col(0);
    /* At least as wide as the caption, so it never gets cut off */
    lay *l = &K.lay[K.nl - 1];
    int cw = caption ? text_width(label_font(), caption, -1) + S(12) : 0;
    if (l->max.x + cw > l->ex)
        l->ex = l->max.x + cw;
    kit_min_height(CONTENT_H);
    if (content_h < CONTENT_H)
        kit_space_px(S((CONTENT_H - content_h) * 0.5f));
    kit_row(2);
}

void rb_group_end(void)
{
    const char *caption = n_groups ? groups[--n_groups].caption : NULL;
    kit_end_layout();
    rect content = kit_end_layout();
    rect r = kit_alloc_px(content.w, S(17));
    if (caption)
        text_in(K.dst, label_font(), rect_make(r.x, r.y + S(1), r.w, r.h), ALIGN_CENTER, caption, PAL_TEXT_WEAK);
    kit_end_layout();
    rect sep = kit_alloc_px(S(9), S(CONTENT_H + 17));
    hair_v(K.dst, sep.x + sep.w / 2, sep.y + S(2), sep.y + sep.h - S(2) - 1, PAL_GROUP_SEPARATOR);
}

/* ---- tabs ---- */

kit_resp rb_file_tab(void)
{
    font *f = ui_font(FONT_BODY, 0);
    rect r = kit_alloc(56, TAB_STRIP_H);
    kit_resp res = kit_interact(r, kit_id_of("File"), SENSE_CLICK);
    round_top(K.dst, r, S(TAB_RADIUS), res.hovered ? PAL_FILE_TAB_HOVER : PAL_FILE_TAB);
    text_in(K.dst, f, rect_make(r.x, r.y + S(1), r.w, r.h), ALIGN_CENTER, "File", COLOR(0xFFFFFF));
    return res;
}

void rb_selected_tab_outline(rect r)
{
    surface *d = K.dst;
    float fl = SF(TAB_FLARE), tr = SF(TAB_RADIUS), h = SF(0.5f);
    float x0 = r.x + h, x1 = r.x + r.w - h, y0 = r.y + h, base = r.y + r.h - h;
    float pts[2 * 40];
    int n = 0;
    /* The two flares: the corner between the tab, the baseline and the curve, in the ribbon colour */
    for (int side = 0; side < 2; side++)
    {
        float cx = side ? x1 + fl : x0 - fl, cy = base - fl;
        float fill[2 * 10];
        fill[0] = side ? (float)(r.x + r.w) : (float)r.x;
        fill[1] = (float)(r.y + r.h);
        for (int i = 0; i <= 8; i++)
        {
            float t = side ? 3.14159265f - i * 1.5707963f / 8 : i * 1.5707963f / 8;
            fill[2 + 2 * i] = cx + cosf(t) * fl;
            fill[3 + 2 * i] = cy + sinf(t) * fl;
        }
        gfx_polygon(d, fill, 10, PAL_RIBBON);
    }
    /* Outline: up from the left flare, round the top, down into the right flare */
    struct
    {
        float cx, cy, r, from, to;
    } arcs[4] = { { x0 - fl, base - fl, fl, 1.5707963f, 0 },
                  { x0 + tr, y0 + tr, tr, 3.14159265f, 4.71238898f },
                  { x1 - tr, y0 + tr, tr, 4.71238898f, 6.2831853f },
                  { x1 + fl, base - fl, fl, 3.14159265f, 1.5707963f } };
    for (int a = 0; a < 4; a++)
        for (int i = 0; i <= 8; i++)
        {
            float t = arcs[a].from + (arcs[a].to - arcs[a].from) * i / 8;
            pts[2 * n] = arcs[a].cx + cosf(t) * arcs[a].r;
            pts[2 * n + 1] = arcs[a].cy + sinf(t) * arcs[a].r;
            n++;
        }
    gfx_stroke(d, pts, n, SF(1), PAL_RIBBON_BORDER, GFX_MITER | GFX_BUTT);
}

kit_resp rb_tab(const char *label, int selected, int contextual)
{
    font *f = ui_font(FONT_BODY, 0);
    rect r = kit_alloc_px(text_width(f, label, -1) + S(26), S(TAB_STRIP_H));
    kit_resp res = kit_interact(r, kit_id_of(label), SENSE_CLICK);
    surface *d = K.dst;
    int rad = S(TAB_RADIUS);
    if (contextual)
    {
        round_top(d, r, rad, selected ? PAL_RIBBON : COLOR(0xFFF6D5));
        /* The coloured band follows the rounded top edge */
        rect old = gfx_clip(d, rect_make(r.x, r.y, r.w, S(3)));
        round_top(d, r, rad, PAL_CONTEXTUAL);
        d->clip = old;
    }
    else if (selected)
        round_top(d, r, rad, PAL_RIBBON);
    else if (res.hovered)
        /* Full height, so the highlight meets the line under the tabs */
        round_top(d, rect_make(r.x, r.y + S(2), r.w, r.h - S(2)), rad, PAL_HOVER);
    if (selected)
        rb_selected_tab_outline(r);
    text_in(d, f, rect_make(r.x, r.y + S(1), r.w, r.h), ALIGN_CENTER, label, selected || res.hovered ? PAL_TEXT : COLOR(0x444444));
    return res;
}

void rb_tab_line(int x0, int x1, int y, const rect *selected)
{
    if (!selected)
    {
        hair_h(K.dst, x0, x1 - 1, y, PAL_RIBBON_BORDER);
        return;
    }
    int fl = S(TAB_FLARE);
    hair_h(K.dst, x0, selected->x - fl - 1, y, PAL_RIBBON_BORDER);
    hair_h(K.dst, selected->x + selected->w + fl, x1 - 1, y, PAL_RIBBON_BORDER);
}

/* ---- menus ---- */

void rb_menu_header(const char *text)
{
    rect r = kit_alloc_px(kit_avail().w, S(22));
    gfx_fill(K.dst, r, COLOR(0xEEEFF1));
    hair_h(K.dst, r.x, r.x + r.w - 1, r.y + r.h - HAIR(), PAL_RIBBON_BORDER);
    text_in(K.dst, ui_font(12, 0), rect_make(r.x + S(8), r.y, r.w - S(8), r.h), ALIGN_LEFT, text, PAL_TEXT);
}

kit_resp rb_menu_item(int icon, uint32_t arg, const char *label, int checked, int enabled)
{
    int w = kit_avail().w;
    if (w < S(160))
        w = S(160);
    rect r = kit_alloc_px(w, S(26));
    kit_resp res = kit_interact_off(r, kit_id_of(label), SENSE_CLICK, enabled);
    rb_frame(K.dst, rect_inset(r, 1), res.hovered, res.held, 0);
    rect gutter = rect_make(r.x + S(3), r.y + S(3), S(20), S(20));
    if (checked)
        highlight(K.dst, gutter, PAL_CHECKED, PAL_CHECKED_BORDER);
    float cx = gutter.x + gutter.w / 2.0f, cy = gutter.y + gutter.h / 2.0f;
    if (icon >= 0)
        icon_at(K.dst, icon, arg, cx, cy, S(16), enabled);
    else if (checked)
        icon_at(K.dst, ICON_CHECK, 0, cx, cy, S(16), enabled);
    text_in(K.dst, ui_font(HIGHLIGHT_TEXT_PT, 0), rect_make(r.x + S(32), r.y, r.w - S(36), r.h), ALIGN_LEFT, label, text_color(enabled));
    return res;
}

kit_resp rb_file_menu_item(int icon, const char *label, int arrow, int highlighted)
{
    rect r = kit_alloc_px(kit_avail().w, S(44));
    kit_resp res = kit_interact(r, kit_id_of(label), SENSE_CLICK);
    rb_frame(K.dst, r, res.hovered || highlighted, res.held, 0);
    icon_at(K.dst, icon, 0, r.x + SF(24), r.y + r.h / 2.0f, S(32), 1);
    text_in(K.dst, ui_font(13, 0), rect_make(r.x + S(50), r.y, r.w - S(70), r.h), ALIGN_LEFT, label, PAL_TEXT);
    if (arrow)
    {
        float cx = r.x + r.w - SF(12), cy = r.y + r.h / 2.0f;
        float tri[6] = { cx - SF(3), cy - SF(5), cx + SF(3), cy, cx - SF(3), cy + SF(5) };
        gfx_polygon(K.dst, tri, 3, PAL_TEXT);
    }
    return res;
}

/* ---- swatches ---- */

void rb_paint_swatch(surface *dst, rect r, color c, int hovered)
{
    gfx_fill(dst, r, COLOR(0xFFFFFF));
    gfx_fill(dst, rect_inset(r, S(2)), c);
    gfx_frame(dst, r, HAIR(), hovered ? PAL_PRESSED_BORDER : COLOR(0xA0A0A0));
}

kit_resp rb_swatch(color c, int side_pt)
{
    rect r = kit_alloc(side_pt, side_pt);
    kit_resp res = kit_interact(r, kit_id_n("swatch", (int)c), SENSE_CLICK);
    rb_paint_swatch(K.dst, rect_inset(r, 1), c, res.hovered);
    return res;
}

void rb_empty_swatch(int side_pt)
{
    rect r = rect_inset(kit_alloc(side_pt, side_pt), 1);
    gfx_fill(K.dst, r, COLOR(0xFFFFFF));
    gfx_frame(K.dst, r, HAIR(), COLOR(0xC8C8C8));
}

/* ---- the shape gallery, the size menu ---- */

int rb_gallery(int icon, int n, int cols, int rows, int cell_pt, int checked, const char *const *names, kit_resp *more)
{
    int cell = S((float)cell_pt), strip_w = S(15);
    rect r = kit_alloc_px(cols * cell + S(2) + strip_w, rows * cell + S(2));
    surface *d = K.dst;
    int picked = -1;
    gfx_fill(d, r, COLOR(0xFFFFFF));
    for (int i = 0; i < n && i < cols * rows; i++)
    {
        rect c = rect_make(r.x + S(1) + (i % cols) * cell, r.y + S(1) + (i / cols) * cell, cell, cell);
        kit_resp res = kit_interact(c, kit_id_n("cell", i), SENSE_CLICK);
        if (i == checked || res.hovered)
            highlight(d, c, i == checked ? PAL_CHECKED : PAL_HOVER, i == checked ? PAL_CHECKED_BORDER : PAL_HOVER_BORDER);
        icon_at(d, icon, (uint32_t)i, c.x + c.w / 2.0f, c.y + c.h / 2.0f, S(16), 1);
        if (names)
            kit_tooltip(&res, names[i]);
        if (res.clicked)
            picked = i;
    }
    /* Paint's gallery scroll strip: up and down (always at the end here) and "more" */
    rect strip = rect_make(r.x + r.w - strip_w, r.y, strip_w, r.h);
    gfx_fill(d, strip, COLOR(0xF0F1F2));
    hair_v(d, strip.x, strip.y, strip.y + strip.h - 1, PAL_RIBBON_BORDER);
    for (int k = 0; k < 3; k++)
    {
        int y0 = strip.y + strip.h * k / 3, y1 = strip.y + strip.h * (k + 1) / 3;
        rect b = rect_make(strip.x + S(1), y0, S(14), y1 - y0);
        int enabled = k == 2;
        kit_resp res = kit_interact_off(b, kit_id_n("strip", k), SENSE_CLICK, enabled);
        if (enabled && res.hovered)
            highlight(d, b, PAL_HOVER, PAL_HOVER_BORDER);
        icon_at(d, k == 0 ? ICON_SCROLL_UP : k == 1 ? ICON_SCROLL_DOWN : ICON_GALLERY_MORE, 0, b.x + b.w / 2.0f, b.y + b.h / 2.0f, S(12), enabled);
        if (enabled && more)
            *more = res;
    }
    gfx_frame(d, r, HAIR(), COLOR(0xC5C5C5));
    return picked;
}

kit_resp rb_size_row(float size, int checked)
{
    rect r = kit_alloc_px(kit_avail().w, S(30));
    kit_resp res = kit_interact(r, kit_id_n("size", (int)(size * 10)), SENSE_CLICK);
    if (checked || res.hovered)
        highlight(K.dst, rect_inset(r, 1), checked ? PAL_CHECKED : PAL_HOVER, checked ? PAL_CHECKED_BORDER : PAL_HOVER_BORDER);
    /* The bar is the real thickness in picture pixels, scaled with the UI */
    float cy = r.y + r.h / 2.0f, half = SF(size) / 2;
    int y0 = (int)roundf(cy - half), y1 = (int)roundf(cy + half);
    if (y1 <= y0)
        y1 = y0 + 1;
    gfx_fill(K.dst, rect_make(r.x + S(10), y0, r.w - S(62), y1 - y0), PAL_TEXT);
    char text[16];
    snprintf(text, sizeof text, "%.0fpx", size);
    text_in(K.dst, ui_font(11.5f, 0), rect_make(r.x, r.y, r.w - S(10), r.h), ALIGN_RIGHT, text, PAL_TEXT_WEAK);
    return res;
}

int rb_value_slider(float *value, float min, float max, int log, const char *suffix)
{
    rect r = kit_alloc_px(kit_avail().w, S(26));
    kit_resp res = kit_interact(r, kit_id_of("value-slider"), SENSE_DRAG);
    rect track = rect_make(r.x + S(8), r.y, r.w - S(64), r.h);
    float lo = log ? logf(min) : min, hi = log ? logf(max) : max;
    int changed = 0;
    if (res.held || res.clicked)
    {
        float t = (res.x - track.x) / (float)(track.w > 0 ? track.w : 1);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float v = lo + t * (hi - lo);
        v = roundf(log ? expf(v) : v);
        v = v < min ? min : v > max ? max : v;
        changed = v != *value;
        *value = v;
    }
    float t = ((log ? logf(*value) : *value) - lo) / (hi - lo);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float y = r.y + r.h / 2.0f, x = track.x + track.w * t;
    int ty = (int)roundf(y - SF(1));
    gfx_fill(K.dst, rect_make(track.x, ty, track.w, S(2)), COLOR(0xC4C4C4));
    gfx_fill(K.dst, rect_make(track.x, ty, (int)roundf(x) - track.x, S(2)), PAL_ACCENT);
    gfx_rrect(K.dst, roundf(x - SF(4)), roundf(y - SF(9)), SF(8), SF(18), SF(4), res.hovered || res.held ? COLOR(0x171717) : PAL_ACCENT);
    char text[32];
    snprintf(text, sizeof text, "%.0f%s", *value, suffix ? suffix : "");
    text_in(K.dst, ui_font(12, 0), rect_make(r.x, r.y, r.w - S(6), r.h), ALIGN_RIGHT, text, PAL_TEXT);
    return changed;
}

/* ---- the status bar ---- */

void rb_status_item(int icon, const char *text, int width_pt)
{
    rect r = kit_alloc(width_pt, 22);
    float cy = r.y + r.h / 2.0f;
    icon_at(K.dst, icon, 0, r.x + SF(10), cy, S(16), 1);
    text_in(K.dst, ui_font(12, 0), rect_make(r.x + S(24), r.y, r.w - S(26), r.h), ALIGN_LEFT, text, PAL_TEXT);
    hair_v(K.dst, r.x + r.w - HAIR(), r.y + S(2), r.y + r.h - S(2) - 1, PAL_STATUS_BORDER);
}

kit_resp rb_status_link(int icon, const char *text)
{
    font *f = ui_font(12, 0);
    rect r = kit_alloc_px(text_width(f, text, -1) + S(34), S(22));
    kit_resp res = kit_interact(r, kit_id_of(text), SENSE_CLICK);
    kit_hover_cursor(&res, CURSOR_HAND);
    if (res.hovered)
        gfx_round(K.dst, rect_make(r.x + S(1), r.y + S(2), r.w - S(2), r.h - S(4)), S(HIGHLIGHT_RADIUS), PAL_HOVER);
    float cy = r.y + r.h / 2.0f;
    icon_at(K.dst, icon, 0, r.x + SF(12), cy, S(16), 1);
    text_in(K.dst, f, rect_make(r.x + S(26), r.y, r.w - S(26), r.h), ALIGN_LEFT, text, PAL_ACCENT);
    hair_v(K.dst, r.x + r.w - HAIR(), r.y + S(2), r.y + r.h - S(2) - 1, PAL_STATUS_BORDER);
    return res;
}

int rb_zoom_slider(float *t, float tick, int width_pt)
{
    rect r = kit_alloc(width_pt, 22);
    kit_resp res = kit_interact(r, kit_id_of("zoom-slider"), SENSE_DRAG);
    float x0 = r.x + SF(4), x1 = r.x + r.w - SF(4);
    int changed = 0;
    if (res.held || res.clicked)
    {
        float nt = (res.x - x0) / (x1 - x0);
        nt = nt < 0 ? 0 : nt > 1 ? 1 : nt;
        /* Snap to 100% */
        if (fabsf(nt - tick) < 0.015f)
            nt = tick;
        changed = nt != *t;
        *t = nt;
    }
    int y = r.y + r.h / 2;
    color grey = COLOR(0x9A9A9A);
    hair_h(K.dst, (int)x0, (int)x1, y, grey);
    int tx = (int)roundf(x0 + (x1 - x0) * tick);
    hair_v(K.dst, tx, y - S(4), y + S(4), grey);
    float x = x0 + (x1 - x0) * *t;
    rect thumb = rect_make((int)roundf(x - SF(3)), (int)roundf(r.y + r.h / 2.0f - SF(7)), S(6), S(14));
    gfx_fill(K.dst, thumb, res.hovered || res.held ? PAL_ACCENT : COLOR(0x606060));
    return changed;
}

kit_resp rb_status_button(int minus)
{
    rect r = kit_alloc(18, 18);
    kit_resp res = kit_interact(r, kit_id_of(minus ? "zoom-out" : "zoom-in"), SENSE_CLICK);
    rb_frame(K.dst, r, res.hovered, res.held, 0);
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, k = SF(4.5f);
    color c = COLOR(0x404040);
    gfx_stroke(K.dst, (float[]){ cx - k, cy, cx + k, cy }, 2, SF(1.5f), c, GFX_BUTT);
    if (!minus)
        gfx_stroke(K.dst, (float[]){ cx, cy - k, cx, cy + k }, 2, SF(1.5f), c, GFX_BUTT);
    return res;
}

/* ---- drawing together ---- */

/* The first character of a name, upper-cased if it's a plain letter */
static void initial_of(const char *name, char out[8])
{
    int n = 1;
    while (name[n] && ((unsigned char)name[n] & 0xC0) == 0x80 && n < 4)
        n++;
    memcpy(out, name, (size_t)(name[0] ? n : 0));
    out[name[0] ? n : 0] = 0;
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] = (char)(out[0] - 32);
}

static void badge(rect r, const char *name, color c, float text_pt)
{
    char ini[8];
    initial_of(name, ini);
    gfx_circle(K.dst, r.x + r.w / 2.0f, r.y + r.h / 2.0f, r.w / 2.0f, c);
    text_in(K.dst, ui_font(text_pt, 0), r, ALIGN_CENTER, ini, COLOR(0xFFFFFF));
}

void rb_avatar(const char *name, color c, int size_pt)
{
    badge(kit_alloc(size_pt, size_pt), name, c, size_pt * 0.6f);
}

int rb_kid_row(const char *label, const char *name, color c, const char *kick_tip)
{
    kit_push_id(label);
    kit_row_centered(5, INTERACT_H);
    rb_avatar(name, c, 16);
    kit_label(label);
    kit_resp kick = { 0 };
    if (kick_tip)
        kick = kit_interact(kit_alloc(14, 14), kit_id_of("kick"), SENSE_CLICK);
    rect row = kit_end_layout();
    kit_pop_id();
    if (!kick_tip)
        return 0;
    kit_tooltip(&kick, kick_tip);
    kit_hover_cursor(&kick, CURSOR_HAND);
    if (hover_ok(row))
    {
        color red = COLOR(0xC42B1C);
        float cx = kick.r.x + kick.r.w / 2.0f, cy = kick.r.y + kick.r.h / 2.0f, d = SF(3);
        if (kick.hovered)
            gfx_circle(K.dst, cx, cy, SF(7), COLOR_A(0xC42B1C, 38));
        color ink = kick.hovered ? red : COLOR_A(0x1E1E1E, 153);
        gfx_line(K.dst, cx - d, cy - d, cx + d, cy + d, SF(1.5f), ink);
        gfx_line(K.dst, cx - d, cy + d, cx + d, cy - d, SF(1.5f), ink);
    }
    return kick.clicked;
}

kit_resp rb_nearby_row(const char *name, const char *text, int compatible)
{
    rect r = kit_alloc_px(kit_avail().w, S(30));
    kit_resp res = kit_interact(r, kit_id_of(name), SENSE_CLICK);
    kit_hover_cursor(&res, compatible ? CURSOR_HAND : CURSOR_ARROW);
    if (res.hovered && compatible)
        highlight(K.dst, r, PAL_HOVER, PAL_HOVER_BORDER);
    int d = S(20);
    badge(rect_make(r.x + S(16) - d / 2, r.y + (r.h - d) / 2, d, d), name, COLOR(0x4C9BE8), 12);
    text_in(K.dst, ui_font(13, 0), rect_make(r.x + S(34), r.y, r.w - S(36), r.h), ALIGN_LEFT, text, compatible ? PAL_TEXT : PAL_TEXT_WEAK);
    return res;
}

kit_resp rb_more_button(const char *count)
{
    rect r = kit_alloc(26, CONTENT_H - 8);
    kit_resp res = kit_interact(r, kit_id_of("more"), SENSE_CLICK);
    kit_hover_cursor(&res, CURSOR_HAND);
    if (res.hovered || res.held)
        highlight(K.dst, r, PAL_HOVER, PAL_HOVER_BORDER);
    else
        gfx_rrect_ring(K.dst, (float)r.x, (float)r.y, (float)r.w, (float)r.h, SF(HIGHLIGHT_RADIUS), SF(1), PAL_RIBBON_BORDER);
    text_in(K.dst, ui_font(11, 0), rect_make(r.x, r.y + S(8), r.w, S(16)), ALIGN_CENTER, count, PAL_TEXT);
    icon_at(K.dst, ICON_GALLERY_MORE, 0, r.x + r.w / 2.0f, r.y + r.h - SF(14), S(12), 1);
    return res;
}

void rb_door_pill_size(const char *who, const char *text, int tick, int *w, int *h)
{
    *w = S(who ? 8 : 16) + (who ? S(22 + 8) : 0) + text_width(ui_font(13, 0), text, -1) + S(8 + 2) + (tick ? S(24 + 5) : 0) + S(24) + S(6);
    *h = S(6 + 24 + 6);
}

/* A round tick (green) or cross (red) button */
static kit_resp answer_button(int yes, const char *tip)
{
    rect r = kit_alloc(24, 24);
    kit_resp res = kit_interact(r, kit_id_of(yes ? "yes" : "no"), SENSE_CLICK);
    kit_tooltip(&res, tip);
    kit_hover_cursor(&res, CURSOR_HAND);
    color base = yes ? COLOR(0x2E9E44) : COLOR(0xD23A32), lit = yes ? COLOR(0x3DB856) : COLOR(0xE84E45);
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, w = SF(2.2f);
    gfx_circle(K.dst, cx, cy, SF(12), res.hovered ? lit : base);
    color white = COLOR(0xFFFFFF);
    if (yes)
        gfx_stroke(K.dst, (float[]){ cx - SF(5), cy + SF(0.5f), cx - SF(1.5f), cy + SF(4), cx + SF(5), cy - SF(3.5f) }, 3, w, white, GFX_MITER | GFX_BUTT);
    else
    {
        gfx_stroke(K.dst, (float[]){ cx - SF(4), cy - SF(4), cx + SF(4), cy + SF(4) }, 2, w, white, GFX_BUTT);
        gfx_stroke(K.dst, (float[]){ cx - SF(4), cy + SF(4), cx + SF(4), cy - SF(4) }, 2, w, white, GFX_BUTT);
    }
    return res;
}

int rb_door_pill(const char *who, color who_color, const char *text, const char *tick_tip, const char *cross_tip)
{
    int w, h, answer = -1;
    rb_door_pill_size(who, text, tick_tip != NULL, &w, &h);
    rect r = kit_alloc_px(w, h);
    gfx_round(K.dst, r, S(18), COLOR_A(0x282830, 235));
    kit_push_id(text);
    kit_area(rect_make(r.x + S(who ? 8 : 16), r.y + S(6), r.w, S(24)), 1, 8);
    lay *l = &K.lay[K.nl - 1];
    l->cross = S(24);
    if (who)
        rb_avatar(who, who_color, 22);
    kit_label_ex(text, 13, 0, COLOR(0xFFFFFF));
    kit_space(5);   /* egui: the label's 8 of spacing and 2 more, then 5 between the buttons */
    kit_set_gap(5);
    if (tick_tip && answer_button(1, tick_tip).clicked)
        answer = 1;
    if (answer_button(0, cross_tip).clicked)
        answer = 0;
    kit_area_end();
    kit_pop_id();
    return answer;
}
