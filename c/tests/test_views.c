/*
 * Paint (C port) - tests of the window's views: frames built into memory and driven with events
 *
 * The whole window is built with view_build into a surface, as main.c does,
 * with pointer and key events through the kit. With a folder as argument
 * each scene is also written there as a PNG, to look at:
 *
 *   build/<target>/tests/test_views out-dir
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/view.h"
#include "os/os.h"
#include "paint/raster.h"
#include "ui/font.h"

static int failures;
#define CHECK(cond, ...)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            printf("FAIL %s:%d: %s: ", __func__, __LINE__, #cond);          \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static surface *win;
static app *A;
static const char *out_dir;
static double clock_now = 1000;

static void frame(void)
{
    for (int pass = 0; pass < 3; pass++)
    {
        kit_begin(win, clock_now);
        view_build(A);
        if (!pass)
            view_keys(A);
        if (!kit_end())
            break;
    }
}

static void send(int type, int x, int y, int button)
{
    ui_event e = { 0 };
    e.type = type;
    e.x = x;
    e.y = y;
    e.button = button;
    e.clicks = 1;
    e.time = clock_now;
    kit_event(&e);
}

static void move_to(int x, int y)
{
    send(EV_MOVE, x, y, 0);
    frame();
}

static void click(int x, int y, int button)
{
    send(EV_MOVE, x, y, 0);
    send(EV_DOWN, x, y, button);
    frame();
    send(EV_UP, x, y, button);
    frame();
}

static void drag(int x0, int y0, int x1, int y1)
{
    send(EV_MOVE, x0, y0, 0);
    send(EV_DOWN, x0, y0, BTN_LEFT);
    frame();
    for (int i = 1; i <= 8; i++)
        send(EV_MOVE, x0 + (x1 - x0) * i / 8, y0 + (y1 - y0) * i / 8, 0);
    frame();
    send(EV_UP, x1, y1, BTN_LEFT);
    frame();
}

static void key(int k, int mods)
{
    ui_event e = { 0 };
    e.type = EV_KEY;
    e.key = k;
    e.mods = mods;
    kit_event(&e);
    frame();
}

static void type(const char *text)
{
    ui_event e = { 0 };
    e.type = EV_TEXT;
    e.text = text;
    kit_event(&e);
    frame();
}

static void shot(const char *name)
{
    if (!out_dir)
        return;
    img *im = img_new(win->w, win->h, (rgba){ 0, 0, 0, 255 });
    for (int i = 0; i < win->w * win->h; i++)
    {
        uint32_t p = win->px[i];
        im->px[4 * i] = (uint8_t)(p >> 16), im->px[4 * i + 1] = (uint8_t)(p >> 8), im->px[4 * i + 2] = (uint8_t)p;
    }
    char path[512];
    const char *err;
    snprintf(path, sizeof path, "%s/%s.png", out_dir, name);
    if (img_save(im, path, &err) == 0)
        printf("wrote %s\n", path);
    img_free(im);
}

static void reset(int scale)
{
    if (A)
        app_free(A);
    view_free();
    memset(&V, 0, sizeof V);
    theme_set_scale(scale);
    surface_free(win);
    win = surface_new(1460 * scale / 100, 900 * scale / 100);
    kit_init();
    A = app_new();
    app_set_scale(A, scale / 100.0f);
    move_to(-10, -10);
    frame();
}

/* Window pixel of picture pixel (x, y) */
static void to_screen(int x, int y, int *sx, int *sy)
{
    rect c = app_canvas_rect(A);
    float zs = A->zoom * A->ppp;
    *sx = c.x + (int)((x + 0.5f) * zs);
    *sy = c.y + (int)((y + 0.5f) * zs);
}

static int is_white(int x, int y)
{
    const uint8_t *p = img_at(A->img, x, y);
    return p[0] == 255 && p[1] == 255 && p[2] == 255;
}

static void test_pencil_draws_through_the_canvas(void)
{
    reset(100);
    shot("main");
    CHECK(A->tool == TOOL_PENCIL, "tool %d", A->tool);
    int x0, y0, x1, y1;
    to_screen(100, 100, &x0, &y0);
    to_screen(300, 100, &x1, &y1);
    drag(x0, y0, x1, y1);
    CHECK(!is_white(200, 100), "the pencil drew nothing");
    /* What's drawn is on screen */
    CHECK(win->px[y0 * win->stride + (x0 + x1) / 2] != COLOR(0xFFFFFF), "the stroke isn't shown");
    key('Z', KMOD_CTRL);
    CHECK(is_white(200, 100), "Ctrl+Z didn't undo");
}

