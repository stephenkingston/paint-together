/*
 * Paint (C port) - tests of the tools, driven through the app's public API the
 * way mouse and keyboard input drive it (Rust: app.rs's mod tests, minus `together`)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/app.h"
#include "host/headless.h"
#include "paint/raster.h"
#include "paint/transform.h"
#include "ui/icons.h"

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

static const rgba WHITE = { 255, 255, 255, 255 }, BLACK = { 0, 0, 0, 255 };
static const rgba RED = { 237, 28, 36, 255 }, BLUE = { 0, 0, 255, 255 };

static app *mk(void)
{
    app *a = app_new();
    img_free(a->img);
    a->img = img_new(200, 120, WHITE);
    a->view = (rect){ 0, 0, 800, 600 };
    return a;
}

static void drag(app *a, int button, const float *xy, int n)
{
    pt pts[16];
    for (int i = 0; i < n; i++)
        pts[i] = pt_make(xy[2 * i], xy[2 * i + 1]);
    app_begin(a, pts[0], button, pt_make(0, 0));
    app_update(a, pts + 1, n - 1, 0, 1.0f / 60.0f);
    app_end(a);
}
#define DRAG(a, button, ...)                                                           \
    do                                                                                 \
    {                                                                                  \
        const float xy_[] = { __VA_ARGS__ };                                           \
        drag(a, button, xy_, (int)(sizeof xy_ / sizeof *xy_ / 2));                     \
    } while (0)

static rgba px(const app *a, int x, int y)
{
    rgba c;
    memcpy(&c, img_at(a->img, x, y), 4);
    return c;
}

static int same(const img *x, const img *y)
{
    return x->w == y->w && x->h == y->h && !memcmp(x->px, y->px, (size_t)x->w * x->h * 4);
}

static int is_white(const img *im)
{
    for (size_t i = 0; i < (size_t)im->w * im->h * 4; i++)
        if (im->px[i] != 255)
            return 0;
    return 1;
}

#define EQ(c, want) rgba_eq(c, want)

static void test_pencil_undo_redo(void)
{
    app *a = mk();
    DRAG(a, BTN_LEFT, 10, 10, 60, 10);
    CHECK(EQ(px(a, 35, 10), BLACK), "drawn");
    CHECK(EQ(px(a, 35, 12), WHITE), "only the line");
    CHECK(app_is_dirty(a), "dirty");
    app_undo(a);
    CHECK(EQ(px(a, 35, 10), WHITE), "undone");
    CHECK(!app_is_dirty(a), "clean again");
    app_redo(a);
    CHECK(EQ(px(a, 35, 10), BLACK), "redone");
    app_free(a);
}

static void test_right_button(void)
{
    app *a = mk();
    a->secondary = RED;
    DRAG(a, BTN_RIGHT, 10, 20, 60, 20);
    CHECK(EQ(px(a, 30, 20), RED), "Color 2");
    app_free(a);
}

static void test_every_brush(void)
{
    for (int k = 0; k < BRUSH_COUNT; k++)
    {
        app *a = mk();
        img *original = img_dup(a->img);
        a->tool = TOOL_BRUSH;
        a->brush = (brush_kind)k;
        a->size_brush = 14.0f;
        DRAG(a, BTN_LEFT, 20, 60, 90, 60, 160, 70);
        CHECK(!same(a->img, original), "%s drew nothing", brush_name(a->brush));
        app_undo(a);
        CHECK(same(a->img, original), "%s undo", brush_name(a->brush));
        img_free(original);
        app_free(a);
    }
}

static void test_eraser(void)
{
    app *a = mk();
    raster_fill_rect(a->img, irect_of(a->img), BLUE);
    a->tool = TOOL_ERASER;
    a->size_eraser = 10.0f;
    DRAG(a, BTN_LEFT, 50, 50, 80, 50);
    CHECK(EQ(px(a, 65, 50), WHITE), "erased to Color 2");
    CHECK(EQ(px(a, 65, 70), BLUE), "only under it");
    app_free(a);
}

static void test_shapes(void)
{
    app *a = mk();
    a->tool = TOOL_SHAPE;
    a->shape = SHAPE_RECTANGLE;
    a->shape_fill = 1;
    a->secondary = RED;
    a->size_shape = 2.0f;
    DRAG(a, BTN_LEFT, 20, 20, 60, 50, 100, 80);
    CHECK(EQ(px(a, 20, 50), BLACK), "left edge");
    CHECK(EQ(px(a, 60, 50), RED), "interior");
    CHECK(EQ(px(a, 140, 50), WHITE), "outside");
    CHECK(EQ(px(a, 60, 35), RED), "the intermediate drag position left nothing behind");
    CHECK(a->live != NULL, "still editable until committed");
    CHECK(history_state_id(a->history) == 0, "no step yet");
    app_commit_all(a);
    CHECK(a->live == NULL, "committed");
    CHECK(history_state_id(a->history) == 1, "one step");
    CHECK(EQ(px(a, 60, 50), RED), "stays");
    app_free(a);
}

static void rect_shape(app *a)
{
    a->tool = TOOL_SHAPE;
    a->shape = SHAPE_RECTANGLE;
    a->size_shape = 2.0f;
    DRAG(a, BTN_LEFT, 20, 20, 60, 50);
    app_render_live_shape(a);
}

static void test_pick_shape(void)
{
    app *a = mk();
    rect_shape(a);
    app_pick_shape(a, SHAPE_OVAL);
    CHECK(a->live == NULL, "the rectangle is stamped");
    CHECK(history_state_id(a->history) == 1, "one step");
    CHECK(EQ(px(a, 20, 20), BLACK), "still a rectangle: its corner is drawn");
    DRAG(a, BTN_LEFT, 100, 100, 160, 140);
    CHECK(a->live && a->live->kind == SHAPE_OVAL, "an oval now");
    CHECK(EQ(px(a, 20, 20), BLACK), "the rectangle stays");
    app_free(a);
}

static void test_live_shape_moves(void)
{
    app *a = mk();
    rect_shape(a);
    CHECK(EQ(px(a, 20, 35), BLACK), "drawn");
    DRAG(a, BTN_LEFT, 40, 35, 90, 65);
    app_render_live_shape(a);
    CHECK(EQ(px(a, 20, 35), WHITE), "old position cleared");
    CHECK(EQ(px(a, 70, 65), BLACK), "drawn at the new position");
    app_commit_all(a);
    app_undo(a);
    CHECK(is_white(a->img), "one undo step removes it");
    app_free(a);
}

static void test_live_shape_resizes(void)
{
    app *a = mk();
    rect_shape(a);
    DRAG(a, BTN_LEFT, 60, 50, 100, 90);     /* the bottom-right handle */
    app_render_live_shape(a);
    CHECK(EQ(px(a, 100, 70), BLACK), "right edge moved");
    CHECK(EQ(px(a, 60, 50), WHITE), "old corner cleared");
    a->primary = RED;                       /* restyled from the ribbon */
    a->size_shape = 6.0f;
    app_render_live_shape(a);
    CHECK(EQ(px(a, 102, 70), RED), "thicker and red");
    app_set_tool(a, TOOL_PENCIL);
    CHECK(a->live == NULL, "another tool stamps it");
    CHECK(history_state_id(a->history) == 1, "one step");
    app_free(a);
}

