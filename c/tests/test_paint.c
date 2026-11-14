/*
 * Paint (C port) - tests of the pixel engine (Rust: tests.rs, and the engine side of app.rs's tests)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os/os.h"
#include "paint/brush.h"
#include "paint/history.h"
#include "paint/live_shape.h"
#include "paint/logo.h"
#include "paint/raster.h"
#include "paint/shapes.h"
#include "paint/text.h"
#include "paint/text_edit.h"
#include "paint/transform.h"

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

static float coverage_sum(const mask *m, irect r)
{
    float s = 0.0f;
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++)
            s += mask_get(m, x, y);
    return s;
}

static rgba px(const img *im, int x, int y)
{
    const uint8_t *p = img_at(im, x, y);
    return (rgba){ p[0], p[1], p[2], p[3] };
}

static int same(const img *a, const img *b)
{
    return a->w == b->w && a->h == b->h && !memcmp(a->px, b->px, (size_t)a->w * a->h * 4);
}

static int irect_eq(irect a, irect b)
{
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

/* ---- tests.rs ---- */

static void polygon_fill_covers_its_area(void)
{
    mask *m = mask_new(100, 100);
    /* 30x20 rectangle at a fractional offset, plus a triangle of area 200. */
    pt rect[4] = { { 10.5f, 10.25f }, { 40.5f, 10.25f }, { 40.5f, 30.25f }, { 10.5f, 30.25f } };
    irect r = mask_fill_polygon(m, rect, 4, 1.0f);
    CHECK(fabsf(coverage_sum(m, r) - 600.0f) < 1.0f, "rect area %f", coverage_sum(m, r));
    mask_free(m);
    m = mask_new(100, 100);
    pt tri[3] = { { 50, 50 }, { 70, 50 }, { 50, 70 } };
    r = mask_fill_polygon(m, tri, 3, 1.0f);
    CHECK(fabsf(coverage_sum(m, r) - 200.0f) < 2.0f, "triangle area %f", coverage_sum(m, r));
    mask_free(m);
}

static void capsule_is_antialiased_and_bounded(void)
{
    mask *m = mask_new(64, 64);
    irect r = mask_capsule(m, pt_make(10.0f, 32.3f), pt_make(50.0f, 32.3f), 4.0f, 1.0f);
    CHECK(mask_get(m, 30, 31) == 1.0f, "centre %f", mask_get(m, 30, 31));
    CHECK(mask_get(m, 30, 20) == 0.0f, "outside %f", mask_get(m, 30, 20));
    float edge = mask_get(m, 30, 36);
    CHECK(edge > 0.0f && edge < 1.0f, "edge coverage %f", edge);
    /* Roughly length x width + a disc for the caps. */
    float area = coverage_sum(m, r), expected = 40.0f * 8.0f + 3.14159265f * 16.0f;
    CHECK(fabsf(area - expected) < expected * 0.06f, "area %f vs %f", area, expected);
    mask_free(m);
}

static void square_line_is_continuous(void)
{
    mask *m = mask_new(40, 40);
    mask_square_line(m, pt_make(2, 2), pt_make(30, 17), 1);
    for (int x = 2; x < 30; x++)
    {
        int any = 0;
        for (int y = 0; y < 40; y++)
            any |= mask_get(m, x, y) == 1.0f;
        CHECK(any, "gap at x=%d", x);
    }
    mask_free(m);
}

static void flood_fill_respects_boundaries(void)
{
    img *im = img_new(20, 20, WHITE);
    for (int i = 0; i < 20; i++)
        memcpy(img_at(im, 10, i), &BLACK, 4);
    irect r = raster_flood_fill(im, 2, 2, (rgba){ 255, 0, 0, 255 }, 0);
    CHECK(irect_eq(r, irect_make(0, 0, 10, 20)), "filled %d %d %d %d", r.x0, r.y0, r.x1, r.y1);
    CHECK(rgba_eq(px(im, 9, 19), (rgba){ 255, 0, 0, 255 }), "corner not filled");
    CHECK(rgba_eq(px(im, 10, 5), BLACK), "line overwritten");
    CHECK(rgba_eq(px(im, 11, 5), WHITE), "leaked past the line");
    /* Filling with the same colour is a no-op. */
    CHECK(irect_empty(raster_flood_fill(im, 2, 2, (rgba){ 255, 0, 0, 255 }, 0)), "refill did something");
    img_free(im);
}

