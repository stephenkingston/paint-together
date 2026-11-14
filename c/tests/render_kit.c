/*
 * Paint (C port) - golden images of the widget kit: a mock Paint window
 *
 * Builds the window the way app.rs does (title bar with the Quick Access
 * Toolbar, the tab strip, the Home / View / Together / Text ribbons, the
 * canvas with a picture, the status bar, menus, a tooltip and dialogs)
 * from the kit and ribbon controls, and writes each scene as a PNG to
 * compare with docs/screenshots. It is also a template for the app's own
 * screens. Not a test (no test_ prefix):
 *
 *   make BUILD=build/kit build/kit/tests/render_kit && build/kit/tests/render_kit out-dir
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os/os.h"
#include "paint/shapes.h"
#include "ui/font.h"
#include "ui/icons.h"
#include "ui/kit.h"
#include "ui/ribbon.h"
#include "ui/theme.h"
#include "stb_image_write.h"

enum
{
    TAB_HOME,
    TAB_VIEW,
    TAB_TOGETHER,
    TAB_TEXT
};

enum
{
    TOOL_PENCIL,
    TOOL_FILL,
    TOOL_TEXT,
    TOOL_ERASER,
    TOOL_PICKER,
    TOOL_MAGNIFIER,
    TOOL_SELECT,
    TOOL_BRUSH,
    TOOL_SHAPE
};

/* What the mock app shows */
static struct
{
    int tab, tool, shape, select_free, outline, fill;
    color primary, secondary;
    int editing_secondary;
    int show_rulers, show_grid, show_status, show_thumbnail;
    float zoom, size_pencil;
    img *picture;
    int edit_colors;
    float hsl[3];
    int join_dialog;
    char addr[64];
    char me_name[32];
    int bold, italic, underline, strike, opaque;
    int font_index;
    rect view;
} app;

static const struct
{
    uint8_t r, g, b;
    const char *name;
} palette[20] = {
    { 0, 0, 0, "Black" },           { 127, 127, 127, "Gray-50%" },   { 136, 0, 21, "Dark red" },     { 237, 28, 36, "Red" },
    { 255, 127, 39, "Orange" },     { 255, 242, 0, "Yellow" },       { 34, 177, 76, "Green" },       { 0, 162, 232, "Turquoise" },
    { 63, 72, 204, "Indigo" },      { 163, 73, 164, "Purple" },      { 255, 255, 255, "White" },     { 195, 195, 195, "Gray-25%" },
    { 185, 122, 87, "Brown" },      { 255, 174, 201, "Rose" },       { 255, 201, 14, "Gold" },       { 239, 228, 176, "Light yellow" },
    { 181, 230, 29, "Lime" },       { 153, 217, 234, "Light turquoise" }, { 112, 146, 190, "Blue-gray" }, { 200, 191, 231, "Lavender" },
};

static const char *shape_names[SHAPE_COUNT];   /* the gallery's tooltips: shape_name() */

static const char *const fonts[] = { "Arial", "Cantarell", "Comic Neue", "DejaVu Sans", "DejaVu Serif", "Liberation Mono", "Liberation Sans",
                                     "Liberation Serif", "Noto Sans", "Noto Serif", "Ubuntu" };

/* ---- the picture (the screenshots' house, tree and sun) ---- */

static img *make_picture(void)
{
    int w = 1152, h = 648;
    surface *s = surface_new(w, h);
    color sky = RGB3(153, 217, 234), grass = RGB3(34, 177, 76), brown = RGB3(185, 122, 87), purple = RGB3(163, 73, 164);
    gfx_fill(s, rect_make(0, 0, w, h), sky);
    gfx_fill(s, rect_make(4, 520, w - 4, h - 520), grass);
    gfx_circle(s, 1007, 127, 74, RGB3(255, 242, 0));
    gfx_ellipse(s, 740, 331, 111, 91, grass);
    gfx_fill(s, rect_make(719, 399, 42, 124), brown);
    float roof[6] = { 179, 333, 335, 199, 491, 333 };
    gfx_polygon(s, roof, 3, RGB3(237, 28, 36));
    gfx_fill(s, rect_make(200, 330, 270, 193), RGB3(239, 228, 176));
    gfx_frame(s, rect_make(199, 329, 272, 195), 3, brown);
    gfx_fill(s, rect_make(400, 380, 50, 50), sky);
    gfx_frame(s, rect_make(399, 379, 52, 52), 3, brown);
    gfx_fill(s, rect_make(304, 429, 62, 94), brown);
    float bird1[10] = { 551, 142, 577, 122, 603, 142, 628, 122, 653, 142 }, bird2[10] = { 667, 102, 687, 87, 707, 102, 727, 87, 747, 102 };
    gfx_polyline(s, bird1, 5, 5, purple, 0);
    gfx_polyline(s, bird2, 5, 5, purple, 0);
    float stem[16] = { 832, 640, 834, 600, 832, 572, 850, 560, 848, 543, 832, 541, 816, 552, 818, 572 };
    gfx_polyline(s, stem, 8, 5, purple, 0);
    text_draw(s, font_get(30, FONT_BOLD), 40, 28, "Drawing together!", -1, RGB3(237, 28, 36));
    rgba white = { 255, 255, 255, 255 };
    img *im = img_new(w, h, white);
    for (int i = 0; i < w * h; i++)
    {
        uint8_t *p = im->px + (size_t)i * 4;
        p[0] = (uint8_t)C_R(s->px[i]);
        p[1] = (uint8_t)C_G(s->px[i]);
        p[2] = (uint8_t)C_B(s->px[i]);
        p[3] = 255;
    }
    surface_free(s);
    return im;
}

