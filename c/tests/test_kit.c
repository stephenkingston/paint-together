/*
 * Paint (C port) - tests of the drawing and widget kit: rectangles, the
 * anti-aliased rasteriser's coverage, the canvas view, text measuring, icons,
 * and the kit's layout and input handling driven by synthetic events.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os/os.h"
#include "ui/font.h"
#include "ui/icons.h"
#include "ui/kit.h"
#include "ui/ribbon.h"
#include "ui/theme.h"

static int failed, checks;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        checks++;                                                       \
        if (!(cond))                                                    \
        {                                                               \
            failed++;                                                   \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static int near(double a, double b, double tol)
{
    return fabs(a - b) <= tol;
}

/* ---- rectangles ---- */

static void test_rects(void)
{
    rect a = rect_make(0, 0, 10, 10), b = rect_make(5, 5, 10, 10);
    rect c = rect_cut(a, b);
    CHECK(c.x == 5 && c.y == 5 && c.w == 5 && c.h == 5);
    CHECK(rect_empty(rect_cut(a, rect_make(20, 20, 5, 5))));
    rect u = rect_union(a, b);
    CHECK(u.x == 0 && u.y == 0 && u.w == 15 && u.h == 15);
    CHECK(rect_union(rect_make(3, 3, 0, 0), a).w == 10);
    rect i = rect_inset(a, 2);
    CHECK(i.x == 2 && i.w == 6);
    CHECK(rect_inset(a, 8).w == 0);
    CHECK(rect_has(a, 0, 0) && rect_has(a, 9, 9) && !rect_has(a, 10, 9));
}

/* ---- the rasteriser ---- */

/* White drawn on black: the red channel is the coverage */
static double coverage_sum(surface *s)
{
    double sum = 0;
    for (int i = 0; i < s->w * s->h; i++)
        sum += C_R(s->px[i]) / 255.0;
    return sum;
}

static surface *black(int w, int h)
{
    surface *s = surface_new(w, h);
    gfx_fill(s, rect_make(0, 0, w, h), COLOR(0));
    return s;
}

static void test_coverage(void)
{
    surface *s = black(64, 64);
    float quad[8] = { 10.25f, 10.5f, 30.75f, 10.5f, 30.75f, 20.25f, 10.25f, 20.25f };
    gfx_polygon(s, quad, 4, COLOR(0xFFFFFF));
    CHECK(near(coverage_sum(s), 20.5 * 9.75, 0.5));
    CHECK(C_R(s->px[15 * 64 + 20]) == 255);
    CHECK(C_R(s->px[5 * 64 + 5]) == 0);

    /* A circle: its area, and an even edge */
    s = black(64, 64);
    gfx_circle(s, 32, 32, 20, COLOR(0xFFFFFF));
    CHECK(near(coverage_sum(s), 3.14159265 * 400, 3));

    /* A ring is hollow; its area is the difference */
    surface *r = black(64, 64);
    gfx_ring(r, 32, 32, 20, 4, COLOR(0xFFFFFF));
    CHECK(C_R(r->px[32 * 64 + 32]) == 0);
    CHECK(near(coverage_sum(r), 3.14159265 * (22 * 22 - 18 * 18), 4));

    /* A polyline's joints aren't counted twice: an L of two 10 x 2 strokes with a mitered corner */
    surface *l = black(64, 64);
    float el[6] = { 10, 11, 30, 11, 30, 31 };
    gfx_stroke(l, el, 3, 2, COLOR(0xFFFFFF), GFX_MITER | GFX_BUTT);
    CHECK(near(coverage_sum(l), 20 * 2 + 20 * 2, 0.5));
    for (int i = 0; i < 64 * 64; i++)
        CHECK(C_R(l->px[i]) <= 255);

    /* Holes by winding: a square inside a square, drawn the other way round */
    surface *h = black(64, 64);
    float two[16] = { 0, 0, 40, 0, 40, 40, 0, 40, 10, 10, 10, 30, 30, 30, 30, 10 };
    int counts[2] = { 4, 4 };
    gfx_polygons(h, two, counts, 2, COLOR(0xFFFFFF));
    CHECK(near(coverage_sum(h), 1600 - 400, 0.5));
    CHECK(C_R(h->px[20 * 64 + 20]) == 0);

    /* Clipping */
    surface *c = black(64, 64);
    gfx_clip(c, rect_make(0, 0, 16, 64));
    gfx_polygon(c, quad, 4, COLOR(0xFFFFFF));
    CHECK(near(coverage_sum(c), (16 - 10.25) * 9.75, 0.5));

    /* Half-transparent colour over black */
    surface *t = black(8, 8);
    gfx_fill(t, rect_make(0, 0, 8, 8), COLOR_A(0xFFFFFF, 128));
    CHECK(near(C_R(t->px[0]), 128, 1));
    surface_free(s);
    surface_free(r);
    surface_free(l);
    surface_free(h);
    surface_free(c);
    surface_free(t);
}