static void flood_fill_tolerance_terminates_when_fill_matches_target(void)
{
    img *im = img_new(8, 8, (rgba){ 200, 200, 200, 255 });
    irect r = raster_flood_fill(im, 0, 0, (rgba){ 205, 205, 205, 255 }, 20);
    CHECK(irect_eq(r, irect_make(0, 0, 8, 8)), "filled %d %d %d %d", r.x0, r.y0, r.x1, r.y1);
    img_free(im);
}

static void blend_over_mixes_colours(void)
{
    CHECK(rgba_eq(rgba_over(WHITE, BLACK, 1.0f), BLACK), "opaque");
    CHECK(rgba_eq(rgba_over(WHITE, BLACK, 0.0f), WHITE), "transparent");
    rgba half = rgba_over(WHITE, BLACK, 0.5f);
    CHECK(half.r >= 126 && half.r <= 129 && half.a == 255, "half %d %d", half.r, half.a);
    /* Painting onto transparency keeps the paint colour. */
    rgba onto = rgba_over((rgba){ 0, 0, 0, 0 }, (rgba){ 10, 20, 30, 255 }, 0.5f);
    CHECK(rgba_eq(onto, (rgba){ 10, 20, 30, 128 }), "onto clear %d %d %d %d", onto.r, onto.g, onto.b, onto.a);
}

static void history_round_trips_patches_and_replacements(void)
{
    img *original = img_new(16, 16, WHITE);
    img *im = img_dup(original);
    history *h = history_new();

    raster_fill_rect(im, irect_make(2, 3, 6, 9), BLUE);
    img *after_patch = img_dup(im);
    change *c = change_patch(original, im, irect_make(0, 0, 16, 16));
    CHECK(c != NULL, "pixels changed");
    history_push(h, c);

    img *before_rotate = img_dup(im);
    img *rotated = img_rotate90(im);
    img_free(im);
    im = rotated;
    history_push(h, change_replace(before_rotate, im));

    CHECK(history_undo(h, &im), "undo 1");
    CHECK(same(im, after_patch), "undo 1 picture");
    CHECK(history_undo(h, &im), "undo 2");
    CHECK(same(im, original), "undo 2 picture");
    CHECK(!history_undo(h, &im), "nothing more to undo");
    CHECK(history_redo(h, &im), "redo");
    CHECK(same(im, after_patch), "redo picture");
    CHECK(history_state_id(h) == 1 && history_redo_id(h) == 2, "ids %llu %llu", (unsigned long long)history_state_id(h),
          (unsigned long long)history_redo_id(h));
    /* Identical before/after is not recorded. */
    CHECK(change_patch(original, original, irect_make(0, 0, 16, 16)) == NULL, "empty patch");
    history_free(h);
    img_free(im);
    img_free(after_patch);
    img_free(original);
}

static uint32_t rnd(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return (uint32_t)(*s >> 32);
}

/* Photo-like noise (hardly two neighbours alike) over the top two thirds, flat white below */
static img *photo(int w, int h, uint64_t *rng)
{
    img *im = img_new(w, h, WHITE);
    for (int y = 0; y < h * 2 / 3; y++)
        for (int x = 0; x < w; x++)
        {
            uint32_t v = rnd(rng);
            uint8_t *p = img_at(im, x, y);
            p[0] = (uint8_t)(x + (v & 15));
            p[1] = (uint8_t)(y + (v >> 4 & 15));
            p[2] = (uint8_t)(v >> 8);
            p[3] = 255;
        }
    return im;
}

/* Random edits of every kind the app records, then undo all and redo all: every picture comes back
 * byte for byte, both sides of each step unpack exactly, and no step packs bigger than its raw pixels */