/* ---- the title bar (Rust: title_bar) ---- */

static void title_bar(rect bar)
{
    gfx_fill(kit_dst(), bar, PAL_CHROME);
    kit_area(rect_make(bar.x + S(6), bar.y, bar.w - S(12), bar.h), 1, 0);
    kit_row_centered(0, TITLE_BAR_H);
    kit_space(6);
    rect logo = kit_alloc(20, 20);
    icon_draw(kit_dst(), ICON_LOGO, 0, logo.x, logo.y, logo.w, 1);
    kit_space(6);
    rb_icon_button(ICON_SAVE, 0, 0, 1, "Save (Ctrl+S)");
    rb_icon_button(ICON_UNDO, 0, 0, 1, "Undo (Ctrl+Z)");
    rb_icon_button(ICON_REDO, 0, 0, 0, "Redo (Ctrl+Shift+Z)");
    kit_end_layout();
    kit_area_end();
}

/* ---- the Home tab's groups (Rust: home_tab and the *_group functions) ---- */

static void clipboard_group(void)
{
    kit_resp menu;
    kit_resp paste = rb_big_split(ICON_PASTE, 0, "Paste", 0, 1, &menu);
    kit_tooltip(&paste, "Paste (Ctrl+V)");
    if (kit_popup_menu(&menu, 180, 0))
    {
        rb_menu_item(ICON_PASTE, 0, "Paste", 0, 1);
        rb_menu_item(ICON_OPEN, 0, "Paste from", 0, 1);
        kit_popup_end();
    }
    kit_col(0);
    kit_resp cut = rb_small_button(ICON_CUT, 0, "Cut", 0, 0, 0);
    kit_tooltip(&cut, "Cut the selection");
    kit_resp copy = rb_small_button(ICON_COPY, 0, "Copy", 0, 0, 1);
    kit_tooltip(&copy, "Copy the selection");
    kit_end_layout();
}

static void image_group(void)
{
    kit_resp menu;
    kit_resp sel = rb_big_split(app.select_free ? ICON_FREE_SELECT : ICON_SELECT, 0, "Select", app.tool == TOOL_SELECT, 1, &menu);
    kit_tooltip(&sel, "Select");
    if (kit_popup_menu(&menu, 230, POPUP_STAY_OPEN))
    {
        rb_menu_header("Selection shapes");
        if (rb_menu_item(ICON_SELECT, 0, "Rectangular selection", !app.select_free, 1).clicked)
            app.select_free = 0, kit_popup_close();
        if (rb_menu_item(ICON_FREE_SELECT, 0, "Free-form selection", app.select_free, 1).clicked)
            app.select_free = 1, kit_popup_close();
        rb_menu_header("Selection options");
        rb_menu_item(ICON_SELECT_ALL, 0, "Select all", 0, 1);
        rb_menu_item(ICON_DELETE, 0, "Delete", 0, 0);
        rb_menu_item(-1, 0, "Transparent selection", 1, 1);
        kit_popup_end();
    }
    kit_col(0);
    kit_resp crop = rb_small_button(ICON_CROP, 0, "Crop", 0, 0, 0);
    kit_tooltip(&crop, "Crop to the selection");
    kit_resp resize = rb_small_button(ICON_RESIZE, 0, "Resize", 0, 0, 1);
    kit_tooltip(&resize, "Resize and skew");
    kit_resp rotate = rb_small_button(ICON_ROTATE, 0, "Rotate", 1, 0, 1);
    if (kit_popup_menu(&rotate, 200, 0))
    {
        rb_menu_item(ICON_ROTATE, 0, "Rotate right 90\xC2\xB0", 0, 1);
        rb_menu_item(ICON_ROTATE_LEFT, 0, "Rotate left 90\xC2\xB0", 0, 1);
        rb_menu_item(ICON_ROTATE_180, 0, "Rotate 180\xC2\xB0", 0, 1);
        rb_menu_item(ICON_FLIP_VERTICAL, 0, "Flip vertical", 0, 1);
        rb_menu_item(ICON_FLIP_HORIZONTAL, 0, "Flip horizontal", 0, 1);
        kit_popup_end();
    }
    kit_end_layout();
}

static void tools_group(void)
{
    static const struct
    {
        int tool, icon;
        const char *tip;
    } tools[6] = { { TOOL_PENCIL, ICON_PENCIL, "Pencil" },       { TOOL_FILL, ICON_FILL, "Fill with color" },
                   { TOOL_TEXT, ICON_TEXT, "Text" },             { TOOL_ERASER, ICON_ERASER, "Eraser" },
                   { TOOL_PICKER, ICON_PICKER, "Color picker" }, { TOOL_MAGNIFIER, ICON_MAGNIFIER, "Magnifier" } };
    kit_col(2);
    for (int row = 0; row < 2; row++)
    {
        kit_row(0);
        for (int i = row * 3; i < row * 3 + 3; i++)
            if (rb_icon_button(tools[i].icon, 0, app.tool == tools[i].tool, 1, tools[i].tip).clicked)
                app.tool = tools[i].tool;
        kit_end_layout();
    }
    kit_end_layout();
    kit_space(4);
}

