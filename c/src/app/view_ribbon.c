/*
 * Paint (C port) - the tab strip and the ribbon's tabs (Rust: app.rs's ribbon, home_tab and its
 * groups, view_tab, text_tab and together_tab)
 *
 * Each group is laid out as the Rust lays it out in egui; the controls are
 * ribbon.h's. Clicking calls the app's actions; the few plain settings
 * (show rulers, the tab shown...) are the app's fields, set directly as the
 * Rust UI set them.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/view.h"
#include "collab/names.h"
#include "paint/shapes.h"
#include "paint/text.h"

static const char *const COLOR1_TIP = "Color 1 (foreground color)\nClick here and then select a color from the palette. "
                                      "This color is used with the pencil and with brushes, as well as for shape outlines.";
static const char *const COLOR2_TIP = "Color 2 (background color)\nClick here and then select a color from the palette. "
                                      "This color is used with the eraser and for shape fills.";

/* ---- Home ---- */

void view_clipboard_group(app *a)
{
    char tip[64];
    kit_resp menu;
    kit_resp paste = rb_big_split(ICON_PASTE, 0, "Paste", 0, 1, &menu);
    kit_tooltip(&paste, view_tip(tip, sizeof tip, "Paste", KMOD_CTRL, 'V'));
    if (paste.clicked)
        app_paste(a);
    if (kit_popup_menu(&menu, 180, 0))
    {
        if (rb_menu_item(ICON_PASTE, 0, "Paste", 0, 1).clicked)
            app_paste(a), kit_popup_close();
        if (rb_menu_item(ICON_OPEN, 0, "Paste from", 0, 1).clicked)
            kit_popup_close(), app_paste_from(a);
        kit_popup_end();
    }
    kit_col(0);
    kit_resp cut = rb_small_button(ICON_CUT, 0, "Cut", 0, 0, a->sel != NULL);
    kit_tooltip(&cut, "Cut the selection");
    if (cut.clicked)
        app_cut(a);
    kit_resp copy = rb_small_button(ICON_COPY, 0, "Copy", 0, 0, 1);
    kit_tooltip(&copy, "Copy the selection");
    if (copy.clicked)
        app_copy(a);
    kit_end_layout();
}

void view_rotate_items(app *a)
{
    static const struct
    {
        int icon, how;
        const char *label;
    } items[5] = { { ICON_ROTATE, ROTATE_RIGHT, "Rotate right 90\xC2\xB0" },  { ICON_ROTATE_LEFT, ROTATE_LEFT, "Rotate left 90\xC2\xB0" },
                   { ICON_ROTATE_180, ROTATE_180, "Rotate 180\xC2\xB0" },       { ICON_FLIP_VERTICAL, FLIP_VERTICAL, "Flip vertical" },
                   { ICON_FLIP_HORIZONTAL, FLIP_HORIZONTAL, "Flip horizontal" } };
    for (int i = 0; i < 5; i++)
        if (rb_menu_item(items[i].icon, 0, items[i].label, 0, 1).clicked)
        {
            app_rotate(a, items[i].how);
            kit_popup_close();
        }
}

static void image_group(app *a)
{
    int selecting = a->tool == TOOL_SELECT || a->tool == TOOL_FREE_SELECT, has_sel = a->sel != NULL;
    kit_resp menu;
    kit_resp sel = rb_big_split(a->select_mode == TOOL_FREE_SELECT ? ICON_FREE_SELECT : ICON_SELECT, 0, "Select", selecting, 1, &menu);
    kit_tooltip(&sel, "Select");
    if (sel.clicked)
        app_set_tool(a, a->select_mode);
    if (kit_popup_menu(&menu, 230, POPUP_STAY_OPEN))
    {
        rb_menu_header("Selection shapes");
        if (rb_menu_item(ICON_SELECT, 0, "Rectangular selection", a->select_mode == TOOL_SELECT, 1).clicked)
            app_set_select_mode(a, TOOL_SELECT), kit_popup_close();
        if (rb_menu_item(ICON_FREE_SELECT, 0, "Free-form selection", a->select_mode == TOOL_FREE_SELECT, 1).clicked)
            app_set_select_mode(a, TOOL_FREE_SELECT), kit_popup_close();
        rb_menu_header("Selection options");
        if (rb_menu_item(ICON_SELECT_ALL, 0, "Select all", 0, 1).clicked)
            app_select_all(a), kit_popup_close();
        if (rb_menu_item(ICON_DELETE, 0, "Delete", 0, has_sel).clicked)
            app_delete_selection(a), kit_popup_close();
        if (rb_menu_item(-1, 0, "Transparent selection", a->transparent_selection, 1).clicked)
            app_toggle_transparent_selection(a);
        kit_popup_end();
    }
    kit_col(0);
    kit_resp crop = rb_small_button(ICON_CROP, 0, "Crop", 0, 0, has_sel);
    kit_tooltip(&crop, "Crop to the selection");
    if (crop.clicked)
        app_crop(a);
    kit_resp resize = rb_small_button(ICON_RESIZE, 0, "Resize", 0, 0, 1);
    kit_tooltip(&resize, "Resize and skew");
    if (resize.clicked)
        app_open_resize(a);
    kit_resp rotate = rb_small_button(ICON_ROTATE, 0, "Rotate", 1, 0, 1);
    if (kit_popup_menu(&rotate, 200, 0))
    {
        view_rotate_items(a);
        kit_popup_end();
    }
    kit_end_layout();
}