static void history_packs_every_kind_of_step(void)
{
    enum { STEPS = 80 };
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    img *im = photo(120, 90, &rng), *states[STEPS + 1];
    history *h = history_new();
    paint_font font;
    int have_font = fonts_count() > 0 && fonts_load(fonts_default_family(), 0, 0, &font) == 0;
    int n = 0;
    states[n++] = img_dup(im);
    for (int s = 0; s < STEPS; s++)
    {
        img *before = img_dup(im);
        mask *m = mask_new(im->w, im->h);
        pt a = pt_make((float)(rnd(&rng) % im->w), (float)(rnd(&rng) % im->h));
        pt b = pt_make((float)(rnd(&rng) % im->w), (float)(rnd(&rng) % im->h));
        irect r = IRECT_EMPTY;
        change *c = NULL;
        int kind = (int)(rnd(&rng) % 8);
        if (kind == 0)          /* pencil */
        {
            r = mask_square_line(m, a, b, 1);
            raster_blend_mask(im, m, BLACK, 1.0f, r);
        }
        else if (kind == 1)     /* brush */
        {
            brush_kind k = (brush_kind)(rnd(&rng) % BRUSH_COUNT);
            r = brush_segment(k, m, a, b, 10.0f, &rng);
            raster_blend_mask(im, m, RED, brush_opacity(k), r);
        }
        else if (kind == 2)     /* fill */
            r = raster_flood_fill(im, (int)a.x, (int)a.y, BLUE, (uint8_t)(rnd(&rng) % 80));
        else if (kind == 3)     /* a pasted photo, partly off the picture */
        {
            img *p = photo(30 + (int)(rnd(&rng) % 40), 20 + (int)(rnd(&rng) % 40), &rng);
            r = irect_make((int)a.x - 10, (int)a.y - 10, (int)a.x - 10 + p->w, (int)a.y - 10 + p->h);
            raster_replace(im, p, r.x0, r.y0);
            img_free(p);
        }
        else if (kind == 4 && have_font)
        {
            r = ptext_render(m, &font, 18.0f, "Hello", a, 0, 0);
            raster_blend_mask(im, m, BLACK, 1.0f, r);
        }
        else if (kind == 5)     /* two colours at random: short runs everywhere, the packer's worst case */
        {
            r = irect_intersect(irect_around(a.x, a.y, b.x, b.y, 2.0f), irect_of(im));
            for (int y = r.y0; y < r.y1; y++)
                for (int x = r.x0; x < r.x1; x++)
                    memcpy(img_at(im, x, y), rnd(&rng) & 1 ? &RED : &WHITE, 4);
        }
        else if (kind >= 6)     /* invert, rotate */
        {
            img *out = kind == 6 ? raster_invert(im) : img_rotate90(im);
            c = change_replace(before, out);
            before = NULL;
            img_free(im);
            im = out;
        }
        if (!c && before)
            c = change_patch(before, im, r);
        if (c)
        {
            size_t raw = ((size_t)c->bw * c->bh + (size_t)c->aw * c->ah) * 4;
            CHECK(c->bytes <= raw + 5 * (size_t)(c->bh + c->ah), "step %d (kind %d): %lu bytes, raw %lu", s, kind,
                  (unsigned long)c->bytes, (unsigned long)raw);
            int patch = c->kind == CHANGE_PATCH;
            img *was = patch ? img_crop(states[n - 1], c->x, c->y, c->bw, c->bh) : img_dup(states[n - 1]);
            img *now = patch ? img_crop(im, c->x, c->y, c->aw, c->ah) : img_dup(im);
            img *b0 = change_image(c, 0), *a0 = change_image(c, 1);
            CHECK(same(b0, was) && same(a0, now), "step %d (kind %d) unpacks", s, kind);
            img_free(was);
            img_free(now);
            img_free(b0);
            img_free(a0);
            history_push(h, c);
            states[n++] = img_dup(im);
        }
        img_free(before);
        mask_free(m);
    }
    CHECK(n > STEPS / 2, "only %d steps changed anything", n - 1);
    for (int i = n - 1; i > 0; i--)
        CHECK(history_undo(h, &im) && same(im, states[i - 1]), "undo to %d", i - 1);
    CHECK(!history_can_undo(h), "all undone");
    for (int i = 1; i < n; i++)
        CHECK(history_redo(h, &im) && same(im, states[i]), "redo to %d", i);
    CHECK(!history_can_redo(h), "all redone");
    for (int i = 0; i < n; i++)
        img_free(states[i]);
    history_free(h);
    img_free(im);
}