static void test_shape_then_move(void)
{
    reset(100);
    app_pick_shape(A, SHAPE_RECTANGLE);
    int x0, y0, x1, y1;
    to_screen(100, 100, &x0, &y0);
    to_screen(300, 250, &x1, &y1);
    drag(x0, y0, x1, y1);
    CHECK(A->live != NULL, "no live shape");
    shot("live-shape");
    /* drag its body */
    int mx, my, tx, ty;
    to_screen(200, 175, &mx, &my);
    to_screen(260, 235, &tx, &ty);
    drag(mx, my, tx, ty);
    CHECK(A->live && fabsf(live_shape_rect(A->live).x0 - 160) < 2, "moved to %f", A->live ? live_shape_rect(A->live).x0 : -1);
}

static void test_selection_menu_and_ants(void)
{
    reset(100);
    app_set_tool(A, TOOL_SELECT);
    int x0, y0, x1, y1;
    to_screen(50, 50, &x0, &y0);
    to_screen(200, 150, &x1, &y1);
    drag(x0, y0, x1, y1);
    CHECK(A->sel != NULL, "no selection");
    click(x0 + 20, y0 + 20, BTN_RIGHT);
    shot("selection-menu");
    /* the menu is an overlay over the canvas now */
    CHECK(!kit_free_at(x0 + 40, y0 + 40), "the right-click menu isn't open");
    key(KEY_ESCAPE, 0);
}

/* The selection menu's Rotate opens beside it on hover, stays open on the way
 * there, closes when another row is hovered, and a pick closes both */
static void test_selection_submenu(void)
{
    reset(100);
    app_set_tool(A, TOOL_SELECT);
    int x0, y0, x1, y1;
    to_screen(50, 50, &x0, &y0);
    to_screen(200, 150, &x1, &y1);
    drag(x0, y0, x1, y1);
    click(x0 + 20, y0 + 20, BTN_RIGHT);
    /* Rotate's row: below the frame and margin, 6 rows and 2 separators (each with its spacing) */
    int mx = x0 + 20, row_y = y0 + 20 + 1 + S(7) + 6 * S(31) + 2 * S(11) + S(11);
    move_to(mx + S(60), row_y);
    move_to(mx + S(62), row_y + 1);
    int beside = mx + S(214) + S(40);
    CHECK(!kit_free_at(beside, row_y), "the Rotate submenu isn't open beside the menu");
    CHECK(kit_free_at(mx + S(60), row_y + S(140)), "the submenu opened below its row");
    shot("selection-submenu");
    /* On its way there over the row below, it stays open */
    send(EV_MOVE, mx + S(150), row_y + S(20), 0);
    frame();
    CHECK(!kit_free_at(beside, row_y), "the submenu closed on the way to it");
    /* Resting on another row closes it, but not the menu */
    clock_now += 0.2;
    move_to(mx + S(60), row_y + S(31));
    clock_now += 0.2;
    kit_repaint();
    frame();
    CHECK(kit_free_at(beside, row_y), "the submenu stayed open over another row");
    CHECK(!kit_free_at(mx + S(60), row_y), "the menu closed");
    /* Open it again and pick Rotate right: both menus close, the selection turns */
    move_to(mx + S(60), row_y);
    move_to(beside, row_y + 2);
    irect before = A->sel->rect;
    click(beside, row_y + 2, BTN_LEFT);
    CHECK(kit_free_at(beside, row_y) && kit_free_at(mx + S(60), row_y), "a pick in the submenu left a menu open");
    CHECK(A->sel && A->sel->rect.x1 - A->sel->rect.x0 == before.y1 - before.y0, "the selection didn't turn");
    /* Escape closes both menus at once, as egui's popups do */
    click(x0 + 20, y0 + 20, BTN_RIGHT);
    move_to(mx + S(60), row_y);
    move_to(mx + S(62), row_y + 1);
    CHECK(!kit_free_at(beside, row_y), "the submenu didn't open again");
    key(KEY_ESCAPE, 0);
    CHECK(kit_free_at(beside, row_y) && kit_free_at(mx + S(60), row_y), "Escape left a menu open");
}

/* The ribbon's drop-down menus have egui's spacing between their rows */
static void test_ribbon_menus(void)
{
    reset(100);
    click(222, 112, BTN_LEFT);
    CHECK(!kit_free_at(230, 125 + 5 * 31 - 10), "the Rotate menu is too short");
    shot("ribbon-rotate-menu");
    click(152, 116, BTN_LEFT);
    CHECK(kit_free_at(375, 200), "opening the Select menu left the Rotate menu open");
    shot("ribbon-select-menu");
    key(KEY_ESCAPE, 0);
}