static void tools_group(app *a)
{
    static const struct
    {
        app_tool tool;
        int icon;
        const char *tip;
    } tools[6] = { { TOOL_PENCIL, ICON_PENCIL, "Pencil" },       { TOOL_FILL, ICON_FILL, "Fill with color" },
                   { TOOL_TEXT, ICON_TEXT, "Text" },             { TOOL_ERASER, ICON_ERASER, "Eraser" },
                   { TOOL_PICKER, ICON_PICKER, "Color picker" }, { TOOL_MAGNIFIER, ICON_MAGNIFIER, "Magnifier" } };
    kit_col(2);
    for (int row = 0; row < 2; row++)
    {
        kit_row(0);
        for (int i = row * 3; i < row * 3 + 3; i++)
            if (rb_icon_button(tools[i].icon, 0, a->tool == tools[i].tool, 1, tools[i].tip).clicked)
                app_set_tool(a, tools[i].tool);
        kit_end_layout();
    }
    kit_end_layout();
    kit_space(4);
}

/* The Brushes gallery's samples: an "S" stroke laid by each brush, as coverage
 * for the size the cells are shown at (Rust: brush_preview, drawn at 96 px and scaled) */
static struct
{
    int side;
    uint8_t *cov[BRUSH_COUNT];
} previews;

static const uint8_t *brush_preview(brush_kind k, int side)
{
    if (previews.side != side)
    {
        for (int i = 0; i < BRUSH_COUNT; i++)
            free(previews.cov[i]), previews.cov[i] = NULL;
        previews.side = side;
    }
    if (previews.cov[k])
        return previews.cov[k];
    mask *m = mask_new(side, side);
    uint8_t *cov = calloc((size_t)side * side, 1);
    if (!m || !cov)
    {
        mask_free(m);
        free(cov);
        return NULL;
    }
    float s = side / 96.0f;
    uint64_t rng = 0x2545F4914F6CDD1DULL;
    pt prev = { 0 };
    for (int i = 0; i <= 40; i++)
    {
        float t = i / 40.0f;
        pt p = { (14 + t * 68) * s, (48 - sinf(t * 6.2831853f) * 22 + (t - 0.5f) * 20) * s };
        if (i)
            brush_segment(k, m, prev, p, 11 * s, &rng);
        prev = p;
    }
    float op = brush_opacity(k);
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++)
            cov[y * side + x] = (uint8_t)(mask_get(m, x, y) * op * 255 + 0.5f);
    mask_free(m);
    return previews.cov[k] = cov;
}

void view_free_previews(void)
{
    for (int i = 0; i < BRUSH_COUNT; i++)
        free(previews.cov[i]), previews.cov[i] = NULL;
    previews.side = 0;
}