static void brushes_group(void)
{
    kit_resp menu;
    kit_resp main = rb_big_split(ICON_BRUSHES, 0, "Brushes", app.tool == TOOL_BRUSH, 1, &menu);
    kit_tooltip(&main, "Brush");
    if (kit_popup_menu(&menu, 3 * 52 + 8, 0))
    {
        /* The app draws its brush previews into each cell (Rust: brush_preview) */
        kit_col(2);
        for (int row = 0; row < 3; row++)
        {
            kit_row(2);
            for (int c = 0; c < 3; c++)
            {
                rect cell = kit_alloc(50, 50);
                kit_resp r = kit_interact(cell, kit_id_n("brush", row * 3 + c), SENSE_CLICK);
                if (row == 0 && c == 0)
                    rb_frame(kit_dst(), cell, 0, 0, 1);
                else if (r.hovered)
                    rb_frame(kit_dst(), cell, 1, 0, 0);
                float pts[2 * 21];
                for (int i = 0; i <= 20; i++)
                {
                    float t = i / 20.0f;
                    pts[2 * i] = cell.x + SF(9) + t * SF(32);
                    pts[2 * i + 1] = cell.y + SF(25) - sinf(t * 6.2831853f) * SF(9) + (t - 0.5f) * SF(8);
                }
                gfx_polyline(kit_dst(), pts, 21, SF(1 + (row * 3 + c) % 4), COLOR_A(0x1E1E1E, 255 - 40 * c), 0);
            }
            kit_end_layout();
        }
        kit_end_layout();
        kit_popup_end();
    }
}

static void shapes_group(void)
{
    kit_resp more;
    int picked = rb_gallery(ICON_SHAPE, SHAPE_COUNT, 7, 3, 21, app.tool == TOOL_SHAPE ? app.shape : -1, shape_names, &more);
    if (picked >= 0)
        app.tool = TOOL_SHAPE, app.shape = picked;
    if (kit_popup_menu(&more, 240, 0))
    {
        for (int i = 0; i < SHAPE_COUNT; i++)
            if (rb_menu_item(ICON_SHAPE, (uint32_t)i, shape_names[i], app.tool == TOOL_SHAPE && app.shape == i, 1).clicked)
                app.tool = TOOL_SHAPE, app.shape = i;
        kit_popup_end();
    }
    kit_space(4);
    kit_col(0);
    kit_resp outline = rb_small_button(ICON_OUTLINE, app.outline ? app.primary : 0, "Outline", 1, 0, 1);
    kit_tooltip(&outline, app.outline ? "Shape outline: solid Color 1" : "Shape outline: none");
    if (kit_popup_menu(&outline, 170, 0))
    {
        rb_menu_item(ICON_OUTLINE, 0, "No outline", !app.outline, 1);
        rb_menu_item(ICON_OUTLINE, app.primary, "Solid color", app.outline, 1);
        kit_popup_end();
    }
    kit_resp fill = rb_small_button(ICON_FILL_SHAPE, app.fill ? app.secondary : 0, "Fill", 1, 0, 1);
    kit_tooltip(&fill, app.fill ? "Shape fill: solid Color 2" : "Shape fill: none");
    if (kit_popup_menu(&fill, 170, 0))
    {
        rb_menu_item(ICON_FILL_SHAPE, 0, "No fill", !app.fill, 1);
        rb_menu_item(ICON_FILL_SHAPE, app.secondary, "Solid color", app.fill, 1);
        kit_popup_end();
    }
    kit_end_layout();
}

static void size_group(void)
{
    kit_resp size = rb_big_button(ICON_SIZE, 0, "Size", 1, 0, 1);
    if (kit_popup_menu(&size, 200, POPUP_STAY_OPEN))
    {
        static const float presets[4] = { 1, 2, 3, 4 };
        for (int i = 0; i < 4; i++)
            if (rb_size_row(presets[i], fabsf(app.size_pencil - presets[i]) < 0.5f).clicked)
                app.size_pencil = presets[i], kit_popup_close();
        kit_separator();
        rb_value_slider(&app.size_pencil, 1, 20, 1, " px");
        kit_popup_end();
    }
}

static void colors_group(void)
{
    for (int slot = 0; slot < 2; slot++)
    {
        rect pic;
        kit_resp r = rb_big_button_custom(slot ? "Color\n2" : "Color\n1", 0, app.editing_secondary == slot, 1, &pic);
        int side = S(slot ? 22 : 30);
        rb_paint_swatch(kit_dst(), rect_make(pic.x + (pic.w - side) / 2, pic.y + (pic.h - side) / 2, side, side), slot ? app.secondary : app.primary, 0);
        kit_tooltip(&r, slot ? "Color 2 (background color)\nClick here and then select a color from the palette. This color is used with the eraser and for shape fills."
                             : "Color 1 (foreground color)\nClick here and then select a color from the palette. This color is used with the pencil and with brushes, as well as for shape outlines.");
        if (r.clicked)
            app.editing_secondary = slot;
    }
    kit_space(3);
    kit_col(0);
    for (int row = 0; row < 3; row++)
    {
        kit_row(0);
        for (int col = 0; col < 10; col++)
        {
            if (row == 2)
            {
                rb_empty_swatch(22);
                continue;
            }
            int i = row * 10 + col;
            kit_resp s = rb_swatch(RGB3(palette[i].r, palette[i].g, palette[i].b), 22);
            kit_tooltip(&s, palette[i].name);
            if (s.clicked)
                *(app.editing_secondary ? &app.secondary : &app.primary) = RGB3(palette[i].r, palette[i].g, palette[i].b);
        }
        kit_end_layout();
    }
    kit_end_layout();
    kit_space(3);
    kit_resp edit = rb_big_button(ICON_EDIT_COLORS, 0, "Edit\ncolors", 0, 0, 1);
    kit_tooltip(&edit, "Edit colors\nSelect a color from the color palette.");
    if (edit.clicked)
        app.edit_colors = 1;
}