static void shapes_fit_their_box(void)
{
    static pt pts[SHAPE_MAX_CONTOURS * SHAPE_MAX_POINTS];
    int counts[SHAPE_MAX_CONTOURS];
    for (int k = 0; k < SHAPE_COUNT; k++)
    {
        int nc = shape_contours(k, pt_make(10, 20), pt_make(110, 70), pts, counts);
        CHECK(counts[0] >= 2, "%s", shape_name(k));
        for (int c = 0; c < nc; c++)
            for (int i = 0; i < counts[c]; i++)
            {
                pt p = pts[c * SHAPE_MAX_POINTS + i];
                CHECK(p.x >= 9.99f && p.x <= 110.01f && p.y >= 19.99f && p.y <= 70.01f, "%s point %f,%f outside box", shape_name(k), p.x, p.y);
            }
    }
    /* The biggest oval fits the outline array */
    CHECK(shape_outline(SHAPE_OVAL, pt_make(0, 0), pt_make(5000, 5000), pts) == 360, "big oval");
    pt sq = shape_constrain(SHAPE_RECTANGLE, pt_make(0, 0), pt_make(30, -10), 1);
    CHECK(sq.x == 30.0f && sq.y == -30.0f, "square %f %f", sq.x, sq.y);
    pt line = shape_constrain(SHAPE_LINE, pt_make(0, 0), pt_make(50, 3), 1);
    CHECK(fabsf(line.y) < 1e-3f && fabsf(line.x - 50.0f) < 1e-3f, "line %f %f", line.x, line.y);
}

static void every_brush_lays_paint(void)
{
    uint64_t rng = 0x123456789abcdef0ull;
    for (int k = 0; k < BRUSH_COUNT; k++)
    {
        mask *m = mask_new(80, 80);
        irect r = brush_segment(k, m, pt_make(20, 40), pt_make(60, 40), 12.0f, &rng);
        CHECK(!irect_empty(r) && coverage_sum(m, r) > 10.0f, "%s", brush_name(k));
        mask_free(m);
    }
}

static void resize_canvas_pads_with_fill(void)
{
    img *im = img_new(4, 4, (rgba){ 1, 2, 3, 255 });
    img *out = raster_resize_canvas(im, 6, 3, (rgba){ 9, 9, 9, 255 });
    CHECK(out->w == 6 && out->h == 3, "size %dx%d", out->w, out->h);
    CHECK(rgba_eq(px(out, 3, 2), (rgba){ 1, 2, 3, 255 }), "kept");
    CHECK(rgba_eq(px(out, 5, 0), (rgba){ 9, 9, 9, 255 }), "padded");
    img_free(im);
    img_free(out);
}

static void text_renders_with_a_system_font(void)
{
    double t0 = os_now();
    int n = fonts_count();
    printf("   %d font families, scanned in %.0f ms; default %s\n", n, (os_now() - t0) * 1000.0, fonts_name(fonts_default_family()));
#ifdef _WIN32
    if (n == 0)
    {
        printf("   no fonts here (wine?): skipping the text test\n");
        return;
    }
#endif
    CHECK(n > 0, "no fonts found");
    if (n == 0)
        return;
    int fam = fonts_default_family();
    paint_font font;
    CHECK(fonts_load(fam, 0, 0, &font) == 0, "default font loads");
    text_size size = ptext_measure(&font, 32.0f, "Hello\nWorld");
    CHECK(size.width > 60.0f && size.height > 60.0f, "%fx%f", size.width, size.height);
    mask *m = mask_new(300, 120);
    irect r = ptext_render(m, &font, 32.0f, "Hello", pt_make(10, 10), 1, 1);
    CHECK(!irect_empty(r) && coverage_sum(m, r) > 100.0f, "rendered %f", coverage_sum(m, r));
    /* Bold and italic resolve to real faces or fall back to synthesized ones. */
    CHECK(fonts_load(fam, 1, 1, &font) == 0, "bold italic");
    mask_reset(m);
    r = ptext_render(m, &font, 32.0f, "Hello", pt_make(10, 10), 0, 0);
    CHECK(!irect_empty(r) && coverage_sum(m, r) > 100.0f, "bold italic rendered %f", coverage_sum(m, r));
    mask_free(m);
    /* Every family's regular face loads */
    int bad = 0;
    for (int i = 0; i < n; i++)
        if (fonts_load(i, 0, 0, &font) != 0)
        {
            if (bad++ < 5)
                printf("   can't load %s\n", fonts_name(i));
        }
    CHECK(bad == 0, "%d families don't load", bad);
    fonts_release();
}

/* ---- app.rs's tests, the engine side ---- */

static shape_style default_style(void)
{
    return (shape_style){ 1, 0, 3.0f, BLACK, WHITE };
}

/* Draw a live shape from a to b and commit it, as dragging out a shape does */
static void draw_shape(img **im, history *h, shape_kind kind, pt a, pt b, shape_style style)
{
    live_shape *s = live_shape_new(kind, a, 0, img_dup(*im));
    s->b = b;
    live_shape_render(s, *im, style);
    history_push(h, change_patch(s->snapshot, *im, live_shape_touched(s)));
    live_shape_free(s);
}