static void test_delete_live_shape(void)
{
    app *a = mk();
    rect_shape(a);
    app_key(a, KEY_DELETE, 0);
    CHECK(a->live == NULL, "deleted");
    CHECK(is_white(a->img), "and gone from the picture");
    rect_shape(a);
    DRAG(a, BTN_LEFT, 120, 80, 180, 110);   /* a new shape elsewhere commits the first */
    app_render_live_shape(a);
    CHECK(history_state_id(a->history) == 1, "one step");
    CHECK(EQ(px(a, 20, 35), BLACK), "the first stays");
    CHECK(a->live != NULL, "the second is live");
    app_free(a);
}

static void test_click_draws_no_shape(void)
{
    app *a = mk();
    a->tool = TOOL_SHAPE;
    DRAG(a, BTN_LEFT, 20, 20);
    CHECK(is_white(a->img), "nothing drawn");
    CHECK(!history_can_undo(a->history), "nothing to undo");
    app_free(a);
}

static void test_fill_inside_outline(void)
{
    app *a = mk();
    a->tool = TOOL_SHAPE;
    a->shape = SHAPE_RECTANGLE;
    DRAG(a, BTN_LEFT, 20, 20, 80, 80);
    a->tool = TOOL_FILL;
    a->primary = RED;
    DRAG(a, BTN_LEFT, 50, 50);
    CHECK(EQ(px(a, 50, 50), RED), "filled");
    CHECK(EQ(px(a, 120, 50), WHITE), "not outside");
    app_free(a);
}

