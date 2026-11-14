/*
 * Paint (C port) - the window's frame (Rust: app.rs's ui, title_bar, status_bar, file_menu,
 * thumbnail, door_notices and paint_toasts)
 *
 * The window is built top to bottom as the Rust's panels were: the title bar
 * (the Quick Access Toolbar), the tab strip and ribbon, the status bar at
 * the bottom and the picture area in between; then the File menu, the
 * thumbnail, the dialogs, the notices at the door and the toasts on top.
 * The window keeps the system's own title bar on every platform, so the
 * strip at the top is just the toolbar.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app/view.h"

view_state V;

color view_rgba(rgba c)
{
    return RGB3(c.r, c.g, c.b);
}

const char *view_tip(char *buf, size_t cap, const char *what, int mods, int key)
{
    char keys[32];
    snprintf(buf, cap, "%s (%s)", what, app_shortcut(mods, key, keys, sizeof keys));
    return buf;
}

/* ---- the title bar: the logo and the Quick Access Toolbar (Rust: title_bar) ---- */

static void title_bar(app *a, rect bar)
{
    char tip[64];
    gfx_fill(kit_dst(), bar, PAL_CHROME);
    kit_area(rect_make(bar.x + S(6), bar.y, bar.w - S(12), bar.h), 1, 0);
    kit_row_centered(0, TITLE_BAR_H);
    kit_space(6);
    rect logo = kit_alloc(20, 20);
    icon_draw(kit_dst(), ICON_LOGO, 0, logo.x, logo.y, logo.w, 1);
    kit_space(6);
    if (rb_icon_button(ICON_SAVE, 0, 0, 1, view_tip(tip, sizeof tip, "Save", KMOD_CTRL, 'S')).clicked)
        app_save(a);
    int can_undo = history_can_undo(a->history) || app_is_dirty(a);
    if (rb_icon_button(ICON_UNDO, 0, 0, can_undo, view_tip(tip, sizeof tip, "Undo", KMOD_CTRL, 'Z')).clicked)
        app_undo(a);
    if (rb_icon_button(ICON_REDO, 0, 0, history_can_redo(a->history), view_tip(tip, sizeof tip, "Redo", KMOD_CTRL | KMOD_SHIFT, 'Z')).clicked)
        app_redo(a);
    kit_end_layout();
    kit_area_end();
}

/* ---- the status bar (Rust: status_bar) ---- */

static void status_bar(app *a, rect bar)
{
    surface *d = kit_dst();
    char text[160];
    gfx_fill(d, bar, PAL_STATUS);
    hair_h(d, bar.x, bar.x + bar.w - 1, bar.y, PAL_STATUS_BORDER);
    kit_area(rect_make(bar.x + S(4), bar.y, bar.w - S(8), bar.h), 1, 0);
    kit_row_centered(0, STATUS_H);
    kit_space(4);
    text[0] = 0;
    if (a->has_cursor_px)
        snprintf(text, sizeof text, "%d, %dpx", a->cursor_px_x, a->cursor_px_y);
    rb_status_item(ICON_CURSOR_POS, text, 130);
    int w, h;
    text[0] = 0;
    if (app_selection_size(a, &w, &h))
        snprintf(text, sizeof text, "%d \xC3\x97 %dpx", w, h);
    rb_status_item(ICON_SELECTION_SIZE, text, 130);
    app_canvas_size(a, &w, &h);
    snprintf(text, sizeof text, "%d \xC3\x97 %dpx", w, h);
    rb_status_item(ICON_CANVAS_SIZE, text, 150);
    if (a->has_meta)
    {
        char size[32];
        app_format_bytes(a->file_size, size, sizeof size);
        snprintf(text, sizeof text, "Size: %s", size);
        rb_status_item(ICON_FILE_SIZE, text, 140);
    }
    else
        kit_alloc(140, 22);
    kit_end_layout();

    /* Drawing together: who's here, or a drawing nearby to join (left of the zoom, when there's room) */
    const nearby *invite = NULL;
    if (!a->link && a->discovery)
    {
        int n;
        const nearby *near = app_nearby(a, &n);
        for (int i = 0; i < n && !invite; i++)
            if (near[i].compatible)
                invite = &near[i];
    }
    kit_row_rtl(2, STATUS_H);
    kit_space(8);
    kit_resp in = rb_status_button(0);
    kit_tooltip(&in, "Zoom in");
    if (in.clicked)
        app_zoom_step(a, 1, NULL);
    float t = app_zoom_slider(a);
    if (rb_zoom_slider(&t, app_zoom_slider_tick(), 110))
        app_set_zoom_slider(a, t);
    kit_resp out = rb_status_button(1);
    kit_tooltip(&out, "Zoom out");
    if (out.clicked)
        app_zoom_step(a, 0, NULL);
    snprintf(text, sizeof text, "%.0f%%", a->zoom * 100);
    rect z = kit_alloc(44, 20);
    text_in(d, ui_font(FONT_BODY, 0), z, ALIGN_CENTER, text, PAL_TEXT);
    kit_space(6);
    font *f = ui_font(12, 0);
    if (app_together_status(a, text, sizeof text))
    {
        int tw = text_width(f, text, -1) + S(36);
        if (kit_avail().w >= tw)
            rb_status_item(ICON_TOGETHER, text, (int)(tw * 100 / ui_scale));
    }
    if (invite)
    {
        snprintf(text, sizeof text, "Join %s's drawing", invite->name);
        if (kit_avail().w >= text_width(f, text, -1) + S(36))
        {
            kit_resp link = rb_status_link(ICON_TOGETHER, text);
            kit_tooltip(&link, "A drawing nearby: draw on it together");
            if (link.clicked)
            {
                char addr[64];
                os_addr_format(invite->addr, addr, sizeof addr);
                app_request(a, PENDING_JOIN, addr);
            }
        }
    }
    kit_end_layout();
    kit_area_end();
}