static void every_brush_changes_pixels_and_undoes_cleanly(void)
{
    for (int k = 0; k < BRUSH_COUNT; k++)
    {
        img *im = img_new(200, 120, WHITE), *original = img_dup(im);
        history *h = history_new();
        mask *m = mask_new(200, 120);
        uint64_t rng = 0x2545F4914F6CDD1Dull;
        pt p[3] = { { 20, 60 }, { 90, 60 }, { 160, 70 } };
        irect dirty = IRECT_EMPTY;
        for (int i = 0; i < 2; i++)
            dirty = irect_union(dirty, brush_segment(k, m, p[i], p[i + 1], 14.0f, &rng));
        raster_copy_rect(im, original, dirty);
        raster_blend_mask(im, m, BLACK, brush_opacity(k), dirty);
        CHECK(!same(im, original), "%s drew nothing", brush_name(k));
        history_push(h, change_patch(original, im, dirty));
        history_undo(h, &im);
        CHECK(same(im, original), "%s didn't undo cleanly", brush_name(k));
        mask_free(m);
        history_free(h);
        img_free(im);
        img_free(original);
    }
}

static void live_shape_moves_resizes_and_restyles(void)
{
    img *im = img_new(200, 120, WHITE), *original = img_dup(im);
    live_shape *s = live_shape_new(SHAPE_RECTANGLE, pt_make(20, 20), 0, img_dup(im));
    CHECK(live_shape_is_empty(s), "a click is empty");
    s->b = pt_make(60, 50);
    shape_style st = default_style();
    irect r = live_shape_render(s, im, st);
    CHECK(!irect_empty(r) && rgba_eq(px(im, 20, 30), BLACK), "outline drawn");
    CHECK(irect_empty(live_shape_render(s, im, st)), "unchanged: no redraw");
    /* Grabbing the body and moving it */
    CHECK(live_shape_contains(s, pt_make(40, 35), 4.0f), "body grabbed");
    live_shape_translate(s, s->a, s->b, 50.0f, 10.0f);
    live_shape_render(s, im, st);
    CHECK(rgba_eq(px(im, 20, 30), WHITE) && rgba_eq(px(im, 70, 40), BLACK), "moved");
    /* The bottom-right handle, then Shift for a square */
    pt hs[8];
    CHECK(live_shape_handles(s, hs) == 8 && live_shape_handle_at(s, hs[4], 3.0f) == 4, "handle 4");
    live_shape_drag_handle(s, 4, s->a, s->b, pt_make(150, 100), 0);
    CHECK(s->b.x == 150.0f && s->b.y == 100.0f, "resized");
    live_shape_drag_handle(s, 4, s->a, s->b, pt_make(130, 70), 1);
    CHECK(s->b.x - s->a.x == s->b.y - s->a.y, "square");
    /* Restyling from the ribbon redraws */
    st.fill = 1;
    st.fill_color = RED;
    live_shape_render(s, im, st);
    CHECK(rgba_eq(px(im, (int)(s->a.x + s->b.x) / 2, (int)(s->a.y + s->b.y) / 2), RED), "filled");
    live_shape_erase(s, im);
    CHECK(same(im, original), "erase puts back the picture");
    CHECK(live_shape_handle_cursor(0, 0) == SHAPE_CURSOR_NWSE && live_shape_handle_cursor(1, 3) == SHAPE_CURSOR_CROSS, "cursors");
    live_shape_free(s);
    /* A line's ends are its handles */
    s = live_shape_new(SHAPE_LINE, pt_make(10, 10), 0, img_dup(im));
    s->b = pt_make(100, 13);
    CHECK(live_shape_contains(s, pt_make(50, 12), 2.0f) && !live_shape_contains(s, pt_make(50, 30), 2.0f), "near the line");
    live_shape_drag_handle(s, 1, s->a, s->b, pt_make(100, 14), 1);
    CHECK(fabsf(s->b.y - 10.0f) < 1e-3f, "snapped to horizontal %f", s->b.y);
    live_shape_free(s);
    img_free(im);
    img_free(original);
}