static void home_tab(void)
{
    rb_group_begin("Clipboard", CONTENT_H);
    clipboard_group();
    rb_group_end();
    rb_group_begin("Image", CONTENT_H);
    image_group();
    rb_group_end();
    rb_group_begin("Tools", 46);
    tools_group();
    rb_group_end();
    rb_group_begin(NULL, CONTENT_H);
    brushes_group();
    rb_group_end();
    rb_group_begin("Shapes", CONTENT_H);
    shapes_group();
    rb_group_end();
    rb_group_begin(NULL, CONTENT_H);
    size_group();
    rb_group_end();
    rb_group_begin("Colors", CONTENT_H);
    colors_group();
    rb_group_end();
}

static void view_tab(void)
{
    rb_group_begin("Zoom", CONTENT_H);
    rb_big_button(ICON_ZOOM_IN, 0, "Zoom\nin", 0, 0, 1);
    rb_big_button(ICON_ZOOM_OUT, 0, "Zoom\nout", 0, 0, 1);
    rb_big_button(ICON_ZOOM_100, 0, "100\n%", 0, 0, 1);
    rb_group_end();
    rb_group_begin("Show or hide", CONTENT_H);
    kit_col(0);
    kit_checkbox("Rulers", &app.show_rulers);
    kit_checkbox("Gridlines", &app.show_grid);
    kit_checkbox("Status bar", &app.show_status);
    kit_end_layout();
    rb_group_end();
    rb_group_begin("Display", CONTENT_H);
    kit_resp fs = rb_big_button(ICON_FULL_SCREEN, 0, "Full\nscreen", 0, 0, 1);
    kit_tooltip(&fs, "Full screen (F11)");
    kit_col(0);
    kit_space(4);
    kit_checkbox("Thumbnail", &app.show_thumbnail);
    kit_end_layout();
    rb_group_end();
}

static void together_tab(void)
{
    static const struct
    {
        const char *name;
        color c;
    } kids[3] = { { "Happy Otter", COLOR(0xF39C12) }, { "Brave Tiger", COLOR(0x8E44AD) }, { "Sunny Koala", COLOR(0x27AE60) } };
    rb_group_begin("Draw together", CONTENT_H);
    rb_big_button(ICON_LEAVE_ROOM, 0, "Leave", 0, 0, 1);
    rb_group_end();
    rb_group_begin("Friends", CONTENT_H);
    kit_col(0);
    kit_space(2);
    for (int i = 0; i < 3; i++)
    {
        char label[64];
        snprintf(label, sizeof label, i == 2 ? "%s (you)" : "%s", kids[i].name);
        rb_kid_row(label, kids[i].name, kids[i].c, i == 2 ? NULL : "Kick out");
    }
    kit_end_layout();
    rb_group_end();
    rb_group_begin("Me", CONTENT_H);
    kit_col(3);
    kit_space(4);
    kit_row_centered(6, INTERACT_H);
    rb_avatar(app.me_name, kids[2].c, 20);
    kit_text_field("me", app.me_name, sizeof app.me_name, 20, 130, NULL, FIELD_DISABLED);
    kit_end_layout();
    kit_row(2);
    static const uint32_t dots[12] = { 0xE74C3C, 0xF39C12, 0xF1C40F, 0x27AE60, 0x16A085, 0x3498DB, 0x8E44AD, 0xE91E63, 0x00ACC1, 0x8D6E63, 0x7CB342, 0xFF7043 };
    for (int i = 0; i < 12; i++)
    {
        rect d = kit_alloc(13, 13);
        gfx_circle(kit_dst(), d.x + d.w / 2.0f, d.y + d.h / 2.0f, SF(6), COLOR(dots[i]));
        if (i == 3)
            gfx_ring(kit_dst(), d.x + d.w / 2.0f, d.y + d.h / 2.0f, SF(6), SF(1.5f), PAL_TEXT);
    }
    kit_end_layout();
    kit_end_layout();
    kit_space(6);
    rb_group_end();
}

static void text_tab(void)
{
    rb_group_begin("Clipboard", CONTENT_H);
    clipboard_group();
    rb_group_end();
    rb_group_begin("Font", 52);
    kit_col(4);
    kit_row(2);
    if (kit_combo("font", fonts[app.font_index], 150, 420))
    {
        for (int i = 0; i < (int)(sizeof fonts / sizeof *fonts); i++)
            if (kit_combo_item(fonts[i], i == app.font_index))
                app.font_index = i;
        kit_combo_end();
    }
    if (kit_combo("font-size", "26", 48, 300))
    {
        static const char *const sizes[] = { "8", "9", "10", "11", "12", "14", "16", "18", "20", "22", "24", "26", "28", "36", "48", "72" };
        for (int i = 0; i < 16; i++)
            kit_combo_item(sizes[i], i == 11);
        kit_combo_end();
    }
    kit_end_layout();
    kit_row(0);
    int *flags[4] = { &app.bold, &app.italic, &app.underline, &app.strike };
    static const int icons[4] = { ICON_BOLD, ICON_ITALIC, ICON_UNDERLINE, ICON_STRIKE };
    static const char *const tips[4] = { "Bold", "Italic", "Underline", "Strikethrough" };
    for (int i = 0; i < 4; i++)
        if (rb_icon_button(icons[i], 0, *flags[i], 1, tips[i]).clicked)
            *flags[i] = !*flags[i];
    kit_end_layout();
    kit_end_layout();
    rb_group_end();
    rb_group_begin("Background", 46);
    kit_col(0);
    if (rb_small_button(ICON_OPAQUE, 0, "Opaque", 0, app.opaque, 1).clicked)
        app.opaque = 1;
    if (rb_small_button(ICON_TRANSPARENT, 0, "Transparent", 0, !app.opaque, 1).clicked)
        app.opaque = 0;
    kit_end_layout();
    rb_group_end();
    rb_group_begin("Colors", CONTENT_H);
    colors_group();
    rb_group_end();
}