static void test_fill_no_pale_ring(void)
{
    app *a = mk();
    a->tool = TOOL_SHAPE;
    a->shape = SHAPE_OVAL;
    a->size_shape = 2.0f;
    DRAG(a, BTN_LEFT, 20, 20, 120, 100);
    app_commit_all(a);
    a->tool = TOOL_FILL;
    a->primary = RED;
    DRAG(a, BTN_LEFT, 70, 60);
    float cx = 70, cy = 60, rx = 50, ry = 40;
    int pale = 0, leaked = 0;
    for (int y = 20; y < 100; y++)
        for (int x = 20; x < 120; x++)
        {
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            if (dx * dx + dy * dy < 1.0f && px(a, x, y).g > 60)
                pale++;
        }
    for (int y = 0; y < 120; y++)
        for (int x = 0; x < 200; x++)
        {
            float dx = (x + 0.5f - cx) / (rx + 3.0f), dy = (y + 0.5f - cy) / (ry + 3.0f);
            if (dx * dx + dy * dy > 1.0f && !EQ(px(a, x, y), WHITE))
                leaked++;
        }
    CHECK(pale == 0, "%d pale pixels between the fill and the outline", pale);
    CHECK(leaked == 0, "%d pixels: fill escaped the outline", leaked);
    app_free(a);
}

static void test_picker(void)
{
    app *a = mk();
    memcpy(img_at(a->img, 5, 5), &RED, 4);
    app_set_tool(a, TOOL_BRUSH);
    app_set_tool(a, TOOL_PICKER);
    DRAG(a, BTN_RIGHT, 5.5f, 5.5f);
    CHECK(EQ(a->secondary, RED), "Color 2 picked");
    CHECK(a->tool == TOOL_BRUSH, "back to the brush");
    app_free(a);
}

static void test_move_selection(void)
{
    app *a = mk();
    raster_fill_rect(a->img, irect_make(10, 10, 30, 30), BLUE);
    img *original = img_dup(a->img);
    a->tool = TOOL_SELECT;
    DRAG(a, BTN_LEFT, 10, 10, 30, 30);
    CHECK(a->sel && a->sel->rect.x0 == 10 && a->sel->rect.y0 == 10 && a->sel->rect.x1 == 30 && a->sel->rect.y1 == 30, "selected");
    DRAG(a, BTN_LEFT, 15, 15, 65, 15);
    CHECK(EQ(px(a, 70, 20), BLUE), "moved pixels");
    CHECK(EQ(px(a, 20, 20), WHITE), "hole filled with Color 2");
    app_commit_selection(a);
    CHECK(history_state_id(a->history) == 1, "one step");
    app_undo(a);
    CHECK(same(a->img, original), "undone");
    img_free(original);
    app_free(a);
}

static void test_free_form_delete(void)
{
    app *a = mk();
    raster_fill_rect(a->img, irect_of(a->img), BLUE);
    a->tool = TOOL_FREE_SELECT;
    DRAG(a, BTN_LEFT, 20, 20, 80, 20, 20, 80, 20, 20);
    CHECK(a->sel && a->sel->lasso, "a free-form selection");
    app_delete_selection(a);
    CHECK(EQ(px(a, 30, 30), WHITE), "inside the triangle");
    CHECK(EQ(px(a, 75, 75), BLUE), "inside the box but outside the triangle");
    app_free(a);
}