/* ---- the File menu (Rust: file_menu and its two columns) ---- */

static void recent_column(app *a, int width)
{
    surface *d = kit_dst();
    rect head = kit_alloc_px(width, S(30));
    text_in(d, ui_font(13, 0), rect_make(head.x + S(12), head.y, head.w - S(12), head.h), ALIGN_LEFT, "Recent pictures", PAL_TEXT);
    hair_h(d, head.x + S(8), head.x + head.w - S(8) - 1, head.y + head.h - HAIR(), PAL_RIBBON_BORDER);
    int open = -1;
    for (int i = 0; i < a->n_recent; i++)
    {
        const char *path = a->recent[i], *name = path;
        for (const char *s = path; *s; s++)
            if (*s == '/' || *s == '\\')
                name = s + 1;
        rect r = kit_alloc_px(width, S(24));
        kit_resp res = kit_interact(r, kit_id_n("recent", i), SENSE_CLICK);
        if (res.hovered)
            rb_frame(d, rect_make(r.x + S(4), r.y + S(1), r.w - S(8), r.h - S(2)), 1, 0, 0);
        char num[4];
        snprintf(num, sizeof num, "%d", i + 1);
        text_in(d, ui_font(12, 0), rect_make(r.x + S(14), r.y, S(18), r.h), ALIGN_LEFT, num, PAL_TEXT);
        text_in(d, ui_font(FONT_BODY, 0), rect_make(r.x + S(32), r.y, r.w - S(40), r.h), ALIGN_LEFT, *name ? name : path, PAL_TEXT);
        kit_tooltip(&res, path);
        if (res.clicked)
            open = i;
    }
    if (!a->n_recent)
    {
        kit_space(8);
        kit_row(0);
        kit_space(14);
        kit_label_ex("Pictures you open or save appear here.", FONT_BODY, 0, PAL_TEXT_WEAK);
        kit_end_layout();
    }
    if (open >= 0)
    {
        a->file_menu_open = 0;
        app_request(a, PENDING_OPEN, a->recent[open]);
    }
}

