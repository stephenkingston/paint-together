/*
 * Paint (C port) - the dialogs (Rust: app.rs's dialogs, together_dialogs, spectrum,
 * luminance_bar and color_fields)
 *
 * Each dialog shows while the app's field for it is set, and its buttons
 * call the app's answer for it. The "type a path" dialog is the C port's
 * own: it stands in for the file picker where the platform has none.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app/view.h"

/* Buttons right to left at the bottom of a dialog: returns the one clicked (0 = the first named), or -1 */
static int buttons(const char *const *labels, int n, int default_index, int enabled_mask)
{
    int clicked = -1;
    kit_row_rtl(6, 24);
    for (int i = 0; i < n; i++)
        if (kit_dialog_button(labels[i], i == default_index, enabled_mask >> i & 1, 0).clicked)
            clicked = i;
    kit_end_layout();
    return clicked;
}

/* The widest of some labels: a grid's first column */
static int column_width(const char *const *labels, int n)
{
    int w = S(INTERACT_W);
    for (int i = 0; i < n; i++)
    {
        int lw = text_width(ui_font(FONT_BODY, 0), labels[i], -1);
        w = lw > w ? lw : w;
    }
    return w;
}

/* ---- drawing together: Join a drawing, and "for everyone?" ---- */

static void join_dialog(app *a)
{
    static char addr[64];
    static int was_open;
    if (!a->join_dialog)
    {
        was_open = 0;
        return;
    }
    if (!was_open)
        snprintf(addr, sizeof addr, "%s", a->join_dialog), was_open = 1;
    app_discover(a);
    int dismissed = 0, n;
    if (!kit_dialog_begin("join", "Join a drawing", 360, &dismissed))
        return;
    const nearby *near = app_nearby(a, &n);
    char picked[64] = "";
    if (!n)
        kit_label_ex("Looking for drawings on this Wi-Fi\xE2\x80\xA6", FONT_BODY, 0, PAL_TEXT_WEAK);
    else
    {
        kit_label("Drawings nearby:");
        kit_space(2);
        for (int i = 0; i < n; i++)
        {
            char kids[24], text[200];
            snprintf(kids, sizeof kids, near[i].kids == 1 ? "1 kid" : "%d kids", near[i].kids);
            if (near[i].compatible)
                snprintf(text, sizeof text, "%s's drawing  \xC2\xB7  %s", near[i].name, kids);
            else
                snprintf(text, sizeof text, "%s's drawing  (different version of Paint)", near[i].name);
            kit_push_n(i);
            if (rb_nearby_row(near[i].name, text, near[i].compatible).clicked && near[i].compatible)
                os_addr_format(near[i].addr, picked, sizeof picked);
            kit_pop_id();
        }
    }
    kit_space(12);
    kit_label_wrap("Can't see it? Type the address shown on your friend's Together tab:", FONT_BODY, PAL_TEXT_WEAK);
    kit_space(4);
    if (!kit_focused())
        kit_focus(kit_id_of("addr"));
    int f = kit_text_field("addr", addr, sizeof addr, 0, 0, "192.168.1.20", 0);
    kit_space(14);
    /* the address without spaces round it */
    char *s = addr;
    while (isspace((unsigned char)*s))
        s++;
    char trimmed[64];
    snprintf(trimmed, sizeof trimmed, "%s", s);
    for (size_t len = strlen(trimmed); len && isspace((unsigned char)trimmed[len - 1]);)
        trimmed[--len] = 0;
    static const char *const labels[2] = { "Cancel", "Join" };
    int b = buttons(labels, 2, 1, trimmed[0] ? 3 : 1);
    kit_dialog_end();
    if (picked[0])
        app_close_join_dialog(a), app_request(a, PENDING_JOIN, picked);
    else if ((b == 1 || f & KIT_SUBMITTED) && trimmed[0])
        app_close_join_dialog(a), app_request(a, PENDING_JOIN, trimmed);
    else if (b == 0 || dismissed)
        app_close_join_dialog(a);
}