static void test_rounds(void)
{
    surface *s = black(40, 40);
    gfx_round(s, rect_make(5, 5, 20, 20), 4, COLOR(0xFFFFFF));
    /* The square less the corners' 4x4 squares plus their quarter circles */
    CHECK(near(coverage_sum(s), 400 - 4 * (16 - 3.14159265 * 4), 1.5));
    CHECK(C_R(s->px[5 * 40 + 5]) == 0);
    CHECK(C_R(s->px[15 * 40 + 5]) == 255);

    surface *f = black(40, 40);
    gfx_round_frame(f, rect_make(5, 5, 20, 20), 0, 1, COLOR(0xFFFFFF));
    CHECK(near(coverage_sum(f), 76, 0.1));
    surface_free(s);
    surface_free(f);
}

static void test_overlay_surface(void)
{
    /* A surface placed in the window: window coordinates draw into it */
    surface *o = surface_at(rect_make(100, 50, 20, 10));
    gfx_fill(o, rect_make(0, 0, 200, 200), COLOR(0x123456));
    CHECK(o->px[0] == COLOR(0x123456) && o->px[199] == COLOR(0x123456));
    gfx_fill(o, rect_make(105, 52, 1, 1), COLOR(0xFF0000));
    CHECK(o->px[2 * 20 + 5] == COLOR(0xFF0000));
    surface *w = surface_new(200, 100);
    gfx_copy(w, o, rect_make(0, 0, 200, 100));
    CHECK(w->px[52 * 200 + 105] == COLOR(0xFF0000) && w->px[49 * 200 + 105] == COLOR(0xFFFFFF));
    surface_free(o);
    surface_free(w);
}

/* ---- the canvas view ---- */

static void test_canvas(void)
{
    rgba red = { 255, 0, 0, 255 };
    img *im = img_new(4, 4, red);
    uint8_t *p = img_at(im, 1, 0);
    p[0] = 0, p[2] = 255;   /* (1,0) blue */
    p = img_at(im, 0, 1);
    p[3] = 0;               /* (0,1) transparent */
    surface *s = surface_new(16, 16);
    gfx_canvas_checker(8);
    gfx_canvas(s, rect_make(0, 0, 8, 8), im, 0, 0, 2);
    CHECK(s->px[0] == COLOR(0xFF0000) && s->px[1] == COLOR(0xFF0000));
    CHECK(s->px[2] == COLOR(0x0000FF) && s->px[1 * 16 + 3] == COLOR(0x0000FF));
    CHECK(s->px[2 * 16 + 0] == COLOR(0xFFFFFF));    /* the checkerboard's light square */
    CHECK(s->px[8 * 16] == COLOR(0xFFFFFF));        /* outside `to`: untouched */

    /* Zoomed out: each screen pixel the average of a 2 x 2 block */
    surface *z = surface_new(4, 4);
    gfx_canvas(z, rect_make(0, 0, 2, 2), im, 0, 0, 0.5);
    /* top-left block: red, blue, transparent, red over the light checker */
    CHECK(near(C_R(z->px[0]), (255 + 0 + 255 + 255) / 4.0, 2));
    CHECK(near(C_B(z->px[0]), (0 + 255 + 255 + 0) / 4.0, 2));
    CHECK(z->px[1] == COLOR(0xFF0000));
    surface_free(s);
    surface_free(z);
    img_free(im);
}

/* ---- text ---- */