static void formats_column(app *a, int width)
{
    static const struct
    {
        int kind;
        const char *title, *desc, *ext;
    } formats[5] = {
        { FILE_KIND_PNG, "PNG picture", "Save a photo or drawing with high quality and use it on your computer or on the web.", "png" },
        { FILE_KIND_JPEG, "JPEG picture", "Save a photo with good quality and use it on your computer, in email, or on the web.", "jpg" },
        { FILE_KIND_BMP, "BMP picture", "Save any kind of picture with high quality and use it on your computer.", "bmp" },
        { FILE_KIND_GIF, "GIF picture", "Save a simple drawing with lower quality and use it in email or on the web.", "gif" },
        { FILE_KIND_OTHER, "Other formats", "Open the Save As dialog box to select from all possible file types.", NULL },
    };
    surface *d = kit_dst();
    rect head = kit_alloc_px(width, S(30));
    text_in(d, ui_font(13, 0), rect_make(head.x + S(12), head.y, head.w - S(12), head.h), ALIGN_LEFT, "Save a copy of the document", PAL_TEXT);
    hair_h(d, head.x + S(8), head.x + head.w - S(8) - 1, head.y + head.h - HAIR(), PAL_RIBBON_BORDER);
    for (int i = 0; i < 5; i++)
    {
        rect r = kit_alloc_px(width, S(54));
        kit_resp res = kit_interact(r, kit_id_n("format", i), SENSE_CLICK);
        if (res.hovered)
            rb_frame(d, rect_make(r.x + S(4), r.y + S(1), r.w - S(8), r.h - S(2)), 1, 0, 0);
        icon_at(d, ICON_FILE_TYPE, (uint32_t)formats[i].kind, r.x + SF(26), r.y + r.h / 2.0f, S(32), 1);
        text_draw(d, ui_font(13, 0), r.x + S(50), r.y + S(8), formats[i].title, -1, PAL_TEXT);
        text_wrap(d, ui_font(FONT_SMALL, 0), rect_make(r.x + S(50), r.y + S(25), r.w - S(62), 0), ALIGN_LEFT, formats[i].desc, PAL_TEXT_WEAK);
        if (res.clicked)
        {
            a->file_menu_open = 0;
            app_save_as_format(a, formats[i].ext);
        }
    }
}

static void file_menu(app *a)
{
    if (!a->file_menu_open)
        return;
    static const struct
    {
        int icon;
        const char *label;
    } items[7] = { { ICON_NEW, "New" },           { ICON_OPEN, "Open" },         { ICON_SAVE, "Save" }, { ICON_SAVE_AS, "Save as" },
                   { ICON_PROPERTIES, "Properties" }, { ICON_ABOUT, "About Paint" }, { ICON_EXIT, "Exit" } };
    int lw = S(210), rw = S(300), tall = S(338);
    kit_overlay_begin("file-menu", rect_make(V.file_tab.x, V.file_tab.y + V.file_tab.h - 1, lw + HAIR() + rw + 2, 0), 1);
    rect in = kit_avail();
    surface *d = kit_dst();
    kit_area(rect_make(in.x, in.y, lw, 1 << 16), 0, 0);
    kit_space(4);
    int formats = -1, clicked = -1;
    for (int i = 0; i < 7; i++)
    {
        if (i >= 4)
        {
            kit_space(3);
            rect r = kit_alloc_px(lw, 1);
            hair_h(d, r.x + S(8), r.x + r.w - S(8) - 1, r.y, PAL_RIBBON_BORDER);
            kit_space(3);
        }
        int arrow = i == 3;
        kit_resp r = rb_file_menu_item(items[i].icon, items[i].label, arrow, arrow && a->file_menu_formats);
        if (r.hovered)
            formats = arrow;
        if (r.clicked)
            clicked = i;
    }
    kit_space(4);
    rect left = kit_area_end();
    if (formats >= 0)
        a->file_menu_formats = formats;
    hair_v(d, in.x + lw, in.y, in.y + tall - 1, PAL_RIBBON_BORDER);
    kit_area(rect_make(in.x + lw + HAIR(), in.y, rw, 1 << 16), 0, 0);
    if (a->file_menu_formats)
        formats_column(a, rw);
    else
        recent_column(a, rw);
    rect right = kit_area_end();
    int h = left.h > right.h ? left.h : right.h;
    kit_alloc_px(lw + HAIR() + rw, h > tall ? h : tall);
    rect menu = kit_overlay_end();

    /* A press anywhere else (but the File tab, which toggles it) or Escape closes it */
    int n;
    const ui_event *ev = kit_events(&n);
    for (int i = 0; i < n; i++)
        if (ev[i].type == EV_DOWN && !rect_has(menu, ev[i].x, ev[i].y) && !rect_has(V.file_tab, ev[i].x, ev[i].y))
            a->file_menu_open = 0;
    if (kit_key(KEY_ESCAPE, 0))
        a->file_menu_open = 0;
    switch (clicked)
    {
    case 0:
        a->file_menu_open = 0;
        app_request(a, PENDING_NEW, NULL);
        break;
    case 1:
        a->file_menu_open = 0;
        app_request(a, PENDING_OPEN, NULL);
        break;
    case 2:
        a->file_menu_open = 0;
        app_save(a);
        break;
    case 3:
        a->file_menu_open = 0;
        app_save_as(a);
        break;
    case 4:
        a->file_menu_open = 0;
        app_open_properties(a);
        break;
    case 5:
        a->file_menu_open = 0;
        a->about_open = 1;
        break;
    case 6:
        a->file_menu_open = 0;
        if (app_close_requested(a))
            host_quit();
        break;
    }
}