static void test_transparent_selection(void)
{
    app *a = mk();
    raster_fill_rect(a->img, irect_make(10, 10, 20, 20), BLUE);
    raster_fill_rect(a->img, irect_make(60, 0, 120, 120), RED);
    a->transparent_selection = 1;
    a->tool = TOOL_SELECT;
    DRAG(a, BTN_LEFT, 5, 5, 25, 25);
    DRAG(a, BTN_LEFT, 15, 15, 75, 15);
    CHECK(EQ(px(a, 75, 15), BLUE), "moved");
    CHECK(EQ(px(a, 66, 6), RED), "white margin of the selection is see-through");
    app_toggle_transparent_selection(a);
    CHECK(EQ(px(a, 66, 6), WHITE), "opaque again once toggled off");
    app_free(a);
}

static void test_rotate_selection(void)
{
    app *a = mk();
    raster_fill_rect(a->img, irect_make(10, 10, 50, 20), BLUE);
    memcpy(img_at(a->img, 150, 100), &RED, 4);
    a->tool = TOOL_SELECT;
    DRAG(a, BTN_LEFT, 10, 10, 50, 20);
    app_rotate(a, ROTATE_RIGHT);
    CHECK(a->sel && irect_w(a->sel->rect) == 10 && irect_h(a->sel->rect) == 40, "turned");
    CHECK(EQ(px(a, 15, 45), BLUE), "rotated pixels");
    CHECK(EQ(px(a, 45, 15), WHITE), "hole");
    CHECK(EQ(px(a, 150, 100), RED), "the rest untouched");
    app_free(a);
}

static void test_paste_larger(void)
{
    app *a = mk();
    app_paste_image(a, img_new(300, 50, RED));
    CHECK(a->img->w == 300 && a->img->h == 120, "grown to %dx%d", a->img->w, a->img->h);
    CHECK(EQ(px(a, 250, 10), RED), "pasted");
    app_commit_selection(a);
    app_undo(a);
    CHECK(a->img->w == 200 && a->img->h == 120, "shrunk back");
    app_free(a);
}

static void test_text_committed(void)
{
    app *a = mk();
    a->tool = TOOL_TEXT;
    a->text_style.size_pt = 24.0f;
    img *original = img_dup(a->img);
    DRAG(a, BTN_LEFT, 10, 10);
    CHECK(a->text != NULL, "a text box");
    if (a->text)
    {
        free(a->text->text);
        a->text->text = strdup("Hello");
    }
    app_update_text_preview(a);
    CHECK(!same(a->img, original), "rendered");
    app_set_tool(a, TOOL_PENCIL);
    CHECK(a->text == NULL, "committed");
    CHECK(history_can_undo(a->history), "undoable");
    app_undo(a);
    CHECK(same(a->img, original), "undone");
    img_free(original);
    app_free(a);
}

static void test_canvas_resize_and_crop(void)
{
    app *a = mk();
    a->op.kind = OP_RESIZE_CANVAS;
    a->op.button = BTN_LEFT;
    a->op.handle = HANDLE_CORNER;
    a->op.size_w = 200;
    a->op.size_h = 120;
    pt p = pt_make(250.4f, 90.0f);
    app_update(a, &p, 1, 0, 0.0f);
    app_end(a);
    CHECK(a->img->w == 250 && a->img->h == 90, "resized to %dx%d", a->img->w, a->img->h);
    a->tool = TOOL_SELECT;
    DRAG(a, BTN_LEFT, 10, 10, 50, 40);
    app_crop(a);
    CHECK(a->img->w == 40 && a->img->h == 30, "cropped to %dx%d", a->img->w, a->img->h);
    app_undo(a);
    CHECK(a->img->w == 250 && a->img->h == 90, "uncropped");
    app_free(a);
}

static void test_hsl_and_text_tab(void)
{
    static const rgba cs[] = { { 0, 120, 215, 255 }, { 255, 255, 255, 255 }, { 0, 0, 0, 255 }, { 237, 28, 36, 255 }, { 127, 127, 127, 255 } };
    for (int i = 0; i < 5; i++)
    {
        float hsl[3];
        rgb_to_hsl(cs[i], hsl);
        rgba back = hsl_to_rgb(hsl);
        CHECK(EQ(back, cs[i]), "%d %d %d", back.r, back.g, back.b);
    }
    app *a = mk();
    app_set_tool(a, TOOL_TEXT);
    CHECK(a->tab == TAB_TEXT, "the Text tab");
    app_set_tool(a, TOOL_PENCIL);
    CHECK(a->tab == TAB_HOME, "back home");
    app_free(a);
}