static void everyone_dialog(app *a)
{
    if (a->confirm_everyone.kind == PENDING_NONE)
        return;
    int dismissed = 0;
    if (!kit_dialog_begin("everyone", "Paint", 380, &dismissed))
        return;
    char q[96];
    snprintf(q, sizeof q, "%s for everyone drawing with you?", a->confirm_everyone.kind == PENDING_NEW ? "Start a new picture" : "Open a picture");
    kit_label_wrap(q, FONT_BODY, PAL_TEXT);
    kit_space(4);
    kit_label_wrap("It replaces the picture on all your friends' screens too.", FONT_BODY, PAL_TEXT_WEAK);
    kit_space(14);
    static const char *const labels[2] = { "Cancel", "Continue" };
    int b = buttons(labels, 2, 1, 3);
    kit_dialog_end();
    if (b == 1)
        app_confirm_everyone_answer(a, 1);
    else if (b == 0 || dismissed)
        app_confirm_everyone_answer(a, 0);
}

/* ---- "Do you want to save changes?" ---- */

static void confirm_dialog(app *a)
{
    if (a->confirm.kind == PENDING_NONE)
        return;
    int dismissed = 0;
    if (!kit_dialog_begin("confirm", "Paint", 380, &dismissed))
        return;
    char name[256], q[320];
    snprintf(q, sizeof q, "Do you want to save changes to %s?", app_doc_name(a, name, sizeof name));
    kit_label_wrap(q, 15, DIALOG_HEADING);
    kit_space(16);
    static const char *const labels[3] = { "Cancel", "Don't Save", "Save" };
    int b = buttons(labels, 3, 2, 7);
    kit_dialog_end();
    if (b >= 0)
        app_confirm_answer(a, 2 - b);
    else if (dismissed)
        app_confirm_answer(a, 2);
}

/* ---- Resize and Skew ---- */

static void resize_dialog_view(app *a)
{
    resize_dialog *r = a->resize;
    if (!r)
        return;
    int dismissed = 0;
    if (!kit_dialog_begin("resize", "Resize and Skew", 320, &dismissed))
        return;
    kit_label_ex(a->sel ? "Resize the selection" : "Resize the whole image", FONT_BODY, 0, PAL_TEXT_WEAK);
    kit_space(8);
    kit_row_centered(ITEM_SPACING_X, INTERACT_H);
    kit_label("By:");
    int percent = r->percent;
    if (kit_radio("Percentage", percent, 1))
        app_resize_set_mode(a, 1);
    if (kit_radio("Pixels", !percent, 1))
        app_resize_set_mode(a, 0);
    kit_end_layout();
    static const char *const names[2] = { "Horizontal:", "Vertical:" };
    int col = column_width(names, 2);
    float max = r->percent ? 1000 : 30000;
    for (int i = 0; i < 2; i++)
    {
        kit_push_n(i);
        kit_row_centered(12, INTERACT_H);
        kit_label_w(names[i], col);
        float v = roundf(i ? r->h : r->w);
        if (kit_number("value", &v, 1, max, 60))
            i ? app_resize_set_h(a, v) : app_resize_set_w(a, v);
        kit_end_layout();
        kit_pop_id();
    }
    kit_checkbox("Maintain aspect ratio", &r->keep_aspect);
    kit_space(14);
    static const char *const labels[2] = { "Cancel", "OK" };
    int b = buttons(labels, 2, 1, 3);
    kit_dialog_end();
    if (b == 1)
        app_resize_ok(a);
    else if (b == 0 || dismissed)
        app_resize_cancel(a);
}

/* ---- Image Properties ---- */