/* ---- the tab strip and ribbon body (Rust: ribbon) ---- */

static void ribbon(rect full)
{
    surface *d = kit_dst();
    rect strip = rect_make(full.x, full.y, full.w, S(TAB_STRIP_H));
    gfx_fill(d, strip, PAL_CHROME);
    rect selected = { 0 };
    int have_selected = 0;
    kit_area(strip, 1, 0);
    kit_set_gap(TAB_FLARE);
    kit_space(4);
    kit_resp file = rb_file_tab();
    (void)file;
    static const char *const labels[4] = { "Home", "View", "Together", "Text" };
    for (int t = 0; t < (app.tool == TOOL_TEXT ? 4 : 3); t++)
    {
        kit_resp r = rb_tab(labels[t], app.tab == t, t == TAB_TEXT);
        if (r.clicked)
            app.tab = t;
        if (app.tab == t)
            selected = r.r, have_selected = 1;
    }
    kit_area_end();
    kit_area(strip, 1, 0);
    kit_row_rtl(0, TAB_STRIP_H);
    kit_space(6);
    rb_icon_button(ICON_HELP, 0, 0, 1, "About Paint");
    rb_icon_button(ICON_COLLAPSE, 0, 0, 1, "Minimize the Ribbon");
    kit_end_layout();
    kit_area_end();

    rect body = rect_make(full.x, strip.y + strip.h, full.w, S(CONTENT_H + 26));
    gfx_fill(d, body, PAL_RIBBON);
    rb_tab_line(full.x, full.x + full.w, strip.y + strip.h - HAIR(), have_selected ? &selected : NULL);
    if (have_selected)
        rb_selected_tab_outline(selected);
    hair_h(d, full.x, full.x + full.w - 1, body.y + body.h - HAIR(), PAL_RIBBON_BORDER);
    kit_area(rect_make(body.x + S(4), body.y + S(4), body.w - S(8), body.h - S(6)), 1, 0);
    switch (app.tab)
    {
    case TAB_HOME:
        home_tab();
        break;
    case TAB_VIEW:
        view_tab();
        break;
    case TAB_TOGETHER:
        together_tab();
        break;
    case TAB_TEXT:
        text_tab();
        break;
    }
    kit_area_end();
}

/* ---- the status bar (Rust: status_bar) ---- */

static void status_bar(rect bar)
{
    surface *d = kit_dst();
    gfx_fill(d, bar, PAL_STATUS);
    hair_h(d, bar.x, bar.x + bar.w - 1, bar.y, PAL_RIBBON_BORDER);
    kit_area(rect_make(bar.x + S(4), bar.y, bar.w - S(8), bar.h), 1, 0);
    kit_row_centered(0, STATUS_H);
    kit_space(4);
    rb_status_item(ICON_CURSOR_POS, app.tab == TAB_TOGETHER ? "992, 256px" : "", 130);
    rb_status_item(ICON_SELECTION_SIZE, "", 130);
    char size[64];
    snprintf(size, sizeof size, "%d \xC3\x97 %dpx", app.picture->w, app.picture->h);
    rb_status_item(ICON_CANVAS_SIZE, size, 150);
    kit_space(140);
    kit_end_layout();
    kit_row_rtl(2, STATUS_H);
    kit_space(8);
    if (rb_status_button(0).clicked)
        app.zoom *= 2;
    float lo = logf(0.125f), hi = logf(8), t = (logf(app.zoom) - lo) / (hi - lo);
    if (rb_zoom_slider(&t, -lo / (hi - lo), 110))
        app.zoom = expf(lo + t * (hi - lo));
    rb_status_button(1);
    char pct[16];
    snprintf(pct, sizeof pct, "%.0f%%", app.zoom * 100);
    rect z = kit_alloc(44, 20);
    text_in(d, ui_font(FONT_BODY, 0), z, ALIGN_CENTER, pct, PAL_TEXT);
    kit_space(6);
    font *f = ui_font(12, 0);
    const char *together = "Drawing with 2 friends";
    rb_status_item(ICON_TOGETHER, together, (int)((text_width(f, together, -1) + S(36)) * 100 / ui_scale));
    kit_end_layout();
    kit_area_end();
}

/* ---- the canvas (Rust: canvas, paint_canvas) ---- */