static int move_to(app *a, float x, float y)
{
    ui_event ev = { 0 };
    ev.type = EV_MOVE;
    ev.x = (int)x;
    ev.y = (int)y;
    app_canvas_event(a, &ev);
    return a->cursor;
}

static void test_tool_pointers(void)
{
    app *a = mk();
    app_set_view(a, (rect){ 0, 0, 1000, 700 });
    rect cr = app_canvas_rect(a);
    float ox = (float)cr.x + 20, oy = (float)cr.y + 20;
    static const struct
    {
        app_tool tool;
        int icon;
    } tools[] = { { TOOL_PENCIL, ICON_PENCIL }, { TOOL_FILL, ICON_FILL }, { TOOL_BRUSH, ICON_BRUSHES }, { TOOL_PICKER, ICON_PICKER } };
    for (int i = 0; i < 4; i++)
    {
        a->tool = tools[i].tool;
        CHECK(move_to(a, ox, oy) == CURSOR_NONE, "tool %d hides the pointer", tools[i].tool);
        CHECK(a->tool_icon == tools[i].icon && a->tool_x == ox && a->tool_y == oy, "tool %d's icon", tools[i].tool);
    }
    /* The eraser shows just its footprint: no crosshair, no icon */
    a->tool = TOOL_ERASER;
    CHECK(move_to(a, ox, oy) == CURSOR_NONE, "eraser");
    CHECK(a->tool_icon == -1, "no icon");
    a->tool = TOOL_SHAPE;
    CHECK(move_to(a, ox, oy) == CURSOR_CROSS, "shapes: crosshair");
    CHECK(a->tool_icon == -1, "no icon");
    /* Off the canvas the normal pointer comes back */
    a->tool = TOOL_PENCIL;
    CHECK(move_to(a, 990, 690) == CURSOR_ARROW, "arrow");
    CHECK(a->tool_icon == -1, "no icon off the canvas");
    /* Over the resize handle */
    CHECK(move_to(a, (float)(cr.x + cr.w + 4), (float)(cr.y + cr.h + 4)) == CURSOR_SIZE_NWSE, "corner handle");
    app_free(a);
}

static void test_canvas_events(void)
{
    /* A drag in screen coordinates draws, and the canvas handle resizes; middle-drag pans */
    app *a = mk();
    app_set_view(a, (rect){ 0, 0, 1000, 700 });
    rect cr = app_canvas_rect(a);
    ui_event ev = { 0 };
    ev.type = EV_DOWN;
    ev.button = BTN_LEFT;
    ev.x = cr.x + 10;
    ev.y = cr.y + 10;
    app_canvas_event(a, &ev);
    CHECK(a->op.kind == OP_STROKE, "a stroke began");
    ev.type = EV_MOVE;
    ev.x = cr.x + 60;
    app_canvas_event(a, &ev);
    ev.type = EV_UP;
    app_canvas_event(a, &ev);
    CHECK(a->op.kind == OP_IDLE && EQ(px(a, 35, 10), BLACK), "drawn by events");
    ev.type = EV_DOWN;
    ev.x = cr.x + cr.w + 4;
    ev.y = cr.y + cr.h + 4;
    app_canvas_event(a, &ev);
    CHECK(a->op.kind == OP_RESIZE_CANVAS, "grabbed the corner");
    ev.type = EV_UP;
    ev.x += 30;
    ev.y += 10;
    app_canvas_event(a, &ev);
    CHECK(a->img->w == 234 && a->img->h == 134, "resized to %dx%d", a->img->w, a->img->h);
    /* Right-click with the selection tool asks for its menu on the release */
    a->tool = TOOL_SELECT;
    ev.type = EV_DOWN;
    ev.button = BTN_RIGHT;
    ev.x = cr.x + 5;
    ev.y = cr.y + 5;
    app_canvas_event(a, &ev);
    ev.type = EV_UP;
    app_canvas_event(a, &ev);
    CHECK(a->menu_open && a->menu_x == cr.x + 5, "the selection menu");
    /* Ctrl+wheel zooms about the pointer */
    ev.type = EV_WHEEL;
    ev.mods = KMOD_CTRL;
    ev.wheel_y = 1.0f;
    app_canvas_event(a, &ev);
    CHECK(a->zoom > 1.0f, "zoomed in to %g", a->zoom);
    app_free(a);
}