/* The Together tab's check box and name field have tooltips */
static void test_together_tooltips(void)
{
    reset(100);
    A->tab = TAB_TOGETHER;
    frame();
    /* where the check box's text and the name field are */
    static const struct
    {
        int x, y;
        const char *name;
    } at[2] = { { 165, 80, "together-tip-let-in" }, { 455, 72, "together-tip-name" } };
    for (int i = 0; i < 2; i++)
    {
        int x = S(at[i].x), y = S(at[i].y);
        move_to(x, y);
        clock_now += 0.6;
        kit_repaint();
        frame();
        shot(at[i].name);
    }
}

/* Toasts are drawn over menus and dialogs, and leave them their input */
static void test_toasts_on_top(void)
{
    reset(100);
    snprintf(A->toasts[0].text, sizeof A->toasts[0].text, "Happy Otter joined");
    A->toasts[0].until = os_now() + 30;
    A->n_toasts = 1;
    app_set_tool(A, TOOL_SELECT);
    frame();
    rect v = V.view;
    int tx = v.x + v.w / 2, ty = v.y + v.h - S(18) - S(14);
    int dark = 0;
    for (int dx = -S(60); dx <= S(60); dx++)
        dark |= (win->px[ty * win->w + tx + dx] & 0xFF) < 0x60;
    CHECK(dark, "no toast");
    /* A right-click menu over the toast: the toast still shows, the menu takes the clicks */
    A->menu_open = 1;
    send(EV_MOVE, tx - S(100), ty - S(250), 0);
    send(EV_UP, tx - S(100), ty - S(250), BTN_RIGHT);
    frame();
    frame();
    CHECK(!kit_free_at(tx - S(90), ty), "the menu doesn't cover the toast");
    dark = 0;
    for (int dx = -S(60); dx <= S(60); dx++)
        dark |= (win->px[ty * win->w + tx + dx] & 0xFF) < 0x60;
    CHECK(dark, "the toast is under the menu");
    shot("toast-over-menu");
    key(KEY_ESCAPE, 0);
    A->about_open = 1;
    frame();
    frame();
    shot("toast-over-dialog");
    A->about_open = 0;
    A->n_toasts = 0;
}

static void test_text(void)
{
    reset(100);
    app_set_tool(A, TOOL_TEXT);
    frame();
    CHECK(A->tab == TAB_TEXT || A->tab == TAB_HOME, "tab %d", A->tab);
    int x, y;
    to_screen(80, 80, &x, &y);
    drag(x, y, x + 200, y + 60);
    type("Hello");
    CHECK(A->text && A->text->text && !strcmp(A->text->text, "Hello"), "typed '%s'", A->text && A->text->text ? A->text->text : "(none)");
    clock_now += 0.01;
    frame();
    shot("text");
}

static void test_file_menu(void)
{
    reset(100);
    click(V.file_tab.x + 10, V.file_tab.y + 10, BTN_LEFT);
    CHECK(A->file_menu_open, "the File menu didn't open");
    shot("file-menu");
    key(KEY_ESCAPE, 0);
    CHECK(!A->file_menu_open, "Escape didn't close the File menu");
}

static void test_dialogs_show(void)
{
    static const char *const names[] = { "resize", "properties", "about", "edit-colors", "error", "join", "confirm", "ask-path" };
    for (int i = 0; i < 8; i++)
    {
        reset(i == 3 ? 150 : 100);
        switch (i)
        {
        case 0:
            app_open_resize(A);
            break;
        case 1:
            app_open_properties(A);
            break;
        case 2:
            A->about_open = 1;
            break;
        case 3:
            app_open_edit_colors(A);
            break;
        case 4:
            A->error = strdup("Paint can't read this file.");
            break;
        case 5:
            app_open_join_dialog(A);
            break;
        case 6:
            A->confirm.kind = PENDING_NEW;
            break;
        case 7:
            A->ask_kind = ASK_OPEN;
            break;
        }
        frame();
        frame();
        char name[32];
        snprintf(name, sizeof name, "dialog-%s", names[i]);
        shot(name);
        /* a modal dialog covers the canvas */
        rect v = V.view;
        CHECK(!kit_free_at(v.x + 5, v.y + 5), "%s: the dialog isn't modal", names[i]);
        key(KEY_ESCAPE, 0);
        frame();
        int open = A->resize || A->properties || A->about_open || A->edit_colors || A->error || A->join_dialog ||
                   A->confirm.kind != PENDING_NONE || A->ask_kind != ASK_NONE;
        CHECK(!open, "%s: Escape didn't close it", names[i]);
    }
}