static void fill_stays_inside_outline(void)
{
    img *im = img_new(200, 120, WHITE);
    history *h = history_new();
    draw_shape(&im, h, SHAPE_RECTANGLE, pt_make(20, 20), pt_make(80, 80), default_style());
    raster_flood_fill(im, 50, 50, RED, 26);
    CHECK(rgba_eq(px(im, 50, 50), RED), "filled inside");
    CHECK(rgba_eq(px(im, 120, 50), WHITE), "outside untouched");
    history_free(h);
    img_free(im);
}

static void fill_leaves_no_pale_ring_inside_an_antialiased_outline(void)
{
    img *im = img_new(200, 120, WHITE);
    history *h = history_new();
    shape_style st = default_style();
    st.size = 2.0f;
    draw_shape(&im, h, SHAPE_OVAL, pt_make(20, 20), pt_make(120, 100), st);
    raster_flood_fill(im, 70, 60, RED, 26);
    /* Inside the outline's centre line, every pixel is fill, outline, or a blend of
     * the two: never the old white showing through (high green). */
    float cx = 70, cy = 60, rx = 50, ry = 40;
    int pale = 0, leaked = 0;
    for (int y = 20; y < 100; y++)
        for (int x = 20; x < 120; x++)
        {
            float dx = ((float)x + 0.5f - cx) / rx, dy = ((float)y + 0.5f - cy) / ry;
            if (dx * dx + dy * dy < 1.0f && px(im, x, y).g > 60)
                pale++;
        }
    CHECK(pale == 0, "%d pale pixels between the fill and the outline", pale);
    /* And nothing leaked past the outline. */
    for (int y = 0; y < 120; y++)
        for (int x = 0; x < 200; x++)
        {
            float dx = ((float)x + 0.5f - cx) / (rx + 3.0f), dy = ((float)y + 0.5f - cy) / (ry + 3.0f);
            if (dx * dx + dy * dy > 1.0f && !rgba_eq(px(im, x, y), WHITE))
                leaked++;
        }
    CHECK(leaked == 0, "fill escaped the outline at %d pixels", leaked);
    history_free(h);
    img_free(im);
}

static void text_is_rendered_and_committed(void)
{
    if (fonts_count() == 0)
        return;
    img *im = img_new(200, 120, WHITE), *original = img_dup(im);
    text_style style = { fonts_default_family(), 24.0f, 0, 0, 0, 0, 0 };
    paint_font font;
    CHECK(fonts_load(style.family, 0, 0, &font) == 0, "font");
    mask *m = mask_new(200, 120);
    irect r = ptext_render(m, &font, text_style_px(&style), "Hello", pt_make(10, 10), 0, 0);
    raster_blend_mask(im, m, BLACK, 1.0f, r);
    CHECK(!same(im, original), "text drawn");
    history *h = history_new();
    history_push(h, change_patch(original, im, r));
    CHECK(history_can_undo(h), "recorded");
    history_undo(h, &im);
    CHECK(same(im, original), "undone");
    history_free(h);
    mask_free(m);
    img_free(im);
    img_free(original);
}