static void test_gridlines(void)
{
    CHECK(app_grid_step(1.0f) == 8, "1");
    CHECK(app_grid_step(0.5f) == 16, "0.5");
    CHECK(app_grid_step(2.0f) == 8, "2");
    CHECK(app_grid_step(3.0f) == 8, "3");
    CHECK(app_grid_step(4.0f) == 1, "4");
    CHECK(app_grid_step(8.0f) == 1, "8");
    CHECK(app_grid_step(0.125f) == 64, "0.125");
}

static const char *typed(const app *a)
{
    return a->text ? a->text->text : "";
}

static void test_typing(void)
{
    app *a = mk();
    a->tool = TOOL_TEXT;
    DRAG(a, BTN_LEFT, 40, 30);
    img *blank = img_dup(a->img);
    app_text(a, "Helo");
    CHECK(!same(a->img, blank), "rendered on the canvas as you type");
    app_tick(a, 0);
    app_key(a, KEY_LEFT, 0);
    app_text(a, "l");
    CHECK(!strcmp(typed(a), "Hello"), "%s", typed(a));
    app_key(a, KEY_END, 0);
    app_key(a, KEY_ENTER, 0);
    app_text(a, "World");
    CHECK(!strcmp(typed(a), "Hello\nWorld"), "%s", typed(a));
    /* Option+Shift+Left (Ctrl+Shift+Left off the Mac) selects the last word; typing replaces it; undo brings it back */
#ifdef __APPLE__
    app_key(a, KEY_LEFT, KMOD_ALT | KMOD_SHIFT);
#else
    app_key(a, KEY_LEFT, KMOD_CTRL | KMOD_SHIFT);
#endif
    app_text(a, "Paint");
    CHECK(!strcmp(typed(a), "Hello\nPaint"), "%s", typed(a));
    app_key(a, 'Z', KMOD_CTRL);
    CHECK(!strcmp(typed(a), "Hello\nWorld"), "%s", typed(a));
    /* Undo put the caret back where that edit began; End, then Up keeps the column */
    app_key(a, KEY_END, 0);
    app_key(a, KEY_UP, 0);
    app_key(a, KEY_BACKSPACE, 0);
    CHECK(!strcmp(typed(a), "Hell\nWorld"), "%s", typed(a));
    /* An input method can report a keystroke twice; it is typed once */
    app_key(a, 'E', 0);     /* the key that types it (not a shortcut) */
    app_text(a, "\xC3\xA9");
    app_ime_commit(a, "\xC3\xA9");
    CHECK(!strcmp(typed(a), "Hell\xC3\xA9\nWorld"), "%s", typed(a));
    /* Pasting text inserts it at the caret */
    host_clip_set_text("!");
    app_key(a, 'V', KMOD_CTRL);
    CHECK(!strcmp(typed(a), "Hell\xC3\xA9!\nWorld"), "%s", typed(a));
    /* Esc stops typing; the arrows then move the text instead */
    app_key(a, KEY_ESCAPE, 0);
    CHECK(a->text && !a->text->editing, "not typing");
    pt before = a->text->pos;
    app_key(a, KEY_RIGHT, 0);
    CHECK(a->text->pos.x == before.x + 1 && a->text->pos.y == before.y, "moved");
    CHECK(!strcmp(typed(a), "Hell\xC3\xA9!\nWorld"), "%s", typed(a));
    img_free(blank);
    app_free(a);
}