static void brushes_group(app *a)
{
    kit_resp menu;
    kit_resp main = rb_big_split(ICON_BRUSHES, 0, "Brushes", a->tool == TOOL_BRUSH, 1, &menu);
    kit_tooltip(&main, brush_name(a->brush));
    if (main.clicked)
        app_set_tool(a, TOOL_BRUSH);
    if (kit_popup_menu(&menu, 3 * 52 + 8, 0))
    {
        kit_col(2);
        for (int row = 0; row < 3; row++)
        {
            kit_row(2);
            for (int c = 0; c < 3; c++)
            {
                brush_kind k = (brush_kind)(row * 3 + c);
                rect cell = kit_alloc(50, 50);
                kit_resp r = kit_interact(cell, kit_id_n("brush", k), SENSE_CLICK);
                if (a->brush == k || r.hovered)
                    rb_frame(kit_dst(), cell, r.hovered && a->brush != k, 0, a->brush == k);
                int side = S(40);
                const uint8_t *cov = brush_preview(k, side);
                if (cov)
                    gfx_mask(kit_dst(), cell.x + (cell.w - side) / 2, cell.y + (cell.h - side) / 2, cov, side, side, side, PAL_TEXT);
                kit_tooltip(&r, brush_name(k));
                if (r.clicked)
                {
                    app_pick_brush(a, k);
                    kit_popup_close();
                }
            }
            kit_end_layout();
        }
        kit_end_layout();
        kit_popup_end();
    }
}

static void shapes_group(app *a)
{
    static const char *names[SHAPE_COUNT];
    for (int i = 0; i < SHAPE_COUNT; i++)
        names[i] = shape_name((shape_kind)i);
    kit_resp more;
    int picked = rb_gallery(ICON_SHAPE, SHAPE_COUNT, 7, 3, 21, a->tool == TOOL_SHAPE ? (int)a->shape : -1, names, &more);
    if (picked >= 0)
        app_pick_shape(a, (shape_kind)picked);
    if (kit_popup_menu(&more, 240, 0))
    {
        for (int i = 0; i < SHAPE_COUNT; i++)
            if (rb_menu_item(ICON_SHAPE, (uint32_t)i, names[i], a->tool == TOOL_SHAPE && (int)a->shape == i, 1).clicked)
            {
                app_pick_shape(a, (shape_kind)i);
                kit_popup_close();
            }
        kit_popup_end();
    }
    kit_space(4);
    kit_col(0);
    /* The icons show the current choice: the colour in use, or a red slash for none */
    color line = view_rgba(a->primary), fill = view_rgba(a->secondary);
    kit_resp outline = rb_small_button(ICON_OUTLINE, a->shape_outline ? line : 0, "Outline", 1, 0, 1);
    kit_tooltip(&outline, a->shape_outline ? "Shape outline: solid Color 1" : "Shape outline: none");
    if (kit_popup_menu(&outline, 170, 0))
    {
        if (rb_menu_item(ICON_OUTLINE, 0, "No outline", !a->shape_outline, 1).clicked)
            app_set_shape_outline(a, 0), kit_popup_close();
        if (rb_menu_item(ICON_OUTLINE, line, "Solid color", a->shape_outline, 1).clicked)
            app_set_shape_outline(a, 1), kit_popup_close();
        kit_popup_end();
    }
    kit_resp fl = rb_small_button(ICON_FILL_SHAPE, a->shape_fill ? fill : 0, "Fill", 1, 0, 1);
    kit_tooltip(&fl, a->shape_fill ? "Shape fill: solid Color 2" : "Shape fill: none");
    if (kit_popup_menu(&fl, 170, 0))
    {
        if (rb_menu_item(ICON_FILL_SHAPE, 0, "No fill", !a->shape_fill, 1).clicked)
            app_set_shape_fill(a, 0), kit_popup_close();
        if (rb_menu_item(ICON_FILL_SHAPE, fill, "Solid color", a->shape_fill, 1).clicked)
            app_set_shape_fill(a, 1), kit_popup_close();
        kit_popup_end();
    }
    kit_end_layout();
}

static void size_group(app *a)
{
    app_tool t = a->tool;
    int sized = t == TOOL_PENCIL || t == TOOL_BRUSH || t == TOOL_ERASER || t == TOOL_SHAPE || t == TOOL_FILL;
    kit_resp r = rb_big_button(ICON_SIZE, 0, "Size", 1, 0, sized);
    if (!kit_popup_menu(&r, 200, POPUP_STAY_OPEN))
        return;
    if (t == TOOL_FILL)
    {
        rb_menu_header("Fill tolerance");
        kit_space(4);
        rb_value_slider(&a->fill_tolerance, 0, 100, 0, "%");
    }
    else
    {
        const float *presets;
        float max;
        float *size = app_tool_size(a, &presets, &max);
        for (int i = 0; size && i < 4; i++)
            if (rb_size_row(presets[i], fabsf(*size - presets[i]) < 0.5f).clicked)
            {
                *size = presets[i];
                kit_popup_close();
            }
        kit_separator();
        if (size)
            rb_value_slider(size, 1, max, 1, " px");
    }
    kit_popup_end();
}