static void properties_dialog(app *a)
{
    if (!a->properties)
        return;
    int dismissed = 0;
    if (!kit_dialog_begin("properties", "Image Properties", 380, &dismissed))
        return;
    char saved[64] = "Not available", size[64] = "Not available";
    if (a->has_meta)
    {
        if (a->file_mtime)
            app_format_time(a->file_mtime, saved, sizeof saved);
        app_format_bytes(a->file_size, size, sizeof size);
    }
    static const char *const names[3] = { "Last saved:", "Size on disk:", "Resolution:" };
    const char *values[3] = { saved, size, "96 DPI" };
    int col = column_width(names, 3) + S(16) - S(ITEM_SPACING_X);
    kit_group_box_begin("File attributes", 344);
    for (int i = 0; i < 3; i++)
    {
        kit_push_n(i);
        kit_row(ITEM_SPACING_X);
        kit_label_w(names[i], col);
        kit_label(values[i]);
        kit_end_layout();
        kit_pop_id();
    }
    kit_group_box_end();
    kit_row(ITEM_SPACING_X);
    kit_group_box_begin("Units", 169);
    kit_radio("Inches", 0, 0);
    kit_radio("Centimeters", 0, 0);
    kit_radio("Pixels", 1, 1);
    kit_group_box_end();
    kit_group_box_begin("Colors", 169);
    kit_radio("Black and white", 0, 0);
    kit_radio("Color", 1, 1);
    kit_group_box_end();
    kit_end_layout();
    kit_row_centered(ITEM_SPACING_X, INTERACT_H);
    kit_label("Width:");
    kit_number("w", &a->prop_w, 1, 30000, 60);
    kit_space(12);
    kit_label("Height:");
    kit_number("h", &a->prop_h, 1, 30000, 60);
    kit_end_layout();
    kit_space(12);
    static const char *const labels[2] = { "Cancel", "OK" };
    int b = buttons(labels, 2, 1, 3);
    kit_dialog_end();
    if (b == 1)
        app_properties_ok(a);
    else if (b == 0 || dismissed)
        app_properties_cancel(a);
}

/* ---- About ---- */

static void about_dialog(app *a)
{
    if (!a->about_open)
        return;
    int dismissed = 0;
    if (!kit_dialog_begin("about", "About Paint", 380, &dismissed))
        return;
    kit_row(ITEM_SPACING_X);
    rect logo = kit_alloc(64, 64);
    icon_draw(kit_dst(), ICON_LOGO, 0, logo.x, logo.y, logo.w, 1);
    kit_col(ITEM_SPACING_Y);
    kit_label_ex("Paint", 22, 0, DIALOG_HEADING);
    kit_label("Version " PAINT_VERSION);
    kit_space(6);
    kit_label_wrap("A from-scratch reimplementation of Microsoft Paint, written in C.", FONT_BODY, PAL_TEXT);
    kit_end_layout();
    kit_end_layout();
    kit_space(12);
    static const char *const labels[1] = { "OK" };
    int b = buttons(labels, 1, 0, 1);
    kit_dialog_end();
    if (b == 0 || dismissed)
        a->about_open = 0;
}

/* ---- Edit Colors ---- */

static color hsl_color(float h, float s, float l)
{
    float hsl[3] = { h, s, l };
    return view_rgba(hsl_to_rgb(hsl));
}

/* The Windows colour dialog's spectrum: hue across, saturation up, at mid luminance */
static void spectrum(float *hsl)
{
    rect r = kit_alloc(180, 170);
    kit_resp res = kit_interact(r, kit_id_of("spectrum"), SENSE_DRAG);
    if (res.held || res.clicked)
    {
        hsl[0] = fminf(1, fmaxf(0, (res.x - r.x) / (float)r.w));
        hsl[1] = fminf(1, fmaxf(0, 1 - (res.y - r.y) / (float)r.h));
        if (hsl[2] <= 0 || hsl[2] >= 1)
            hsl[2] = 0.5f;
    }
    surface *d = kit_dst();
    rect v = rect_cut(r, d->clip);
    for (int y = v.y; y < v.y + v.h; y++)
    {
        uint32_t *row = d->px + (size_t)(y - d->oy) * d->stride - d->ox;
        for (int x = v.x; x < v.x + v.w; x++)
            row[x] = hsl_color((x - r.x + 0.5f) / r.w, 1 - (y - r.y + 0.5f) / r.h, 0.5f);
    }
    gfx_frame(d, rect_inset(r, -HAIR()), HAIR(), COLOR(0x787878));
    /* Windows' crosshair */
    float cx = r.x + hsl[0] * r.w, cy = r.y + (1 - hsl[1]) * r.h;
    for (int k = 0; k < 4; k++)
    {
        float dx = k == 0 ? 1 : k == 1 ? -1 : 0, dy = k == 2 ? 1 : k == 3 ? -1 : 0;
        float seg[4] = { cx + dx * SF(3), cy + dy * SF(3), cx + dx * SF(9), cy + dy * SF(9) };
        gfx_stroke(d, seg, 2, SF(2), COLOR(0), GFX_BUTT);
    }
}