static void test_text(void)
{
    font *f = font_get(12.5f, FONT_REGULAR);
    CHECK(font_height(f) >= 12 && font_height(f) <= 20);
    CHECK(text_width(f, "", -1) == 0);
    CHECK(text_width(f, "iii", -1) < text_width(f, "WWW", -1));
    CHECK(text_width(f, "Home", 2) == text_width(f, "Ho", -1));
    CHECK(text_width(font_get(25, FONT_REGULAR), "Paint", -1) > text_width(f, "Paint", -1));
    CHECK(text_width(f, "\xC3\x97", -1) > 0);   /* the multiplication sign of "1152 x 648px" */
    int one = text_wrap(NULL, f, rect_make(0, 0, 1000, 0), ALIGN_LEFT, "one line", 0);
    CHECK(one == font_height(f));
    CHECK(text_wrap(NULL, f, rect_make(0, 0, 60, 0), ALIGN_LEFT, "several words that need wrapping here", 0) > 2 * one);
    CHECK(text_wrap(NULL, f, rect_make(0, 0, 1000, 0), ALIGN_LEFT, "two\nlines", 0) == 2 * one);

    /* Drawn text lands in its box, and a long one is cut to fit */
    surface *s = surface_new(200, 40);
    text_in(s, f, rect_make(10, 10, 50, 20), ALIGN_LEFT, "A rather long line of text", COLOR(0));
    int inked_outside = 0, inked = 0;
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 200; x++)
            if (s->px[y * 200 + x] != COLOR(0xFFFFFF))
                inked++, inked_outside += !rect_has(rect_make(10, 10, 50, 20), x, y);
    CHECK(inked > 20 && inked_outside == 0);
    surface_free(s);
}

/* ---- icons ---- */

static void test_icons(void)
{
    surface *s = surface_new(40, 40);
    for (int i = 0; i < ICON_COUNT; i++)
    {
        gfx_fill(s, rect_make(0, 0, 40, 40), COLOR(0xFFFFFF));
        icon_draw(s, i, i == ICON_OUTLINE ? COLOR(0xFF0000) : 0, 4, 4, 32, 1);
        int inked = 0, outside = 0;
        for (int y = 0; y < 40; y++)
            for (int x = 0; x < 40; x++)
                if (s->px[y * 40 + x] != COLOR(0xFFFFFF))
                    inked++, outside += !rect_has(rect_make(4, 4, 32, 32), x, y);
        CHECK(inked > 20);
        CHECK(outside == 0);
    }
    /* Disabled: grey and faded */
    gfx_fill(s, rect_make(0, 0, 40, 40), COLOR(0xFFFFFF));
    icon_draw(s, ICON_DELETE, 0, 4, 4, 32, 0);
    color c = s->px[20 * 40 + 20];
    CHECK(c != COLOR(0xFFFFFF) && abs(C_R(c) - C_G(c)) < 4 && C_R(c) > 150);
    surface_free(s);
}

/* ---- the kit ---- */

static surface *win;
static double now = 10;

static void send(int type, int x, int y)
{
    ui_event e = { 0 };
    e.type = type;
    e.x = x;
    e.y = y;
    e.clicks = 1;
    kit_event(&e);
}

static void test_layout(void)
{
    kit_init();
    kit_begin(win, now);
    kit_area(rect_make(10, 10, 300, 100), 1, 4);
    rect a = kit_alloc(20, 10), b = kit_alloc(30, 10);
    CHECK(a.x == 10 && b.x == 34 && b.y == 10);
    kit_col(2);
    rect c = kit_alloc(5, 5), d = kit_alloc(8, 5);
    rect col = kit_end_layout();
    CHECK(c.x == 68 && d.y == 17 && col.w == 8 && col.h == 12);
    kit_space(10);
    rect e = kit_alloc(1, 1);
    CHECK(e.x == 68 + 8 + 10 + 4);    /* the gap goes before an item, not before a space */
    rect used = kit_area_end();
    CHECK(used.x == 10 && used.w == e.x + 1 - 10 && used.h == 12);
    kit_area(rect_make(0, 0, 200, 50), 1, 0);
    kit_row_rtl(2, 20);
    rect r1 = kit_alloc(10, 10), r2 = kit_alloc(10, 10);
    kit_end_layout();
    kit_area_end();
    CHECK(r1.x == 190 && r2.x == 178 && r1.y == 5);
    kit_end();
}

static kit_resp button_frame(int *clicks)
{
    kit_begin(win, now);
    kit_area(rect_make(0, 0, 400, 300), 0, 0);
    kit_resp r = kit_dialog_button("OK", 1, 1, 0);
    *clicks += r.clicked;
    kit_area_end();
    kit_end();
    return r;
}