void view_colors_group(app *a)
{
    for (int slot = 0; slot < 2; slot++)
    {
        rect pic;
        kit_resp r = rb_big_button_custom(slot ? "Color\n2" : "Color\n1", 0, a->editing_secondary == slot, 1, &pic);
        int side = S(slot ? 22 : 30);
        rb_paint_swatch(kit_dst(), rect_make(pic.x + (pic.w - side) / 2, pic.y + (pic.h - side) / 2, side, side),
                        view_rgba(slot ? a->secondary : a->primary), 0);
        kit_tooltip(&r, slot ? COLOR2_TIP : COLOR1_TIP);
        if (r.clicked)
            a->editing_secondary = slot;
    }
    kit_space(3);
    kit_col(0);
    for (int row = 0; row < 3; row++)
    {
        kit_row(0);
        for (int col = 0; col < 10; col++)
        {
            rgba c;
            const char *name = "Custom color";
            if (row < 2)
            {
                const uint8_t *p = APP_PALETTE[row * 10 + col];
                c = (rgba){ p[0], p[1], p[2], 255 };
                name = APP_PALETTE_NAMES[row * 10 + col];
            }
            else if (a->has_custom[col])
                c = a->custom[col];
            else
            {
                rb_empty_swatch(22);
                continue;
            }
            kit_push_n(row * 10 + col);
            kit_resp s = rb_swatch(view_rgba(c), 22);
            kit_pop_id();
            kit_tooltip(&s, name);
            if (s.clicked)
                app_set_active_color(a, c);
            if (s.right_clicked)
                a->secondary = c;
        }
        kit_end_layout();
    }
    kit_end_layout();
    kit_space(3);
    kit_resp edit = rb_big_button(ICON_EDIT_COLORS, 0, "Edit\ncolors", 0, 0, 1);
    kit_tooltip(&edit, "Edit colors\nSelect a color from the color palette.");
    if (edit.clicked)
        app_open_edit_colors(a);
}

static void home_tab(app *a)
{
    rb_group_begin("Clipboard", CONTENT_H);
    view_clipboard_group(a);
    rb_group_end();
    rb_group_begin("Image", CONTENT_H);
    image_group(a);
    rb_group_end();
    rb_group_begin("Tools", 46);
    tools_group(a);
    rb_group_end();
    rb_group_begin(NULL, CONTENT_H);
    brushes_group(a);
    rb_group_end();
    rb_group_begin("Shapes", CONTENT_H);
    shapes_group(a);
    rb_group_end();
    rb_group_begin(NULL, CONTENT_H);
    size_group(a);
    rb_group_end();
    rb_group_begin("Colors", CONTENT_H);
    view_colors_group(a);
    rb_group_end();
}

/* ---- View ---- */

static void view_tab(app *a)
{
    rb_group_begin("Zoom", CONTENT_H);
    if (rb_big_button(ICON_ZOOM_IN, 0, "Zoom\nin", 0, 0, 1).clicked)
        app_zoom_step(a, 1, NULL);
    if (rb_big_button(ICON_ZOOM_OUT, 0, "Zoom\nout", 0, 0, 1).clicked)
        app_zoom_step(a, 0, NULL);
    if (rb_big_button(ICON_ZOOM_100, 0, "100\n%", 0, 0, 1).clicked)
        app_zoom_at(a, 1, NULL);
    rb_group_end();
    rb_group_begin("Show or hide", CONTENT_H);
    kit_col(0);
    kit_checkbox("Rulers", &a->show_rulers);
    kit_checkbox("Gridlines", &a->show_grid);
    kit_checkbox("Status bar", &a->show_status);
    kit_end_layout();
    kit_space(8);
    rb_group_end();
    rb_group_begin("Display", CONTENT_H);
    kit_resp fs = rb_big_button(ICON_FULL_SCREEN, 0, "Full\nscreen", 0, 0, 1);
    kit_tooltip(&fs, "Full screen (F11)");
    if (fs.clicked)
        app_set_full_screen(a, 1);
    kit_col(0);
    kit_space(4);
    kit_checkbox("Thumbnail", &a->show_thumbnail);
    kit_end_layout();
    kit_space(8);
    rb_group_end();
}