static void test_click_places_caret(void)
{
    app *a = mk();
    a->tool = TOOL_TEXT;
    DRAG(a, BTN_LEFT, 20, 20);
    app_text(a, "Hello");
    app_key(a, KEY_ESCAPE, 0);
    text_box *tb = a->text;
    pt c = caret_pos(&tb->layout, 2);
    DRAG(a, BTN_LEFT, tb->pos.x + c.x + 0.4f, tb->pos.y + c.y + tb->layout.line_h * 0.5f);
    CHECK(a->text && a->text->editing, "typing again");
    CHECK(a->text && a->text->cursor.cursor == 2, "caret at %d", a->text ? a->text->cursor.cursor : -1);
    app_text(a, "_");
    CHECK(!strcmp(typed(a), "He_llo"), "%s", typed(a));
    app_free(a);
}

static int ink(const app *a, frect r)
{
    for (int y = (int)r.y0; y < (int)r.y1; y++)
        for (int x = (int)r.x0; x < (int)r.x1; x++)
            if (!EQ(px(a, x, y), WHITE))
                return 1;
    return 0;
}

static void test_move_and_scale_text(void)
{
    app *a = mk();
    a->tool = TOOL_TEXT;
    a->text_style.size_pt = 20.0f;
    DRAG(a, BTN_LEFT, 10, 10);
    free(a->text->text);
    a->text->text = strdup("Hi");
    app_update_text_preview(a);
    frect old = a->text->bounds;
    DRAG(a, BTN_LEFT, 15, 15, 65, 45);      /* by its body */
    CHECK(a->text->pos.x == 60 && a->text->pos.y == 40, "at %g %g", a->text->pos.x, a->text->pos.y);
    CHECK(!ink(a, old), "old spot cleared");
    CHECK(ink(a, a->text->bounds), "drawn at the new spot");
    a->text->editing = 0;
    DRAG(a, BTN_LEFT, 70, 50);              /* a click without a drag reopens the editor */
    CHECK(a->text->editing, "reopened");
    frect b = a->text->bounds;
    float bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    DRAG(a, BTN_LEFT, b.x1 + 3, b.y1 + 3, b.x0 + 2 * bw, b.y0 + 2 * bh);
    CHECK(fabsf(a->text_style.size_pt - 40.0f) <= 1.0f, "size %g", a->text_style.size_pt);
    CHECK(a->text->pos.x == b.x0 && a->text->pos.y == b.y0, "scaled about the opposite corner");
    app_commit_all(a);
    CHECK(a->text == NULL, "committed");
    CHECK(history_state_id(a->history) == 1, "one step");
    app_free(a);
}

static void test_zoom_anchor(void)
{
    app *a = mk();
    a->scroll_x = 100;
    a->scroll_y = 50;
    pt anchor = pt_make(300, 200);
    float bx = (a->scroll_x + anchor.x - APP_MARGIN) / a->zoom, by = (a->scroll_y + anchor.y - APP_MARGIN) / a->zoom;
    app_zoom_at(a, 4.0f, &anchor);
    float ax = (a->scroll_x + anchor.x - APP_MARGIN) / a->zoom, ay = (a->scroll_y + anchor.y - APP_MARGIN) / a->zoom;
    CHECK(hypotf(ax - bx, ay - by) < 1e-3f, "%g %g vs %g %g", bx, by, ax, ay);
    app_free(a);
}

/* ---- files, the prompt and keys (the C port's own) ---- */