static void caret_geometry_and_editing(void)
{
    caret_layout cl;
    caret_layout_init(&cl);
    CHECK(cl.nlines == 1 && caret_line_len(&cl, 0) == 1 && cl.line_h == 16.0f, "default layout");
    char *text = malloc(1);
    text[0] = 0;
    text_cursor tc;
    text_cursor_init(&tc);
    text_cursor_insert(&tc, &text, "hello wörld");
    text_cursor_insert(&tc, &text, "\nline two");
    CHECK(!strcmp(text, "hello wörld\nline two") && tc.cursor == 20, "typed '%s' at %d", text, tc.cursor);
    if (fonts_count() > 0)
    {
        paint_font font;
        fonts_load(fonts_default_family(), 0, 0, &font);
        caret_layout_build(&cl, &font, 20.0f, text);
        CHECK(cl.nlines == 2 && caret_line_len(&cl, 0) == 12 && caret_line_len(&cl, 1) == 9, "lines");
        int l, c;
        caret_line_col(&cl, 14, &l, &c);
        CHECK(l == 1 && c == 2, "line_col %d %d", l, c);
        pt p = caret_pos(&cl, 14);
        CHECK(p.y == cl.line_h && p.x > 0.0f, "caret %f %f", p.x, p.y);
        CHECK(caret_index_at(&cl, p.x + 0.1f, p.y + 1.0f) == 14, "hit test %d", caret_index_at(&cl, p.x + 0.1f, p.y + 1.0f));
        CHECK(caret_index_at(&cl, -5.0f, -5.0f) == 0, "top-left");
        text_cursor_move_to(&tc, 14, 0);
        text_cursor_vertical(&tc, text, &cl, -1, 0);
        CHECK(caret_line_len(&cl, 0) > tc.cursor && tc.cursor > 0, "up %d", tc.cursor);
        text_cursor_line_end(&tc, &cl, 1);
        CHECK(tc.cursor == 11 && text_cursor_has_selection(&tc), "line end %d", tc.cursor);
        text_cursor_move_to(&tc, 20, 0);
        text_cursor_backspace(&tc, &text, &cl, 0, 1);
        CHECK(!strcmp(text, "hello wörld\n"), "deleted to line start: '%s'", text);
        text_cursor_undo(&tc, &text);
    }
    text_cursor_move_to(&tc, 11, 0);
    text_cursor_left(&tc, text, 1, 1);
    CHECK(tc.cursor == 6, "word left %d", tc.cursor);
    char *sel = text_cursor_selected(&tc, text);
    CHECK(!strcmp(sel, "wörld"), "selected '%s'", sel);
    free(sel);
    text_cursor_insert(&tc, &text, "there");
    CHECK(!strcmp(text, "hello there\nline two"), "replaced '%s'", text);
    text_cursor_backspace(&tc, &text, &cl, 1, 0);
    CHECK(!strcmp(text, "hello \nline two"), "word backspace '%s'", text);
    text_cursor_undo(&tc, &text);
    text_cursor_undo(&tc, &text);
    CHECK(!strcmp(text, "hello wörld\nline two"), "undo '%s'", text);
    text_cursor_redo(&tc, &text);
    CHECK(!strcmp(text, "hello there\nline two"), "redo '%s'", text);
    text_cursor_move_to(&tc, 0, 0);
    text_cursor_delete(&tc, &text, 1);
    CHECK(!strcmp(text, " there\nline two"), "word delete '%s'", text);
    text_cursor_select_all(&tc, text);
    text_cursor_delete_selection(&tc, &text);
    CHECK(!text[0] && tc.cursor == 0, "cleared");
    text_cursor_free(&tc);
    caret_layout_free(&cl);
    free(text);
}

static void transforms(void)
{
    img *im = img_new(3, 2, WHITE);
    raster_fill_rect(im, irect_make(0, 0, 1, 1), RED);
    img *r90 = img_rotate90(im), *r180 = img_rotate180(im), *r270 = img_rotate270(im);
    img *fh = img_flip_horizontal(im), *fv = img_flip_vertical(im);
    CHECK(r90->w == 2 && r90->h == 3 && rgba_eq(px(r90, 1, 0), RED), "rotate 90");
    CHECK(rgba_eq(px(r180, 2, 1), RED), "rotate 180");
    CHECK(r270->w == 2 && rgba_eq(px(r270, 0, 2), RED), "rotate 270");
    CHECK(rgba_eq(px(fh, 2, 0), RED) && rgba_eq(px(fv, 0, 1), RED), "flips");
    img *back = img_rotate270(r90);
    CHECK(same(back, im), "rotations undo each other");
    img *inv = raster_invert(im);
    CHECK(rgba_eq(px(inv, 0, 0), (rgba){ 18, 227, 219, 255 }) && rgba_eq(px(inv, 1, 0), (rgba){ 0, 0, 0, 255 }), "invert");
    img_free(r90), img_free(r180), img_free(r270), img_free(fh), img_free(fv), img_free(back), img_free(inv);
    img_free(im);

    /* Resize: a flat picture stays flat, a 2x upscale of a step keeps its ends, a downscale averages */
    img *flat = img_new(10, 10, BLUE), *big = img_resize(flat, 25, 7, RESIZE_TRIANGLE);
    int ok = big->w == 25 && big->h == 7;
    for (int i = 0; i < 25 * 7 && ok; i++)
        ok = rgba_eq(px(big, i % 25, i / 25), BLUE);
    CHECK(ok, "flat resize");
    img *step = img_new(4, 1, BLACK);
    raster_fill_rect(step, irect_make(2, 0, 4, 1), WHITE);
    img *up = img_resize(step, 8, 1, RESIZE_TRIANGLE), *down = img_resize(step, 2, 1, RESIZE_TRIANGLE);
    CHECK(px(up, 0, 0).r == 0 && px(up, 7, 0).r == 255 && px(up, 3, 0).r > 0 && px(up, 3, 0).r < 255, "upscale %d %d %d", px(up, 0, 0).r, px(up, 3, 0).r,
          px(up, 7, 0).r);
    /* The image crate's weights: 3/4 and 1/4 of the neighbours up, 3:3:1 (normalized) down */
    CHECK(px(up, 3, 0).r == 64 && px(up, 4, 0).r == 191, "triangle weights %d %d", px(up, 3, 0).r, px(up, 4, 0).r);
    CHECK(px(down, 0, 0).r == 36 && px(down, 1, 0).r == 219, "downscale %d %d", px(down, 0, 0).r, px(down, 1, 0).r);
    img_free(flat), img_free(big), img_free(step), img_free(up), img_free(down);
}