/* ---- Text (while the Text tool is chosen) ---- */

static void text_tab(app *a)
{
    rb_group_begin("Clipboard", CONTENT_H);
    view_clipboard_group(a);
    rb_group_end();
    rb_group_begin("Font", 52);
    text_style st = a->text_style;
    kit_col(6);
    kit_row(ITEM_SPACING_X);
    const char *current = st.family >= 0 && st.family < fonts_count() ? fonts_name(st.family) : "\xE2\x80\x94";
    if (kit_combo("font", current, 150, 420))
    {
        for (int i = 0, n = fonts_count(); i < n; i++)
            if (kit_combo_item(fonts_name(i), i == st.family))
                st.family = i;
        kit_combo_end();
    }
    char size[16];
    snprintf(size, sizeof size, "%g", st.size_pt);
    if (kit_combo("font-size", size, 46, COMBO_HEIGHT))
    {
        for (int i = 0; i < 16; i++)
        {
            snprintf(size, sizeof size, "%g", APP_TEXT_SIZES[i]);
            if (kit_combo_item(size, st.size_pt == APP_TEXT_SIZES[i]))
                st.size_pt = APP_TEXT_SIZES[i];
        }
        kit_combo_end();
    }
    kit_end_layout();
    kit_row(0);
    int *flags[4] = { &st.bold, &st.italic, &st.underline, &st.strike };
    static const int icons[4] = { ICON_BOLD, ICON_ITALIC, ICON_UNDERLINE, ICON_STRIKE };
    static const char *const tips[4] = { "Bold", "Italic", "Underline", "Strikethrough" };
    for (int i = 0; i < 4; i++)
        if (rb_icon_button(icons[i], 0, *flags[i], 1, tips[i]).clicked)
            *flags[i] = !*flags[i];
    kit_end_layout();
    kit_end_layout();
    kit_space(6);
    rb_group_end();
    rb_group_begin("Background", 46);
    kit_col(2);
    if (rb_small_button(ICON_OPAQUE, 0, "Opaque", 0, st.opaque, 1).clicked)
        st.opaque = 1;
    if (rb_small_button(ICON_TRANSPARENT, 0, "Transparent", 0, !st.opaque, 1).clicked)
        st.opaque = 0;
    kit_end_layout();
    rb_group_end();
    if (memcmp(&st, &a->text_style, sizeof st))
        app_set_text_style(a, st);
    rb_group_begin("Colors", CONTENT_H);
    view_colors_group(a);
    rb_group_end();
}

/* ---- Together ---- */

static color kid_color(const uint8_t c[3])
{
    return RGB3(c[0], c[1], c[2]);
}

/* A row in the Friends list: the host can send anyone else out (1: kicked) */
static int friend_row(app *a, const kid *k, int hosting)
{
    char label[96], tip[96];
    int me = a->session && k->id == a->session->me;
    snprintf(label, sizeof label, me ? "%s (you)" : "%s", k->name);
    snprintf(tip, sizeof tip, "Kick out %s", k->name);
    return rb_kid_row(label, k->name, kid_color(k->color), hosting && !me ? tip : NULL);
}