static void test_click_outside_dismisses(void)
{
    reset(100);
    A->about_open = 1;
    frame();
    frame();
    click(30, win->h - 60, BTN_LEFT);
    CHECK(!A->about_open, "a click outside didn't close About");
    /* the press and release in one frame, as a quick click arrives */
    app_open_edit_colors(A);
    frame();
    frame();
    send(EV_MOVE, 30, win->h - 60, 0);
    send(EV_DOWN, 30, win->h - 60, BTN_LEFT);
    send(EV_UP, 30, win->h - 60, BTN_LEFT);
    frame();
    frame();
    CHECK(!A->edit_colors, "a quick click outside didn't close Edit Colors");
}

static void test_view_tab_rulers_and_zoom(void)
{
    reset(100);
    A->tab = TAB_VIEW;
    A->show_rulers = 1;
    A->show_grid = 1;
    app_zoom_at(A, 4, NULL);
    frame();
    frame();
    int x, y;
    to_screen(570, 320, &x, &y);
    move_to(x, y);
    CHECK(A->has_cursor_px && A->cursor_px_x == 570 && A->cursor_px_y == 320, "pointer at %d %d", A->cursor_px_x, A->cursor_px_y);
    shot("view-rulers-grid");
    A->show_thumbnail = 1;
    app_zoom_at(A, 1, NULL);
    frame();
    frame();
    shot("thumbnail");
    /* A tall picture that fits across gets the vertical scroll bar only */
    A->show_rulers = A->show_thumbnail = 0;
    frame();
    rect fits = A->view;
    app_rotate(A, ROTATE_RIGHT);
    frame();
    CHECK(A->view.h == fits.h && A->view.w < fits.w, "view %dx%d, was %dx%d", A->view.w, A->view.h, fits.w, fits.h);
}

static kid make_kid(peer_id id, const char *name, uint8_t r, uint8_t g, uint8_t b)
{
    kid k = { id, strdup(name), { r, g, b }, NULL };
    return k;
}

/* A room of three (me hosting) with friends' pointers and one at the door */
static void test_together_scene(void)
{
    reset(100);
    kid_list kids = { 0 };
    kid_list_push(&kids, make_kid(1, "Happy Otter", 0xF3, 0x9C, 0x12));
    kid_list_push(&kids, make_kid(2, "Brave Tiger", 0x8E, 0x44, 0xAD));
    kid_list_push(&kids, make_kid(3, "Sunny Koala", 0x27, 0xAE, 0x60));
    A->session = session_new(3, 3, 1, img_dup(A->img), kids);
    session *s = A->session;
    kid_list_push(&s->waiting, make_kid(4, "Clever Fox", 0xE7, 0x4C, 0x3C));
    s->pointers = calloc(2, sizeof *s->pointers);
    s->n_pointers = 2;
    s->pointers[0] = (pointer){ 1, { 1, 300, 200, HELD_PENCIL, { 0, 0, 0, 255 }, 1 }, A->now };
    s->pointers[1] = (pointer){ 2, { 1, 600, 300, HELD_OTHER, { 0, 0, 0, 255 }, 1 }, A->now };
    A->tab = TAB_TOGETHER;
    frame();
    frame();
    shot("together-room");
    CHECK(app_hosting(A), "not hosting");
    /* The door notice is an overlay at the bottom of the picture */
    rect v = V.view;
    CHECK(!kit_free_at(v.x + v.w / 2, v.y + v.h - S(30)), "no door notice");
}

static void test_tabs(void)
{
    reset(100);
    A->tab = TAB_TOGETHER;
    frame();
    shot("together");
    reset(200);
    shot("main-200");
}

int main(int argc, char **argv)
{
    out_dir = argc > 1 ? argv[1] : NULL;
    os_init();
    if (font_init() != 0)
    {
        printf("no UI font here: skipped\n");
        return 0;
    }
    double t0 = os_now();
    test_pencil_draws_through_the_canvas();
    test_shape_then_move();
    test_selection_menu_and_ants();
    test_selection_submenu();
    test_ribbon_menus();
    test_together_tooltips();
    test_toasts_on_top();
    test_text();
    test_file_menu();
    test_dialogs_show();
    test_click_outside_dismisses();
    test_view_tab_rulers_and_zoom();
    test_together_scene();
    test_tabs();
    reset(100);
    t0 = os_now();
    for (int i = 0; i < 20; i++)
    {
        kit_repaint();
        frame();
    }
    printf("a frame of the whole window: %.2f ms\n", (os_now() - t0) * 1000 / 20);
    app_free(A);
    view_free();
    surface_free(win);
    printf(failures ? "test_views: %d FAILED\n" : "test_views: all passed\n", failures);
    return failures != 0;
}