/* The luminance bar beside it, with its arrow */
static void luminance_bar(float *hsl)
{
    rect r = kit_alloc(26, 170);
    rect bar = rect_make(r.x, r.y, S(14), r.h);
    kit_resp res = kit_interact(r, kit_id_of("lum"), SENSE_DRAG);
    if (res.held || res.clicked)
        hsl[2] = fminf(1, fmaxf(0, 1 - (res.y - r.y) / (float)r.h));
    surface *d = kit_dst();
    for (int y = 0; y < bar.h; y++)
        gfx_fill(d, rect_make(bar.x, bar.y + y, bar.w, 1), hsl_color(hsl[0], hsl[1], 1 - (y + 0.5f) / bar.h));
    gfx_frame(d, rect_inset(bar, -HAIR()), HAIR(), COLOR(0x787878));
    float y = bar.y + (1 - hsl[2]) * bar.h, tx = bar.x + bar.w + SF(2);
    float tri[6] = { tx, y, tx + SF(7), y - SF(5), tx + SF(7), y + SF(5) };
    gfx_polygon(d, tri, 3, COLOR(0));
}

/* Hue/Sat/Lum on Windows' 0-240 scale, and Red/Green/Blue */
static void color_fields(float *hsl)
{
    static const char *const left[3] = { "Hue:", "Sat:", "Lum:" }, *const right[3] = { "Red:", "Green:", "Blue:" };
    int lw = column_width(left, 3), rw = column_width(right, 3);
    rgba c = hsl_to_rgb(hsl);
    uint8_t *ch[3] = { &c.r, &c.g, &c.b };
    kit_col(4);
    for (int i = 0; i < 3; i++)
    {
        kit_push_n(i);
        kit_row_centered(6, INTERACT_H);
        kit_label_w(left[i], lw);
        float max = i ? 240 : 239, v = roundf(hsl[i] * max);
        if (kit_number("hsl", &v, 0, max, 36))
            hsl[i] = v / max;
        kit_label_w(right[i], rw);
        float cv = *ch[i];
        if (kit_number("rgb", &cv, 0, 255, 36))
        {
            *ch[i] = (uint8_t)cv;
            rgb_to_hsl(c, hsl);
        }
        kit_end_layout();
        kit_pop_id();
    }
    kit_end_layout();
}