static void together_tab(app *a)
{
    app_discover(a);
    int hosting = app_hosting(a);
    rb_group_begin("Draw together", CONTENT_H);
    if (!a->link)
    {
        kit_resp start = rb_big_button(ICON_TOGETHER, 0, "Start\ndrawing", 0, 0, 1);
        kit_tooltip(&start, "Let friends on this Wi-Fi draw on this picture with you");
        if (start.clicked)
            app_start_together(a);
        kit_resp join = rb_big_button(ICON_JOIN_ROOM, 0, "Join a\ndrawing", 0, 0, 1);
        kit_tooltip(&join, "Draw on a friend's picture");
        if (join.clicked)
            app_open_join_dialog(a);
    }
    else
    {
        kit_resp leave = rb_big_button(ICON_LEAVE_ROOM, 0, "Leave", 0, 0, 1);
        kit_tooltip(&leave, "Stop drawing together (you keep the picture)");
        if (leave.clicked)
            app_leave_together(a);
    }
    /* Who gets in: chosen before starting, and changeable while hosting */
    if (hosting || (!a->link && !a->session))
    {
        kit_col(0);
        kit_space(8);
        int let_in = !(hosting && a->session ? a->session->ask_first : a->ask_first);
        if (kit_checkbox("Let friends in\nwithout asking", &let_in))
            app_set_ask_first(a, !let_in);
        kit_end_layout();
        kit_space(6);
    }
    rb_group_end();

    rb_group_begin("Friends", CONTENT_H);
    const kid_list *kids = a->session ? &a->session->kids : NULL;
    int n = kids ? kids->n : 0;
    peer_id kicked = 0;
    int any_kicked = 0;
    if (!n)
    {
        char hint[160];
        if (a->asking)
            snprintf(hint, sizeof hint, "Waiting for %s\nto let you in\xE2\x80\xA6", a->asking);
        else
            snprintf(hint, sizeof hint, "%s", a->link ? "Connecting\xE2\x80\xA6" : "Start or join a drawing\nto draw with friends.");
        kit_space(6);
        kit_label_ex(hint, FONT_BODY, 0, PAL_TEXT_WEAK);
        kit_space(6);
    }
    /* Three columns at most; a big room's full list opens from the "more" button */
    enum { SHOWN = 9 };
    for (int i = 0; i < n && i < SHOWN; i += 3)
    {
        if (i)
            kit_space(16);
        kit_push_n(i);
        kit_col(0);
        kit_space(2);
        for (int j = i; j < i + 3 && j < n && j < SHOWN; j++)
            if (friend_row(a, &kids->v[j], hosting))
                kicked = kids->v[j].id, any_kicked = 1;
        kit_end_layout();
        kit_pop_id();
    }
    if (n > SHOWN)
    {
        char count[16], title[48];
        snprintf(count, sizeof count, "+%d", n - SHOWN);
        snprintf(title, sizeof title, "Everyone drawing (%d)", n);
        kit_space(8);
        kit_resp more = rb_more_button(count);
        kit_tooltip(&more, title);
        if (kit_popup_menu(&more, 0, POPUP_STAY_OPEN))
        {
            kit_space(2);
            kit_label_ex(title, FONT_BODY, FONT_BOLD, PAL_TEXT);
            kit_space(6);
            /* Up to seven names a column and four columns side by side; a bigger room scrolls */
            int columns = (n + 6) / 7;
            columns = columns < 1 ? 1 : columns > 4 ? 4 : columns;
            int per = (n + columns - 1) / columns;
            kit_scroll_begin("everyone", 0, 7 * 27, SCROLL_V);
            kit_row(0);
            for (int c = 0; c * per < n; c++)
            {
                if (c)
                    kit_space(16);
                kit_push_n(c);
                kit_col(0);
                for (int j = c * per; j < (c + 1) * per && j < n; j++)
                    if (friend_row(a, &kids->v[j], hosting))
                        kicked = kids->v[j].id, any_kicked = 1;
                kit_end_layout();
                kit_pop_id();
            }
            kit_end_layout();
            kit_scroll_end();
            kit_popup_end();
        }
    }
    rb_group_end();
    if (any_kicked)
        app_kick(a, kicked);

    if (a->has_invite)
    {
        char addr[64];
        os_addr_format(a->invite, addr, sizeof addr);
        rb_group_begin("Invite", CONTENT_H);
        kit_col(0);
        kit_space(8);
        kit_label_ex("Friends can join at", FONT_BODY, 0, PAL_TEXT_WEAK);
        kit_label_ex(addr, FONT_HEADING, FONT_BOLD, PAL_TEXT);
        kit_end_layout();
        kit_space(6);
        rb_group_end();
    }

    rb_group_begin("Me", CONTENT_H);
    int editable = !a->link;
    kit_col(3);
    kit_space(4);
    kit_row_centered(ITEM_SPACING_X, INTERACT_H);
    rb_avatar(a->me_name, kid_color(a->me_color), 20);
    int f = kit_text_field("me", a->me_name, sizeof a->me_name, 20, 130, NULL, editable ? 0 : FIELD_DISABLED);
    if (f & KIT_LOST_FOCUS)
        app_set_me_name(a, a->me_name);
    kit_end_layout();
    kit_row(2);
    for (int i = 0; i < N_KID_COLORS; i++)
    {
        rect d = kit_alloc(13, 13);
        kit_resp r = kit_interact(d, kit_id_n("me-color", i), SENSE_CLICK);
        float cx = d.x + d.w / 2.0f, cy = d.y + d.h / 2.0f;
        gfx_circle(kit_dst(), cx, cy, SF(6), kid_color(KID_COLORS[i]));
        if (!memcmp(KID_COLORS[i], a->me_color, 3))
            gfx_ring(kit_dst(), cx, cy, SF(6), SF(1.5f), PAL_TEXT);
        if (editable && r.clicked)
            app_set_me_color(a, KID_COLORS[i]);
    }
    kit_end_layout();
    kit_end_layout();
    kit_space(6);
    rb_group_end();
}