static void canvas(rect area)
{
    surface *d = kit_dst();
    gfx_fill(d, area, PAL_WORKSPACE);
    app.view = area;
    rect old = gfx_clip(d, area);
    int m = S(8);
    rect crect = rect_make(area.x + m, area.y + m, (int)(app.picture->w * app.zoom), (int)(app.picture->h * app.zoom));
    kit_resp r = kit_interact(rect_cut(area, crect), kit_id_of("canvas"), SENSE_DRAG);
    (void)r;
    gfx_shadow(d, crect, 0, S(4), S(22), PAL_CANVAS_SHADOW);
    rect vis = rect_cut(crect, area);
    gfx_canvas(d, vis, app.picture, (vis.x - crect.x) / app.zoom, (vis.y - crect.y) / app.zoom, app.zoom);
    /* The resize handles: right, bottom and the corner */
    int hs = S(7);
    rect handles[3] = { rect_make(crect.x + crect.w - hs / 2 + S(4), crect.y + crect.h / 2 - hs / 2, hs, hs),
                        rect_make(crect.x + crect.w / 2 - hs / 2, crect.y + crect.h - hs / 2 + S(4), hs, hs),
                        rect_make(crect.x + crect.w - hs / 2 + S(4), crect.y + crect.h - hs / 2 + S(4), hs, hs) };
    for (int i = 0; i < 3; i++)
    {
        gfx_round(d, handles[i], S(2), COLOR(0xFFFFFF));
        gfx_round_frame(d, handles[i], S(2), 1, COLOR(0x6E6E6E));
    }
    d->clip = old;
}

/* ---- dialogs (Rust: dialogs, dialog, spectrum, luminance_bar, color_fields) ---- */

static color hsl_to_rgb(const float *hsl)
{
    float h = hsl[0], s = hsl[1], l = hsl[2];
    float c = (1 - fabsf(2 * l - 1)) * s, hp = fmodf(fmodf(h, 1) + 1, 1) * 6, x = c * (1 - fabsf(fmodf(hp, 2) - 1));
    float r = 0, g = 0, b = 0;
    switch ((int)hp)
    {
    case 0:
        r = c, g = x;
        break;
    case 1:
        r = x, g = c;
        break;
    case 2:
        g = c, b = x;
        break;
    case 3:
        g = x, b = c;
        break;
    case 4:
        r = x, b = c;
        break;
    default:
        r = c, b = x;
    }
    float m = l - c / 2;
#define TO(v) (int)fminf(255, fmaxf(0, roundf(((v) + m) * 255)))
    return RGB3(TO(r), TO(g), TO(b));
#undef TO
}

static void spectrum(float *hsl)
{
    rect r = kit_alloc(180, 170);
    kit_resp res = kit_interact(r, kit_id_of("spectrum"), SENSE_DRAG);
    if (res.held)
    {
        hsl[0] = fminf(1, fmaxf(0, (res.x - r.x) / (float)r.w));
        hsl[1] = fminf(1, fmaxf(0, 1 - (res.y - r.y) / (float)r.h));
    }
    surface *d = kit_dst();
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++)
        {
            float c[3] = { (x + 0.5f) / r.w, 1 - (y + 0.5f) / r.h, 0.5f };
            gfx_fill(d, rect_make(r.x + x, r.y + y, 1, 1), hsl_to_rgb(c));
        }
    gfx_frame(d, rect_inset(r, -1), HAIR(), COLOR(0x787878));
    /* Windows' crosshair */
    float cx = r.x + hsl[0] * r.w, cy = r.y + (1 - hsl[1]) * r.h, w = SF(2);
    gfx_stroke(d, (float[]){ cx + SF(3), cy, cx + SF(9), cy }, 2, w, COLOR(0), GFX_BUTT);
    gfx_stroke(d, (float[]){ cx - SF(3), cy, cx - SF(9), cy }, 2, w, COLOR(0), GFX_BUTT);
    gfx_stroke(d, (float[]){ cx, cy + SF(3), cx, cy + SF(9) }, 2, w, COLOR(0), GFX_BUTT);
    gfx_stroke(d, (float[]){ cx, cy - SF(3), cx, cy - SF(9) }, 2, w, COLOR(0), GFX_BUTT);
}

static void luminance_bar(float *hsl)
{
    rect r = kit_alloc(26, 170);
    rect bar = rect_make(r.x, r.y, S(14), r.h);
    kit_resp res = kit_interact(r, kit_id_of("lum"), SENSE_DRAG);
    if (res.held)
        hsl[2] = fminf(1, fmaxf(0, 1 - (res.y - r.y) / (float)r.h));
    for (int y = 0; y < bar.h; y++)
    {
        float c[3] = { hsl[0], hsl[1], 1 - (y + 0.5f) / bar.h };
        gfx_fill(kit_dst(), rect_make(bar.x, bar.y + y, bar.w, 1), hsl_to_rgb(c));
    }
    gfx_frame(kit_dst(), rect_inset(bar, -1), HAIR(), COLOR(0x787878));
    float y = bar.y + (1 - hsl[2]) * bar.h, tx = bar.x + bar.w + SF(2);
    gfx_polygon(kit_dst(), (float[]){ tx, y, tx + SF(7), y - SF(5), tx + SF(7), y + SF(5) }, 3, COLOR(0));
}