static void test_files(void)
{
    app *a = mk();
    char path[300];
    snprintf(path, sizeof path, "%s/test-picture.png", os_config_dir());
    DRAG(a, BTN_LEFT, 10, 10, 60, 10);
    headless_queue_pick(path);
    CHECK(app_save(a), "saved");
    CHECK(!app_is_dirty(a) && a->path && a->has_meta && a->n_recent >= 1, "clean, named, recent");
    char title[64];
    snprintf(title, sizeof title, "test-picture.png - Paint");
    CHECK(!strcmp(app_title(a), title), "%s", app_title(a));
    DRAG(a, BTN_LEFT, 10, 30, 60, 30);
    CHECK(app_title(a)[0] == '*', "dirty title");
    /* New asks first; Don't Save goes ahead */
    app_request(a, PENDING_NEW, NULL);
    CHECK(a->confirm.kind == PENDING_NEW, "asked");
    CHECK(app_key(a, 'Z', KMOD_CTRL) == 0, "keys wait for the dialog");
    app_confirm_answer(a, 1);
    CHECK(a->img->w == APP_DEFAULT_W && !a->path, "a new picture");
    /* Open with no picker: the app asks for a path */
    app_request(a, PENDING_OPEN, NULL);
    CHECK(a->ask_kind == ASK_OPEN, "asks for a path");
    app_ask_path_ok(a, path);
    CHECK(a->img->w == 200 && EQ(px(a, 35, 10), BLACK) && EQ(px(a, 35, 30), WHITE), "opened what was saved");
    app_request(a, PENDING_OPEN, "/no/such/file.png");
    CHECK(a->error != NULL, "an error for a missing file");
    app_dismiss_error(a);
    /* Quit while dirty asks; Cancel stays */
    DRAG(a, BTN_LEFT, 10, 50, 60, 50);
    CHECK(app_close_requested(a) == 0 && a->confirm.kind == PENDING_QUIT, "asks before closing");
    app_confirm_answer(a, 2);
    CHECK(!a->quit && !a->confirm.kind, "stays");
    remove(path);
    app_free(a);
}

static void test_keys(void)
{
    app *a = mk();
    app_key(a, 'A', KMOD_CTRL);
    CHECK(a->sel && a->tool == TOOL_SELECT, "select all");
    app_key(a, KEY_DELETE, 0);
    CHECK(!a->sel, "deleted");
    app_key(a, 'G', KMOD_CTRL);
    CHECK(a->show_grid, "grid");
    app_key(a, '=', KMOD_CTRL);
    CHECK(a->zoom == 1.5f, "zoom in %g", a->zoom);
    app_key(a, '1', KMOD_CTRL);
    CHECK(a->zoom == 1.0f, "100%%");
    app_key(a, 'W', KMOD_CTRL);
    CHECK(a->resize && a->resize->base_w == 200, "resize dialog");
    app_resize_set_w(a, 50.0f);
    CHECK(a->resize->h == 50.0f, "keeps the aspect");
    app_resize_ok(a);
    CHECK(a->img->w == 100 && a->img->h == 60, "resized to %dx%d", a->img->w, a->img->h);
    char buf[32];
#ifdef __APPLE__
    CHECK(!strcmp(app_shortcut(KMOD_CTRL | KMOD_SHIFT, 'Z', buf, sizeof buf), "\xE2\x87\xA7\xE2\x8C\x98Z"), "%s", buf);
#else
    CHECK(!strcmp(app_shortcut(KMOD_CTRL | KMOD_SHIFT, 'Z', buf, sizeof buf), "Ctrl+Shift+Z"), "%s", buf);
#endif
    app_copy(a);
    app_paste(a);
    CHECK(a->sel && a->sel->fl, "pasted");
    app_free(a);
}

int main(void)
{
#ifndef _WIN32
    setenv("XDG_CONFIG_HOME", "/tmp/paint-c-test-app", 1);
#endif
    os_init();
    test_pencil_undo_redo();
    test_right_button();
    test_every_brush();
    test_eraser();
    test_shapes();
    test_pick_shape();
    test_live_shape_moves();
    test_live_shape_resizes();
    test_delete_live_shape();
    test_click_draws_no_shape();
    test_fill_inside_outline();
    test_fill_no_pale_ring();
    test_picker();
    test_move_selection();
    test_free_form_delete();
    test_transparent_selection();
    test_rotate_selection();
    test_paste_larger();
    test_text_committed();
    test_canvas_resize_and_crop();
    test_hsl_and_text_tab();
    test_tool_pointers();
    test_canvas_events();
    test_gridlines();
    test_typing();
    test_click_places_caret();
    test_move_and_scale_text();
    test_zoom_anchor();
    test_files();
    test_keys();
    if (failures)
    {
        printf("test_app: %d failed\n", failures);
        return 1;
    }
    printf("test_app: ok\n");
    return 0;
}