static void test_clicks(void)
{
    kit_init();
    int clicks = 0;
    button_frame(&clicks);
    /* Press and release in one frame is one click */
    send(EV_MOVE, 10, 10);
    send(EV_DOWN, 10, 10);
    send(EV_UP, 10, 10);
    CHECK(kit_wants_frame());
    button_frame(&clicks);
    CHECK(clicks == 1);
    /* Over two frames, too; released outside it is no click */
    send(EV_DOWN, 10, 10);
    kit_resp held = button_frame(&clicks);
    CHECK(held.held && clicks == 1);
    send(EV_UP, 10, 10);
    button_frame(&clicks);
    CHECK(clicks == 2);
    send(EV_DOWN, 10, 10);
    button_frame(&clicks);
    send(EV_MOVE, 300, 200);
    send(EV_UP, 300, 200);
    button_frame(&clicks);
    CHECK(clicks == 2);
    /* Two clicks waiting for one frame are two clicks, one frame each */
    send(EV_DOWN, 10, 10);
    send(EV_UP, 10, 10);
    send(EV_DOWN, 10, 10);
    send(EV_UP, 10, 10);
    button_frame(&clicks);
    CHECK(clicks == 3 && kit_wants_frame());
    button_frame(&clicks);
    CHECK(clicks == 4);
    /* Hovered when the pointer is over it */
    send(EV_MOVE, 5, 5);
    CHECK(button_frame(&clicks).hovered);
    CHECK(!kit_wants_frame());
}

static int menu_open, picked;

static void menu_frame(void)
{
    for (int pass = 0; pass < 3; pass++)
    {
        kit_begin(win, now);
        kit_area(rect_make(0, 0, 400, 300), 0, 0);
        kit_resp b = rb_small_button(ICON_ROTATE, 0, "Rotate", 1, 0, 1);
        menu_open = 0;
        if (kit_popup_menu(&b, 200, 0))
        {
            menu_open = 1;
            if (rb_menu_item(ICON_ROTATE, 0, "Rotate right", 0, 1).clicked)
                picked = 1;
            rb_menu_item(ICON_FLIP_VERTICAL, 0, "Flip vertical", 0, 1);
            kit_popup_end();
        }
        /* Something under the menu that must not get its clicks */
        kit_space(40);
        if (kit_dialog_button("Under", 0, 1, 0).clicked)
            picked = 2;
        kit_area_end();
        if (!kit_end())
            break;
    }
}

static void click(int x, int y)
{
    send(EV_MOVE, x, y);
    send(EV_DOWN, x, y);
    send(EV_UP, x, y);
}

static void test_popup(void)
{
    kit_init();
    picked = 0;
    menu_frame();
    CHECK(!menu_open);
    click(10, 10);
    menu_frame();
    CHECK(menu_open);
    menu_frame();
    CHECK(menu_open);
    /* The button "Under" is at y 62..86, under the open menu: the menu's row gets the click */
    click(30, 40);
    menu_frame();
    CHECK(picked == 1 && kit_wants_frame());
    menu_frame();
    CHECK(!menu_open);
    /* Open again, then a click outside closes it */
    click(10, 10);
    menu_frame();
    CHECK(menu_open);
    click(380, 280);
    menu_frame();
    CHECK(!menu_open);
    /* Escape closes it */
    click(10, 10);
    menu_frame();
    CHECK(menu_open);
    ui_event esc = { 0 };
    esc.type = EV_KEY;
    esc.key = KEY_ESCAPE;
    kit_event(&esc);
    menu_frame();
    CHECK(!menu_open);
}

static char field[32];
static int field_out;

static void field_frame(void)
{
    kit_begin(win, now);
    kit_area(rect_make(0, 0, 400, 300), 0, 0);
    field_out = kit_text_field("name", field, sizeof field, 8, 150, "hint", 0);
    kit_area_end();
    kit_end();
}

static void type(const char *text)
{
    ui_event e = { 0 };
    e.type = EV_TEXT;
    e.text = text;
    kit_event(&e);
}

static void key(int k, int mods)
{
    ui_event e = { 0 };
    e.type = EV_KEY;
    e.key = k;
    e.mods = mods;
    kit_event(&e);
}