/* ---- View > Thumbnail: a small window showing the whole picture (Rust: thumbnail) ---- */

static void thumbnail(app *a)
{
    if (!a->show_thumbnail)
        return;
    int closed = 0;
    if (!kit_window_begin("thumbnail", "Thumbnail", 240, 170, WIN_RESIZABLE, &closed))
        return;
    rect avail = kit_avail();
    int w = avail.w > S(40) ? avail.w : S(40), h = avail.h > S(30) ? avail.h : S(30);
    rect area = kit_alloc_px(w, h);
    const img *pic = view_picture(a);
    double scale = fmin(w / (double)pic->w, h / (double)pic->h);
    int pw = (int)(pic->w * scale), ph = (int)(pic->h * scale);
    rect r = rect_make(area.x + (w - pw) / 2, area.y + (h - ph) / 2, pw, ph);
    if (pw > 0 && ph > 0)
        gfx_canvas(kit_dst(), r, pic, 0, 0, scale);
    gfx_frame(kit_dst(), rect_inset(r, -HAIR()), HAIR(), COLOR(0xA0A0A0));
    kit_window_end();
    if (closed)
        a->show_thumbnail = 0;
}

/* ---- notices at the bottom of the picture ---- */

/* Friends at the door, and my own wait to be let in; returns the height they take (Rust: door_notices) */
static int door_notices(app *a)
{
    int cx = V.view.x + V.view.w / 2, start = V.view.y + V.view.h - S(18), bottom = start;
    char text[160], tip[96];
    int w, h;
    if (a->asking)
    {
        snprintf(text, sizeof text, "Waiting for %s to let you in\xE2\x80\xA6", a->asking);
        rb_door_pill_size(NULL, text, 0, &w, &h);
        kit_overlay_begin("asking", rect_make(cx - w / 2, bottom - h, w, h), 0);
        int stop = rb_door_pill(NULL, 0, text, NULL, "Stop waiting");
        kit_overlay_end();
        bottom -= h + S(6);
        if (stop >= 0)
            app_leave_together(a);
    }
    if (!a->session || !app_hosting(a))
        return start - bottom;
    /* A few at a time; a crowd at the door gets one more notice for the rest */
    enum { SHOWN = 3 };
    const kid_list *waiting = &a->session->waiting;
    peer_id ids[64];
    int answer = -1, n_ids = 0;
    for (int i = 0; i < waiting->n && i < SHOWN; i++)
    {
        const kid *k = &waiting->v[i];
        snprintf(text, sizeof text, "%s wants to join", k->name);
        snprintf(tip, sizeof tip, "Let %s in", k->name);
        rb_door_pill_size(k->name, text, 1, &w, &h);
        kit_push_n((int)k->id);
        kit_overlay_begin("waiting", rect_make(cx - w / 2, bottom - h, w, h), 0);
        int yes = rb_door_pill(k->name, RGB3(k->color[0], k->color[1], k->color[2]), text, tip, "Not this time");
        kit_overlay_end();
        kit_pop_id();
        bottom -= h + S(6);
        if (yes >= 0)
            answer = yes, ids[0] = k->id, n_ids = 1;
    }
    if (waiting->n > SHOWN)
    {
        int rest = waiting->n - SHOWN;
        snprintf(text, sizeof text, rest == 1 ? "1 more wants to join" : "%d more want to join", rest);
        rb_door_pill_size(NULL, text, 1, &w, &h);
        kit_overlay_begin("waiting-more", rect_make(cx - w / 2, bottom - h, w, h), 0);
        int yes = rb_door_pill(NULL, 0, text, "Let them all in", "Not this time");
        kit_overlay_end();
        bottom -= h + S(6);
        if (yes >= 0)
        {
            answer = yes, n_ids = 0;
            for (int i = SHOWN; i < waiting->n && n_ids < 64; i++)
                ids[n_ids++] = waiting->v[i].id;
        }
    }
    if (answer >= 0)
        app_let_in(a, ids, n_ids, answer);
    return start - bottom;
}

