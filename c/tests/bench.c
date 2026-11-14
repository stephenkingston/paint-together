/*
 * Paint (C port) - timings of what a user feels, for deciding which files the Makefile builds -O2 (HOT)
 *
 * Not a test: `make bench` builds and runs it. Each line is the best of a few
 * runs, in milliseconds, on a 4000x3000 picture where a picture is involved.
 * Pictures to decode are made here (a smooth photo-like picture with noise),
 * so that every build decodes the same bytes.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/view.h"
#include "collab/patch.h"
#include "os/os.h"
#include "paint/raster.h"
#include "paint/text.h"
#include "ui/font.h"

#define W 4000
#define H 3000
#define RUNS 3

static surface *win;
static app *A;
static double best;

static void frame(void)
{
    for (int pass = 0; pass < 3; pass++)
    {
        kit_begin(win, 1000.0);
        view_build(A);
        if (!pass)
            view_keys(A);
        if (!kit_end())
            break;
    }
}

static double t0;
static void start(void)
{
    t0 = os_now();
}
static void stop(void)
{
    double ms = (os_now() - t0) * 1000.0;
    if (ms < best)
        best = ms;
}
static void report(const char *what)
{
    printf("%-40s %9.2f ms\n", what, best);
    fflush(stdout);
}
#define TIME(what, setup, body, after)                                          \
    do                                                                          \
    {                                                                           \
        best = 1e30;                                                            \
        for (int run_ = 0; run_ < RUNS; run_++)                                 \
        {                                                                       \
            setup;                                                              \
            start();                                                            \
            body;                                                               \
            stop();                                                             \
            after;                                                              \
        }                                                                       \
        report(what);                                                           \
    } while (0)

/* Smooth gradients and waves, with a little noise: compresses like a photo */
static img *photo(void)
{
    img *im = img_new(W, H, (rgba){ 255, 255, 255, 255 });
    uint32_t s = 12345;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            s ^= s << 13, s ^= s >> 17, s ^= s << 5;
            int n = (int)(s & 15) - 8;
            uint8_t *p = img_at(im, x, y);
            float v = sinf(x * 0.004f) * cosf(y * 0.003f), u = sinf((x + y) * 0.0021f);
            int r = (int)(128 + 100 * v) + n, g = (int)(128 + 90 * u) + n, b = (int)(x * 255 / W + y * 64 / H) / 2 + n;
            p[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            p[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            p[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
    return im;
}

static void use_picture(img *im)
{
    img_free(A->img);
    A->img = img_dup(im);
}

/* A long stroke: begin, 400 segments of about 9 px across the picture, end (the undo step) */
static void stroke(const char *name, app_tool tool, brush_kind k, float size, img *pic)
{
    char what[64];
    double seg = 1e30, end = 1e30;
    for (int run = 0; run < RUNS; run++)
    {
        use_picture(pic);
        if (tool == TOOL_BRUSH)
            app_pick_brush(A, k);
        else
            app_set_tool(A, tool);
        if (tool == TOOL_PENCIL)
            A->size_pencil = size;
        else
            A->size_brush = size;
        app_begin(A, pt_make(100, 100), BTN_LEFT, pt_make(0, 0));
        start();
        for (int i = 1; i <= 400; i++)
        {
            pt p = pt_make(100 + i * 8.5f, 100 + 1400 * sinf(i * 0.02f) * sinf(i * 0.02f));
            app_update(A, &p, 1, 0, 1.0f / 60.0f);
        }
        best = 1e30, stop(), seg = best < seg ? best : seg;
        start();
        app_end(A);
        best = 1e30, stop(), end = best < end ? best : end;
    }
    snprintf(what, sizeof what, "%s: 400 segments", name);
    best = seg, report(what);
    snprintf(what, sizeof what, "%s: end (undo step)", name);
    best = end, report(what);
}

int main(int argc, char **argv)
{
    os_init();
    if (font_init() != 0)
    {
        printf("no UI font here\n");
        return 1;
    }
    theme_set_scale(100);
    win = surface_new(1460, 900);
    kit_init();
    A = app_new();
    app_set_scale(A, 1.0f);
    frame();

    TIME("frame, whole window 1460x900", kit_repaint(), frame(), (void)0);

    img *pic = photo();
    TIME("canvas 4000x3000 at 8x", (void)0, gfx_canvas(win, rect_make(0, 0, 1460, 900), pic, 1500, 1200, 8.0), (void)0);
    TIME("canvas 4000x3000 at 0.33x", (void)0, gfx_canvas(win, rect_make(0, 0, 1460, 900), pic, 0, 0, 0.33), (void)0);
    TIME("canvas 4000x3000 at 1x", (void)0, gfx_canvas(win, rect_make(0, 0, 1460, 900), pic, 100, 100, 1.0), (void)0);

    use_picture(pic);
    A->zoom = 0.33f;
    TIME("frame with a 4000x3000 picture", kit_repaint(), frame(), (void)0);

    stroke("pencil 3px", TOOL_PENCIL, BRUSH_BRUSH, 3, pic);
    stroke("brush 30px", TOOL_BRUSH, BRUSH_BRUSH, 30, pic);
    stroke("oil 40px", TOOL_BRUSH, BRUSH_OIL, 40, pic);
    stroke("watercolor 40px", TOOL_BRUSH, BRUSH_WATERCOLOR, 40, pic);
    stroke("airbrush 50px (moving)", TOOL_BRUSH, BRUSH_AIRBRUSH, 50, pic);
    TIME("airbrush 50px: 120 frames held", (use_picture(pic), app_pick_brush(A, BRUSH_AIRBRUSH), A->size_brush = 50,
                                            app_begin(A, pt_make(2000, 1500), BTN_LEFT, pt_make(0, 0))),
         for (int i = 0; i < 120; i++) { pt p = pt_make(2000, 1500); app_update(A, &p, 1, 0, 1.0f / 60.0f); }, app_end(A));

    img *white = img_new(W, H, (rgba){ 255, 255, 255, 255 });
    TIME("fill a white 4000x3000 (with undo)", (use_picture(white), app_set_tool(A, TOOL_FILL)),
         app_begin(A, pt_make(10, 10), BTN_LEFT, pt_make(0, 0)), app_end(A));
    TIME("fill the photo, 30% tolerance", (use_picture(pic), app_set_tool(A, TOOL_FILL), A->fill_tolerance = 30),
         app_begin(A, pt_make(2000, 1500), BTN_LEFT, pt_make(0, 0)), app_end(A));
    img_free(white);

    const char *dir = getenv("TMPDIR") ? getenv("TMPDIR") : getenv("TEMP") ? getenv("TEMP") : "/tmp";
    char png[512], jpg[512];
    snprintf(png, sizeof png, "%s/paint-bench.png", dir);
    snprintf(jpg, sizeof jpg, "%s/paint-bench.jpg", dir);
    const char *err;
    TIME("save 4000x3000 PNG", (void)0, img_save(pic, png, &err), (void)0);
    TIME("save 4000x3000 JPEG", (void)0, img_save(pic, jpg, &err), (void)0);
    size_t png_len, jpg_len;
    char *png_data = os_read_file(png, &png_len), *jpg_data = os_read_file(jpg, &jpg_len);
    img *got = NULL;
    TIME("open 4000x3000 PNG", img_free(got), got = img_load_mem(png_data, png_len, &err), (void)0);
    TIME("open 4000x3000 JPEG", img_free(got), got = img_load_mem(jpg_data, jpg_len, &err), (void)0);
    img_free(got);
    printf("  (PNG %lu bytes, JPEG %lu bytes)\n", (unsigned long)png_len, (unsigned long)jpg_len);
    free(png_data), free(jpg_data);
    remove(png), remove(jpg);

    /* A patch as drawing together sends it: 2000x1500 RGBA */
    size_t patch_len = (size_t)2000 * 1500 * 4, zlen = 0;
    uint8_t *raw = malloc(patch_len), *z = NULL;
    for (int y = 0; y < 1500; y++)
        memcpy(raw + (size_t)y * 8000, img_at(pic, 500, 500 + y), 8000);
    TIME("deflate a 2000x1500 patch", (free(z), z = NULL), z = zip_deflate(raw, patch_len, &zlen), (void)0);
    uint8_t *back = NULL;
    size_t back_len;
    TIME("inflate it", (free(back), back = NULL), back = zip_inflate(z, zlen, patch_len, &back_len), (void)0);
    free(back), free(z), free(raw);

    /* Drawing together: a stroke's change as a patch, and the patch put onto a friend's picture */
    img *after = img_dup(pic), *friend = img_dup(pic);
    for (int y = 0; y < H; y++)
        for (int x = y * W / H; x < y * W / H + 40 && x < W; x++)
            memset(img_at(after, x, y), 0, 3);
    patch p = { 0 };
    TIME("patch: diff a stroke on 4000x3000", patch_free(&p), patch_diff(pic, after, irect_of(pic), &p), (void)0);
    TIME("patch: apply it", (void)0, patch_apply_within(&p, friend, irect_of(friend)), (void)0);
    patch_free(&p);
    img_free(after);
    img_free(friend);

    if (fonts_count() > 0)
    {
        paint_font f;
        if (fonts_load(fonts_default_family(), 0, 0, &f) == 0)
        {
            mask *m = mask_new(W, H);
            const char *text = "The quick brown fox jumps over the lazy dog. 0123456789\nPack my box with five dozen liquor jugs!";
            TIME("text tool: 2 lines at 72 px", mask_reset(m), ptext_render(m, &f, 72, text, pt_make(50, 50), 1, 0), (void)0);
            TIME("text tool: 2 lines at 400 px", mask_reset(m), ptext_render(m, &f, 400, text, pt_make(50, 50), 0, 0), (void)0);
            mask_free(m);
        }
    }
    char line[64];
    TIME("UI text: 40 new sizes", (void)0,
         for (int i = 0; i < 40; i++) { font *f = font_get(9.0f + i * 0.75f + (float)run_ * 40.0f, 0);
                                         snprintf(line, sizeof line, "Size %d: Brushes, Shapes, Colours", i);
                                         text_draw(win, f, 10, 10 + i * 20, line, -1, COLOR(0)); }, (void)0);

    img_free(pic);
    app_free(A);
    view_free();
    surface_free(win);
    (void)argc, (void)argv;
    return 0;
}