static void test_text_field(void)
{
    kit_init();
    strcpy(field, "ab");
    field_frame();
    click(140, 8);
    field_frame();
    CHECK(kit_focused() != 0);
    type("cd");
    field_frame();
    CHECK(!strcmp(field, "abcd") && (field_out & KIT_CHANGED));
    key(KEY_LEFT, 0);
    key(KEY_LEFT, 0);
    key(KEY_BACKSPACE, 0);
    field_frame();
    CHECK(!strcmp(field, "acd"));
    key(KEY_HOME, 0);
    key(KEY_END, KMOD_SHIFT);
    type("\xC3\xA9t\xC3\xA9 long text");
    field_frame();
    CHECK(!strcmp(field, "\xC3\xA9t\xC3\xA9 long"));    /* at most 8 characters */
    key(KEY_ENTER, 0);
    field_frame();
    CHECK((field_out & KIT_SUBMITTED) && kit_focused() == 0);
    /* A backlog of typing (each character a key and its text) keeps its order, and Enter after 40 keys still counts */
    field[0] = 0;
    click(140, 8);
    field_frame();
    const char *typed = "ab\bc";
    for (const char *c = typed; *c; c++)
    {
        key(*c == '\b' ? KEY_BACKSPACE : *c - 'a' + 'A', 0);
        if (*c != '\b')
            type((char[2]){ *c, 0 });
    }
    for (int i = 0; i < 40; i++)
        key(KEY_SHIFT, 0);
    key(KEY_ENTER, 0);
    int submitted = 0;
    for (int f = 0; f < 8; f++)
    {
        field_frame();
        submitted |= field_out & KIT_SUBMITTED;
    }
    CHECK(!strcmp(field, "ac") && submitted);
    /* A number takes typing after a click, even a press and release in one frame */
    float num = 100;
    for (int f = 0; f < 4; f++)
    {
        if (f == 1)
            click(20, 8);
        if (f == 2)
            type("50");
        if (f == 3)
            key(KEY_ENTER, 0);
        kit_begin(win, now);
        kit_area(rect_make(0, 0, 400, 300), 0, 0);
        kit_number("num", &num, 1, 1000, 60);
        kit_area_end();
        kit_end();
        if (f == 2)
            CHECK(num == 50);   /* while typing, as egui's DragValue (linked fields follow at once) */
    }
    CHECK(num == 50);
    /* Tab moves on to the next number */
    float num2 = 7;
    for (int f = 0; f < 5; f++)
    {
        if (f == 1)
            click(20, 8);
        if (f == 2)
            key(KEY_TAB, 0);
        if (f == 3)
            type("9");
        if (f == 4)
            key(KEY_ENTER, 0);
        kit_begin(win, now);
        kit_area(rect_make(0, 0, 400, 300), 0, 0);
        kit_number("num", &num, 1, 1000, 60);
        kit_number("num2", &num2, 1, 1000, 60);
        kit_area_end();
        kit_end();
    }
    CHECK(num == 50 && num2 == 9 && kit_focused() == 0);
    /* Typing with nothing focused goes to the app */
    type("x");
    kit_begin(win, now);
    CHECK(kit_text() && !strcmp(kit_text(), "x"));
    kit_end();
}

static void test_dialog(void)
{
    kit_init();
    int closed = 0, frames = 0;
    for (int f = 0; f < 3; f++)
    {
        kit_begin(win, now);
        if (kit_dialog_begin("d", "Title", 300, &closed))
        {
            kit_label("Body");
            kit_dialog_end();
        }
        frames++;
        if (!kit_end())
            break;
    }
    CHECK(frames == 2);     /* the first pass learnt its height */
    CHECK(!kit_free_at(400, 300) && !kit_free_at(5, 5));   /* modal: nothing below gets the pointer */
    click(5, 5);            /* on the backdrop */
    kit_begin(win, now);
    kit_dialog_begin("d", "Title", 300, &closed);
    kit_dialog_end();
    kit_end();
    CHECK(closed);
}

int main(void)
{
    os_init();
    test_rects();
    test_coverage();
    test_rounds();
    test_overlay_surface();
    test_canvas();
    if (font_init() != 0)
        fprintf(stderr, "no UI font here: skipping the text, icon and kit tests\n");
    else
    {
        theme_set_scale(100);
        win = surface_new(800, 600);
        test_text();
        test_icons();
        test_layout();
        test_clicks();
        test_popup();
        test_text_field();
        test_dialog();
        surface_free(win);
    }
    printf("test_kit: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