static void edit_colors_dialog(app *a)
{
    if (!a->edit_colors)
        return;
    int dismissed = 0;
    if (!kit_dialog_begin("edit-colors", "Edit Colors", 520, &dismissed))
        return;
    kit_row(ITEM_SPACING_X);
    kit_col(ITEM_SPACING_Y);
    kit_label("Basic colors:");
    for (int row = 0; row < 6; row++)
    {
        kit_row(2);
        for (int c = 0; c < 8; c++)
        {
            uint32_t v = APP_BASIC_COLORS[row * 8 + c];
            kit_push_n(row * 8 + c);
            if (rb_swatch(COLOR(v), 22).clicked)
                rgb_to_hsl((rgba){ (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v, 255 }, a->hsl);
            kit_pop_id();
        }
        kit_end_layout();
    }
    kit_space(8);
    kit_label("Custom colors:");
    for (int row = 0; row < 2; row++)
    {
        kit_row(2);
        for (int c = 0; c < 5; c++)
        {
            int i = row * 5 + c;
            if (!a->has_custom[i])
            {
                rb_empty_swatch(22);
                continue;
            }
            kit_push_n(100 + i);
            if (rb_swatch(view_rgba(a->custom[i]), 22).clicked)
                rgb_to_hsl(a->custom[i], a->hsl);
            kit_pop_id();
        }
        kit_end_layout();
    }
    kit_end_layout();
    kit_space(10);
    kit_col(ITEM_SPACING_Y);
    kit_row(ITEM_SPACING_X);
    spectrum(a->hsl);
    kit_space(4);
    luminance_bar(a->hsl);
    kit_end_layout();
    kit_space(8);
    kit_row(ITEM_SPACING_X);
    kit_col(ITEM_SPACING_Y);
    rect sw = kit_alloc(66, 42);
    rb_paint_swatch(kit_dst(), sw, view_rgba(hsl_to_rgb(a->hsl)), 0);
    kit_label("Color|Solid");
    kit_end_layout();
    kit_space(8);
    color_fields(a->hsl);
    kit_end_layout();
    kit_space(6);
    if (kit_dialog_button("Add to Custom Colors", 0, 1, 220).clicked)
        app_add_custom_color(a, hsl_to_rgb(a->hsl));
    kit_end_layout();
    kit_end_layout();
    kit_space(12);
    static const char *const labels[2] = { "Cancel", "OK" };
    int b = buttons(labels, 2, 1, 3);
    kit_dialog_end();
    if (b == 1)
        app_edit_colors_ok(a);
    else if (b == 0 || dismissed)
        app_edit_colors_cancel(a);
}

/* ---- "Type a path" (no file picker on this system) ---- */

static void ask_path_dialog(app *a)
{
    static char path[1024];
    static int was;
    if (a->ask_kind == ASK_NONE)
    {
        was = ASK_NONE;
        return;
    }
    if (was != a->ask_kind)
        snprintf(path, sizeof path, "%s", a->ask_name), was = a->ask_kind;
    int dismissed = 0;
    const char *title = a->ask_kind == ASK_SAVE ? "Save as" : a->ask_kind == ASK_PASTE_FROM ? "Paste from" : "Open";
    if (!kit_dialog_begin("ask-path", title, 420, &dismissed))
        return;
    kit_label_wrap(a->ask_kind == ASK_SAVE ? "Type where to save the picture (its folder and file name):" : "Type the path of the picture:",
                   FONT_BODY, PAL_TEXT);
    kit_space(4);
    if (!kit_focused())
        kit_focus(kit_id_of("path"));
    int f = kit_text_field("path", path, sizeof path, 0, 0, a->ask_kind == ASK_SAVE ? "~/Pictures/picture.png" : "~/Pictures/picture.png", 0);
    kit_space(14);
    static const char *const labels[2] = { "Cancel", "OK" };
    int b = buttons(labels, 2, 1, path[0] ? 3 : 1);
    kit_dialog_end();
    if ((b == 1 || f & KIT_SUBMITTED) && path[0])
        app_ask_path_ok(a, path);
    else if (b == 0 || dismissed)
        app_ask_path_cancel(a);
}

/* ---- a message ---- */

static void error_dialog(app *a)
{
    if (!a->error)
        return;
    int dismissed = 0;
    if (!kit_dialog_begin("error", "Paint", 400, &dismissed))
        return;
    kit_label_wrap(a->error, FONT_BODY, PAL_TEXT);
    kit_space(14);
    static const char *const labels[1] = { "OK" };
    int b = buttons(labels, 1, 0, 1);
    kit_dialog_end();
    if (b == 0 || dismissed)
        app_dismiss_error(a);
}

void view_dialogs(app *a)
{
    join_dialog(a);
    everyone_dialog(a);
    confirm_dialog(a);
    resize_dialog_view(a);
    properties_dialog(a);
    about_dialog(a);
    edit_colors_dialog(a);
    ask_path_dialog(a);
    error_dialog(a);
}