static void logo_renders(void)
{
    double t0 = os_now();
    img *big = logo_render(512);
    img *small = logo_render(64);
    printf("   logo 512 + 64 in %.0f ms\n", (os_now() - t0) * 1000.0);
    CHECK(big && big->w == 512 && small && small->w == 64, "sizes");
    CHECK(px(big, 2, 2).a == 0 && px(big, 256, 60).a == 255, "transparent corner, opaque tile");
    CHECK(px(small, 32, 32).a == 255 && px(small, 0, 0).a == 0, "small");
    img_free(big);
    img_free(small);
}

static void mask_tiles_come_and_go(void)
{
    mask *m = mask_new(1000, 1000);
    int tiles = 0;
    mask_capsule(m, pt_make(500, 500), pt_make(510, 500), 3.0f, 1.0f);
    for (int i = 0; i < m->tw * m->th; i++)
        tiles += m->tiles[i] != NULL;
    CHECK(tiles >= 1 && tiles <= 4, "%d tiles for a dab", tiles);
    mask_clear(m, irect_make(0, 0, 1000, 1000));
    tiles = 0;
    for (int i = 0; i < m->tw * m->th; i++)
        tiles += m->tiles[i] != NULL;
    CHECK(tiles == 0 && mask_get(m, 505, 500) == 0.0f, "cleared");
    /* A partial clear keeps the rest */
    mask_capsule(m, pt_make(10, 10), pt_make(100, 10), 3.0f, 1.0f);
    mask_clear(m, irect_make(0, 0, 50, 50));
    CHECK(mask_get(m, 20, 10) == 0.0f && mask_get(m, 80, 10) == 1.0f, "partial clear");
    mask_free(m);
}

int main(void)
{
    os_init();
    struct
    {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        { "polygon_fill_covers_its_area", polygon_fill_covers_its_area },
        { "capsule_is_antialiased_and_bounded", capsule_is_antialiased_and_bounded },
        { "square_line_is_continuous", square_line_is_continuous },
        { "flood_fill_respects_boundaries", flood_fill_respects_boundaries },
        { "flood_fill_tolerance_terminates_when_fill_matches_target", flood_fill_tolerance_terminates_when_fill_matches_target },
        { "blend_over_mixes_colours", blend_over_mixes_colours },
        { "history_round_trips_patches_and_replacements", history_round_trips_patches_and_replacements },
        { "history_packs_every_kind_of_step", history_packs_every_kind_of_step },
        { "shapes_fit_their_box", shapes_fit_their_box },
        { "every_brush_lays_paint", every_brush_lays_paint },
        { "resize_canvas_pads_with_fill", resize_canvas_pads_with_fill },
        { "text_renders_with_a_system_font", text_renders_with_a_system_font },
        { "every_brush_changes_pixels_and_undoes_cleanly", every_brush_changes_pixels_and_undoes_cleanly },
        { "live_shape_moves_resizes_and_restyles", live_shape_moves_resizes_and_restyles },
        { "fill_stays_inside_outline", fill_stays_inside_outline },
        { "fill_leaves_no_pale_ring_inside_an_antialiased_outline", fill_leaves_no_pale_ring_inside_an_antialiased_outline },
        { "text_is_rendered_and_committed", text_is_rendered_and_committed },
        { "caret_geometry_and_editing", caret_geometry_and_editing },
        { "transforms", transforms },
        { "logo_renders", logo_renders },
        { "mask_tiles_come_and_go", mask_tiles_come_and_go },
    };
    for (size_t i = 0; i < sizeof tests / sizeof *tests; i++)
    {
        int before = failures;
        tests[i].fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all paint tests passed\n");
    return 0;
}