static void edit_colors_dialog(void)
{
    static const uint32_t basic[48] = { 0xFF8080, 0xFFFF80, 0x80FF80, 0x00FF80, 0x80FFFF, 0x0080FF, 0xFF80C0, 0xFF80FF, 0xFF0000, 0xFFFF00, 0x80FF00, 0x00FF40,
                                        0x00FFFF, 0x0080C0, 0x8080C0, 0xFF00FF, 0x804040, 0xFF8040, 0x00FF00, 0x008080, 0x004080, 0x8080FF, 0x800040, 0xFF0080,
                                        0x800000, 0xFF8000, 0x008000, 0x008040, 0x0000FF, 0x0000A0, 0x800080, 0x8000FF, 0x400000, 0x804000, 0x004000, 0x004040,
                                        0x000080, 0x000040, 0x400040, 0x400080, 0x000000, 0x808000, 0x808040, 0x808080, 0x408080, 0xC0C0C0, 0x400040, 0xFFFFFF };
    int dismissed = 0;
    if (!kit_dialog_begin("edit-colors", "Edit Colors", 520, &dismissed))
        return;
    kit_row(6);
    kit_col(5);
    kit_label("Basic colors:");
    for (int row = 0; row < 6; row++)
    {
        kit_row(2);
        for (int c = 0; c < 8; c++)
            if (rb_swatch(COLOR(basic[row * 8 + c]), 22).clicked)
                app.hsl[0] = app.hsl[0];
        kit_end_layout();
    }
    kit_space(8);
    kit_label("Custom colors:");
    for (int row = 0; row < 2; row++)
    {
        kit_row(2);
        for (int c = 0; c < 5; c++)
            rb_empty_swatch(22);
        kit_end_layout();
    }
    kit_end_layout();
    kit_space(10);
    kit_col(5);
    kit_row(6);
    spectrum(app.hsl);
    kit_space(4);
    luminance_bar(app.hsl);
    kit_end_layout();
    kit_space(8);
    kit_row(6);
    kit_col(5);
    rect sw = kit_alloc(66, 42);
    rb_paint_swatch(kit_dst(), sw, hsl_to_rgb(app.hsl), 0);
    kit_label("Color|Solid");
    kit_end_layout();
    kit_space(8);
    /* Hue/Sat/Lum on Windows' 0-240 scale, and Red/Green/Blue */
    kit_col(4);
    static const char *const labels[3][2] = { { "Hue:", "Red:" }, { "Sat:", "Green:" }, { "Lum:", "Blue:" } };
    color rgb = hsl_to_rgb(app.hsl);
    int col_w[2] = { S(INTERACT_W), S(INTERACT_W) };     /* egui's Grid: columns at least 36 wide */
    for (int i = 0; i < 6; i++)
    {
        int w = text_width(ui_font(FONT_BODY, 0), labels[i / 2][i % 2], -1);
        if (w > col_w[i % 2])
            col_w[i % 2] = w;
    }
    int channels[3] = { C_R(rgb), C_G(rgb), C_B(rgb) };
    for (int i = 0; i < 3; i++)
    {
        kit_push_n(i);
        kit_row_centered(6, INTERACT_H);
        /* A grid: each column as wide as its widest cell */
        kit_label_w(labels[i][0], col_w[0]);
        float v = roundf(app.hsl[i] * (i ? 240 : 239));
        if (kit_number("hsl", &v, 0, i ? 240 : 239, 36))
            app.hsl[i] = v / (i ? 240 : 239);
        kit_label_w(labels[i][1], col_w[1]);
        float c = (float)channels[i];
        kit_number("rgb", &c, 0, 255, 36);
        kit_end_layout();
        kit_pop_id();
    }
    kit_end_layout();
    kit_end_layout();
    kit_space(6);
    kit_dialog_button("Add to Custom Colors", 0, 1, 220);
    kit_end_layout();
    kit_end_layout();
    kit_space(12);
    kit_row_rtl(6, 24);
    if (kit_dialog_button("Cancel", 0, 1, 0).clicked || dismissed)
        app.edit_colors = 0;
    if (kit_dialog_button("OK", 1, 1, 0).clicked)
        app.edit_colors = 0;
    kit_end_layout();
    kit_dialog_end();
}

static void join_dialog(void)
{
    int dismissed = 0;
    if (!kit_dialog_begin("join", "Join a drawing", 360, &dismissed))
        return;
    kit_label("Drawings nearby:");
    kit_space(2);
    rb_nearby_row("Mighty Hedgehog", "Mighty Hedgehog's drawing  \xC2\xB7  3 kids", 1);
    rb_nearby_row("Happy Otter", "Happy Otter's drawing  \xC2\xB7  3 kids", 1);
    kit_space(12);
    kit_label_wrap("Can't see it? Type the address shown on your friend's Together tab:", FONT_BODY, PAL_TEXT_WEAK);
    kit_space(4);
    kit_text_field("addr", app.addr, sizeof app.addr, 0, 0, "192.168.1.20", 0);
    kit_space(14);
    kit_row_rtl(6, 24);
    if (kit_dialog_button("Cancel", 0, 1, 0).clicked || dismissed)
        app.join_dialog = 0;
    kit_dialog_button("Join", 1, app.addr[0] != 0, 0);
    kit_end_layout();
    kit_dialog_end();
}

/* ---- the frame (Rust: PaintApp::ui) ---- */

static void build(void)
{
    rect scr = kit_screen();
    title_bar(rect_make(0, 0, scr.w, S(TITLE_BAR_H)));
    rect rib = rect_make(0, S(TITLE_BAR_H), scr.w, S(TAB_STRIP_H + CONTENT_H + 26));
    ribbon(rib);
    rect status = rect_make(0, scr.h - S(STATUS_H), scr.w, S(STATUS_H));
    canvas(rect_make(0, rib.y + rib.h, scr.w, status.y - rib.y - rib.h));
    status_bar(status);
    if (app.join_dialog)
        join_dialog();
    if (app.edit_colors)
        edit_colors_dialog();
    kit_draw_toasts(app.view.x + app.view.w / 2, app.view.y + app.view.h - S(18));
}

/* ---- driving it ---- */

static surface *win;
static double clock_now = 100;