/* Notices fading out over the bottom of the picture, above the door notices and
 * on top of everything else, menus and dialogs too (Rust: paint_toasts) */
static int toasts_lift;

static void paint_toasts(void *arg)
{
    const app *a = arg;
    surface *d = kit_dst();
    font *f = ui_font(13, 0);
    double now = os_now();
    int cx = V.view.x + V.view.w / 2, bottom = V.view.y + V.view.h - S(18) - toasts_lift;
    for (int i = a->n_toasts - 1; i >= 0; i--)
    {
        const toast *t = &a->toasts[i];
        if (t->until <= now)
            continue;
        double fade = (t->until - now) / 0.5;
        fade = fade < 0 ? 0 : fade > 1 ? 1 : fade;
        int w = text_width(f, t->text, -1) + S(28), h = font_height(f) + S(12);
        rect pill = rect_make(cx - w / 2, bottom - S(14) - h / 2, w, h);
        gfx_round(d, pill, S(14), COLOR_A(0x282830, (int)(215 * fade)));
        text_in(d, f, pill, ALIGN_CENTER, t->text, COLOR_A(0xFFFFFF, (int)(255 * fade)));
        bottom -= h + S(6);
    }
    kit_repaint_after(0.05);
}

static void toasts(app *a, int lift)
{
    toasts_lift = lift;
    if (a->n_toasts)
        kit_on_top(paint_toasts, a);
}

/* ---- the frame (Rust: PaintApp::ui) ---- */

void view_build(app *a)
{
    view_canvas_input(a);
    rect scr = kit_screen();
    if (a->full_screen)
    {
        V.view = rect_make(0, 0, 0, 0);
        view_full_screen(a, scr);
        view_dialogs(a);
        return;
    }
    title_bar(a, rect_make(0, 0, scr.w, S(TITLE_BAR_H)));
    int rib_h = S(TAB_STRIP_H) + (a->ribbon_minimized ? 0 : S(CONTENT_H + 26));
    rect rib = rect_make(0, S(TITLE_BAR_H), scr.w, rib_h);
    view_ribbon(a, rib);
    int bottom = scr.h;
    if (a->show_status)
    {
        bottom -= S(STATUS_H);
        status_bar(a, rect_make(0, bottom, scr.w, S(STATUS_H)));
    }
    view_canvas(a, rect_make(0, rib.y + rib.h, scr.w, bottom - rib.y - rib.h));
    file_menu(a);
    thumbnail(a);
    view_dialogs(a);
    toasts(a, door_notices(a));
}

/* ---- keys and typing (Rust: handle_keys) ---- */

int view_keys(app *a)
{
    int n, used = 0;
    const ui_event *ev = kit_events(&n);
    /* A text field of the views has the keyboard: the app gets nothing */
    int typing = kit_focused() != 0, text_free = !typing && kit_text() != NULL;
    for (int i = 0; i < n; i++)
    {
        const ui_event *e = &ev[i];
        int mods = e->mods & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT);
        if (e->type == EV_KEY_UP)
            app_key_up(a, e->key, mods);
        else if (e->type == EV_KEY && !typing && kit_key(e->key, mods))
            used |= app_key(a, e->key, mods);
        else if (e->type == EV_TEXT && text_free && e->text)
        {
            app_text(a, e->text);
            used = 1;
        }
    }
    if (used)
        kit_repaint();
    return used;
}

void view_free(void)
{
    view_free_previews();
    view_free_picture();
}