/* ---- the tab strip and the ribbon body (Rust: ribbon) ---- */

void view_ribbon(app *a, rect full)
{
    surface *d = kit_dst();
    if (a->tab == TAB_TEXT && a->tool != TOOL_TEXT)
        a->tab = TAB_HOME;
    rect strip = rect_make(full.x, full.y, full.w, S(TAB_STRIP_H));
    gfx_fill(d, strip, PAL_CHROME);
    rect selected = { 0 };
    int have_selected = 0;
    kit_area(strip, 1, 0);
    kit_set_gap(TAB_FLARE);
    kit_space(4);
    kit_resp file = rb_file_tab();
    V.file_tab = file.r;
    if (file.clicked)
    {
        a->file_menu_open = !a->file_menu_open;
        a->file_menu_formats = 0;
    }
    static const char *const labels[4] = { "Home", "View", "Together", "Text" };
    for (int t = 0; t < (a->tool == TOOL_TEXT ? 4 : 3); t++)
    {
        int is_selected = (int)a->tab == t && !a->ribbon_minimized;
        kit_resp r = rb_tab(labels[t], is_selected, t == TAB_TEXT);
        if (r.clicked)
        {
            a->tab = (app_tab)t;
            a->ribbon_minimized = 0;
        }
        if (is_selected)
            selected = r.r, have_selected = 1;
    }
    kit_area_end();
    kit_area(strip, 1, 0);
    kit_row_rtl(0, TAB_STRIP_H);
    kit_space(6);
    if (rb_icon_button(ICON_HELP, 0, 0, 1, "About Paint").clicked)
        a->about_open = 1;
    int min = a->ribbon_minimized;
    if (rb_icon_button(min ? ICON_DROPDOWN : ICON_COLLAPSE, 0, 0, 1, min ? "Expand the Ribbon" : "Minimize the Ribbon").clicked)
        a->ribbon_minimized = !a->ribbon_minimized;
    kit_end_layout();
    kit_area_end();

    /* The line under the tabs, open beneath the selected one so it joins the ribbon */
    int line_y = strip.y + strip.h - HAIR();
    if (min)
    {
        rb_tab_line(full.x, full.x + full.w, line_y, NULL);
        return;
    }
    rect body = rect_make(full.x, strip.y + strip.h, full.w, S(CONTENT_H + 26));
    gfx_fill(d, body, PAL_RIBBON);
    rb_tab_line(full.x, full.x + full.w, line_y, have_selected ? &selected : NULL);
    if (have_selected)
        rb_selected_tab_outline(selected);
    hair_h(d, full.x, full.x + full.w - 1, body.y + body.h - HAIR(), PAL_RIBBON_BORDER);
    kit_area(rect_make(body.x + S(4), body.y + S(4), body.w - S(8), body.h - S(6)), 1, 0);
    /* A narrow window scrolls the groups sideways (no bars, as in the Rust) */
    kit_scroll_begin("ribbon", 0, 0, SCROLL_H | SCROLL_HIDE_BARS);
    kit_row(0);
    switch (a->tab)
    {
    case TAB_HOME:
        home_tab(a);
        break;
    case TAB_VIEW:
        view_tab(a);
        break;
    case TAB_TOGETHER:
        together_tab(a);
        break;
    case TAB_TEXT:
        text_tab(a);
        break;
    }
    kit_end_layout();
    kit_scroll_end();
    kit_area_end();
}