static void frame(void)
{
    for (int pass = 0; pass < 4; pass++)
    {
        kit_begin(win, clock_now);
        build();
        if (!kit_end())
            break;
    }
}

static void move_to(int x, int y)
{
    ui_event e = { 0 };
    e.type = EV_MOVE;
    e.x = x;
    e.y = y;
    kit_event(&e);
    frame();
}

static void click_at(int x, int y)
{
    ui_event e = { 0 };
    e.x = x;
    e.y = y;
    e.type = EV_MOVE;
    kit_event(&e);
    e.type = EV_DOWN;
    e.clicks = 1;
    kit_event(&e);
    frame();
    e.type = EV_UP;
    kit_event(&e);
    frame();
}

/* Rest the pointer so tooltips show */
static void rest(double seconds)
{
    clock_now += seconds;
    frame();
}

static void put_png(void *ctx, void *data, int size)
{
    FILE *f = ctx;
    fwrite(data, 1, (size_t)size, f);
}

static void save(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    uint8_t *rgba = malloc((size_t)win->w * (size_t)win->h * 4);
    for (int i = 0; i < win->w * win->h; i++)
    {
        uint32_t p = win->px[i];
        rgba[4 * i] = (uint8_t)(p >> 16);
        rgba[4 * i + 1] = (uint8_t)(p >> 8);
        rgba[4 * i + 2] = (uint8_t)p;
        rgba[4 * i + 3] = 255;
    }
    FILE *f = fopen(path, "wb");
    if (f)
    {
        stbi_write_png_to_func(put_png, f, win->w, win->h, 4, rgba, win->w * 4);
        fclose(f);
        printf("wrote %s\n", path);
    }
    free(rgba);
}

static void reset(int scale)
{
    theme_set_scale(scale);
    surface_free(win);
    win = surface_new(1460 * scale / 100, 900 * scale / 100);
    kit_init();
    app.tab = TAB_HOME;
    app.tool = TOOL_PENCIL;
    app.shape = -1;
    app.outline = 1;
    app.fill = 0;
    app.primary = RGB3(237, 28, 36);
    app.secondary = COLOR(0xFFFFFF);
    app.editing_secondary = 0;
    app.show_rulers = 0;
    app.show_status = 1;
    app.zoom = 1;
    app.size_pencil = 1;
    app.edit_colors = 0;
    app.join_dialog = 0;
    app.hsl[0] = 128 / 239.0f;
    app.hsl[1] = 158 / 240.0f;
    app.hsl[2] = 182 / 240.0f;
    app.addr[0] = 0;
    snprintf(app.me_name, sizeof app.me_name, "Sunny Koala");
    app.font_index = 6;
    app.opaque = 0;
    app.bold = 1;
    move_to(-10, -10);
    ui_event leave = { 0 };
    leave.type = EV_LEAVE;
    kit_event(&leave);
    frame();
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    os_init();
    if (font_init() != 0)
    {
        fprintf(stderr, "no UI font found\n");
        return 1;
    }
    app.picture = make_picture();
    for (int i = 0; i < SHAPE_COUNT; i++)
        shape_names[i] = shape_name((shape_kind)i);
    double t0 = os_now();

    reset(100);
    save(dir, "kit-main.png");
    printf("first frames, caches filling: %.1f ms\n", (os_now() - t0) * 1000);
    t0 = os_now();
    for (int i = 0; i < 20; i++)
    {
        kit_repaint();
        frame();
    }
    printf("a frame: %.2f ms\n", (os_now() - t0) * 1000 / 20);

    /* The pencil hovered, with its tooltip */
    move_to(274, 79);
    rest(1);
    save(dir, "kit-tooltip.png");

    /* The Select menu open */
    reset(100);
    click_at(152, 110);
    move_to(200, 240);
    save(dir, "kit-select-menu.png");

    /* The shapes' "more" menu and the Size menu */
    reset(100);
    click_at(578, 112);
    save(dir, "kit-shapes-menu.png");
    reset(100);
    click_at(699, 100);
    save(dir, "kit-size-menu.png");
    reset(100);
    click_at(380, 112);
    save(dir, "kit-brushes.png");

    /* Edit Colors */
    reset(100);
    app.edit_colors = 1;
    frame();
    save(dir, "kit-edit-colors.png");

    /* The other tabs */
    reset(100);
    app.tab = TAB_VIEW;
    app.show_rulers = 1;
    frame();
    save(dir, "kit-view.png");
    reset(100);
    app.tab = TAB_TOGETHER;
    frame();
    move_to(160, 95);
    save(dir, "kit-together.png");
    reset(100);
    app.tab = TAB_TEXT;
    app.tool = TOOL_TEXT;
    frame();
    click_at(200, 75);
    move_to(200, 200);
    save(dir, "kit-text-font-list.png");

    /* The Join dialog, typing an address */
    reset(100);
    app.tab = TAB_TOGETHER;
    app.join_dialog = 1;
    frame();
    frame();
    click_at(730, 514);
    ui_event e = { 0 };
    e.type = EV_TEXT;
    e.text = "192.168.1.7";
    kit_event(&e);
    frame();
    save(dir, "kit-join.png");

    /* 150% */
    reset(150);
    save(dir, "kit-main-150.png");
    app.edit_colors = 1;
    frame();
    save(dir, "kit-edit-colors-150.png");

    /* 200% (a Retina Mac) */
    reset(200);
    click_at(152 * 2, 110 * 2);
    move_to(200 * 2, 240 * 2);
    save(dir, "kit-select-menu-200.png");
    return 0;
}
