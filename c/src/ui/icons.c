/*
 * Paint (C port) - classic Paint-style colour icons (Rust: icons.rs, logo.rs)
 *
 * A "pen" maps the 20x20 grid onto the icon's pixels and draws with gfx.
 * Like the Rust Pen, purely horizontal and vertical lines and the outlines
 * of rectangles are snapped to the pixel grid so they stay crisp, and
 * outlines are one device pixel per point. Each drawn icon is kept as a
 * tile of straight-alpha pixels in a small cache; a missing one is drawn
 * into the least recently used tile.
 *
 * The shape gallery's icons are stroked with the canvas's rasteriser
 * (paint/raster.h) and the app logo is paint/logo.c's, as in the Rust; the
 * colour wheel is supersampled here.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "paint/logo.h"
#include "paint/raster.h"
#include "paint/shapes.h"
#include "ui/font.h"
#include "ui/icons.h"
#include "ui/theme.h"

#define PI 3.14159265f
#define TAU 6.2831853f
#define HEX(rgb) COLOR(rgb)
#define NONE 0u

#define INK HEX(0x2B2B2B)
#define BLUE HEX(0x2B78D4)
#define DARK_BLUE HEX(0x1D5AA3)
#define LIGHT_BLUE HEX(0xC4E0F7)
#define YELLOW HEX(0xF7C948)
#define YELLOW_DARK HEX(0xB98A10)
#define FOLDER HEX(0xF4C55B)
#define FOLDER_BACK HEX(0xDDA33A)
#define PINK HEX(0xF29CB0)
#define WOOD HEX(0xF3D9A6)
#define BROWN HEX(0xC08B57)
#define BROWN_DARK HEX(0x7C5230)
#define SILVER HEX(0xC3C8CE)
#define SILVER_DARK HEX(0x7D838A)
#define GREEN HEX(0x2E9B45)
#define RED HEX(0xD64541)
#define PAPER HEX(0xFFFFFF)
#define PAPER_LINE HEX(0x9AA4AE)

typedef struct
{
    surface *s;
    float u;            /* pixels per grid unit */
    int disabled;
    color solid;        /* every part in this colour (keeping its alpha): the halo */
    int hair;
} pen;

static pen P;

/* ---- the pen (Rust's Pen) ---- */

static float px_(float x)
{
    return x * P.u;
}

static color col(color c)
{
    if (P.solid)
        return COLOR_A(P.solid, C_ALPHA(c));
    if (!P.disabled)
        return c;
    int g = (int)(0.3f * C_R(c) + 0.59f * C_G(c) + 0.11f * C_B(c));
    g = g / 2 + 110;
    return COLOR_A(RGB3(g, g, g), (int)(C_ALPHA(c) * 0.55f));
}

static float sgn(float v)
{
    return v < 0 ? -1.0f : 1.0f;
}

/* Grid points to pixels */
static void dev(const float *g, int n, float *out)
{
    for (int i = 0; i < 2 * n; i++)
        out[i] = px_(g[i]);
}

/* A polyline in pixels drawn as one joined stroke; straight ones snapped to the grid,
 * open curved ones with round ends */
static void path(const float *v, int n, color c, float width)
{
    if (n < 2)
        return;
    c = col(c);
    int straight = 1;
    for (int i = 0; i + 1 < n; i++)
        if (fabsf(v[2 * i] - v[2 * i + 2]) >= 1e-3f && fabsf(v[2 * i + 1] - v[2 * i + 3]) >= 1e-3f)
            straight = 0;
    float q[2 * 128], w;
    if (n > 128)
        n = 128;
    if (straight)
    {
        w = roundf(width * P.u);
        if (w < 1)
            w = 1;
        for (int i = 0; i < 2 * n; i++)
            q[i] = roundf(v[i] - w / 2) + w / 2;
    }
    else
    {
        w = width * P.u;
        memcpy(q, v, sizeof(float) * 2 * n);
    }
    int closed = n > 2 && fabsf(q[0] - q[2 * n - 2]) < 1e-3f && fabsf(q[1] - q[2 * n - 1]) < 1e-3f;
    if (closed)
        gfx_stroke(P.s, q, n - 1, w, c, GFX_CLOSED | GFX_MITER);
    else
        gfx_stroke(P.s, q, n, w, c, GFX_MITER | (!straight && w > 1.6f ? 0 : GFX_BUTT));
}

static void line(const float *g, int n, color c, float width)
{
    float v[2 * 32];
    dev(g, n, v);
    path(v, n, c, width);
}

static void closed(const float *g, int n, color c, float width)
{
    float v[2 * 33];
    dev(g, n, v);
    v[2 * n] = v[0];
    v[2 * n + 1] = v[1];
    path(v, n + 1, c, width);
}

static void poly(const float *g, int n, color fill, color outline)
{
    float v[2 * 32];
    dev(g, n, v);
    gfx_polygon(P.s, v, n, col(fill));
    if (outline)
        closed(g, n, outline, 1.0f);
}

/* A convex polygon on the pixel grid, its outline one joined stroke just inside the edges */
static void crisp(const float *g, int n, color fill, color outline)
{
    float b[2 * 8], cx = 0, cy = 0;
    for (int i = 0; i < n; i++)
    {
        b[2 * i] = roundf(px_(g[2 * i]));
        b[2 * i + 1] = roundf(px_(g[2 * i + 1]));
        cx += b[2 * i] / n;
        cy += b[2 * i + 1] / n;
    }
    if (C_ALPHA(fill))
        gfx_polygon(P.s, b, n, col(fill));
    if (outline)
    {
        float half = P.hair / 2.0f;
        for (int i = 0; i < n; i++)
        {
            b[2 * i] += half * sgn(cx - b[2 * i]);
            b[2 * i + 1] += half * sgn(cy - b[2 * i + 1]);
        }
        gfx_stroke(P.s, b, n, (float)P.hair, col(outline), GFX_CLOSED | GFX_MITER);
    }
}

static void rect_(float x0, float y0, float x1, float y1, color fill, color outline)
{
    float g[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
    crisp(g, 4, fill, outline);
}

/* A horizontal hairline (ruled lines on paper), on a pixel row */
static void rule(float x0, float x1, float y, color c)
{
    float w = (float)P.hair;
    float yy = roundf(px_(y) - w / 2);
    float a = roundf(px_(x0)), b = roundf(px_(x1));
    gfx_fill(P.s, rect_make((int)a, (int)yy, (int)(b - a), (int)w), col(c));
}

static void disc(float x, float y, float r, color fill)
{
    gfx_circle(P.s, px_(x), px_(y), r * P.u, col(fill));
}

static void ring(float x, float y, float r, color c, float width)
{
    gfx_ring(P.s, px_(x), px_(y), r * P.u, width * P.u, col(c));
}

/* egui's dashed line: dashes of 2 units and gaps of 1.5 along a closed outline */
static void dashed(const float *g, int n, color c)
{
    float v[2 * 30];
    dev(g, n, v);
    float w = n == 4 ? (float)P.hair : (1.1f * P.u > 1 ? 1.1f * P.u : 1);
    if (n == 4)
        for (int i = 0; i < 8; i++)
            v[i] = roundf(v[i] - w / 2) + w / 2;
    v[2 * n] = v[0];
    v[2 * n + 1] = v[1];
    c = col(c);
    float dash = 2 * P.u, gap = 1.5f * P.u, left = dash;
    int on = 1, k = 0;
    float cur[2 * 32];
    cur[0] = v[0];
    cur[1] = v[1];
    k = 1;
    for (int i = 0; i < n; i++)
    {
        float ax = v[2 * i], ay = v[2 * i + 1], bx = v[2 * i + 2], by = v[2 * i + 3];
        float len = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay)), at = 0;
        while (len - at > left)
        {
            at += left;
            float x = ax + (bx - ax) * at / len, y = ay + (by - ay) * at / len;
            if (on)
            {
                cur[2 * k] = x;
                cur[2 * k + 1] = y;
                gfx_stroke(P.s, cur, k + 1, w, c, GFX_MITER | GFX_BUTT);
            }
            on = !on;
            left = on ? dash : gap;
            cur[0] = x;
            cur[1] = y;
            k = 1;
        }
        left -= len - at;
        if (on && k < 31)
        {
            cur[2 * k] = bx;
            cur[2 * k + 1] = by;
            k++;
        }
    }
    if (on && k > 1)
        gfx_stroke(P.s, cur, k, w, c, GFX_MITER | GFX_BUTT);
}

/* Points of an arc on the grid, in pixels; returns how many */
static int arc(float *out, float cx, float cy, float r, float from, float to)
{
    int n = (int)ceilf(fabsf(to - from) / 0.15f);
    if (n < 2)
        n = 2;
    for (int i = 0; i <= n; i++)
    {
        float t = from + (to - from) * i / n;
        out[2 * i] = px_(cx + r * cosf(t));
        out[2 * i + 1] = px_(cy + r * sinf(t));
    }
    return n + 1;
}

static void arrow_head(float tx, float ty, float dx, float dy, float size, color c)
{
    float l = sqrtf(dx * dx + dy * dy);
    dx = dx / l * size * P.u;
    dy = dy / l * size * P.u;
    float nx = -dy * 0.8f, ny = dx * 0.8f;
    float v[6] = { tx + dx * 0.55f, ty + dy * 0.55f, tx - dx * 0.55f + nx, ty - dy * 0.55f + ny, tx - dx * 0.55f - nx, ty - dy * 0.55f - ny };
    gfx_polygon(P.s, v, 3, col(c));
}

/* Text centred on a grid point, its line box centred like an egui galley */
static void text(float x, float y, const char *s, float size, int style, color c)
{
    font *f = font_get(size * P.u, style);
    int w = text_width(f, s, -1);
    text_draw(P.s, f, (int)roundf(px_(x) - w / 2.0f), (int)roundf(px_(y) - font_height(f) / 2.0f), s, -1, col(c));
}

/* A rod from a to b built from parts: (start, end, half width, fill); a negative
 * half width tapers from a point to that width */
typedef struct
{
    float s0, s1, half;
    color fill;
} part;

static void rod(float ax, float ay, float bx, float by, const part *parts, int n)
{
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    float nx = -dy / len, ny = dx / len;
    for (int i = 0; i < n; i++)
    {
        const part *p = &parts[i];
        float h0 = p->half < 0 ? 0 : p->half, h1 = p->half < 0 ? -p->half : p->half;
        float g[8] = { ax + dx * p->s0 + nx * h0, ay + dy * p->s0 + ny * h0, ax + dx * p->s1 + nx * h1, ay + dy * p->s1 + ny * h1,
                       ax + dx * p->s1 - nx * h1, ay + dy * p->s1 - ny * h1, ax + dx * p->s0 - nx * h0, ay + dy * p->s0 - ny * h0 };
        poly(g, 4, p->fill, INK);
    }
}

/* The red diagonal that marks "none" (no fill, no outline) */
static void no_entry_slash(float x0, float y0, float x1, float y1)
{
    float g[4] = { x0 + 0.8f, y1 - 0.8f, x1 - 0.8f, y0 + 0.8f };
    line(g, 2, RED, 1.6f);
}

static void page(float x0, float y0, float x1, float y1)
{
    float fold = 3.5f;
    float a[10] = { x0, y0, x1 - fold, y0, x1, y0 + fold, x1, y1, x0, y1 };
    float b[6] = { x1 - fold, y0, x1 - fold, y0 + fold, x1, y0 + fold };
    crisp(a, 5, PAPER, INK);
    crisp(b, 3, HEX(0xDDE3EA), INK);
}

static void floppy(void)
{
    float body[8] = { 2.5f, 2.5f, 17.5f, 2.5f, 17.5f, 17.5f, 2.5f, 17.5f };
    crisp(body, 4, BLUE, NONE);
    /* The label stops short of the bottom edge, with blue below it like a real disk */
    rect_(5, 10, 15, 15.5f, PAPER, NONE);
    rule(6.5f, 13.5f, 12.0f, PAPER_LINE);
    rule(6.5f, 13.5f, 13.9f, PAPER_LINE);
    rect_(6, 2.5f, 14, 7, SILVER, NONE);
    rect_(11, 3.3f, 12.8f, 6.2f, DARK_BLUE, NONE);
    /* Outline last, so nothing covers it */
    crisp(body, 4, NONE, DARK_BLUE);
}

/* A kid's head and shoulders: (cx, cy) is the middle of the shoulders' base */
static void kid(float cx, float cy, float r, color fill)
{
    float v[2 * 32];
    int n = arc(v, cx, cy, r, PI, TAU);
    v[2 * n] = px_(cx);
    v[2 * n + 1] = px_(cy);
    gfx_polygon(P.s, v, n + 1, col(fill));
    float w = 0.8f * P.u > P.hair ? 0.8f * P.u : (float)P.hair;
    gfx_stroke(P.s, v, n, w, col(INK), GFX_MITER | GFX_BUTT);
    float base[4] = { cx - r, cy, cx + r, cy };
    line(base, 2, INK, 0.8f);
    float hy = cy - r - r * 0.62f;
    disc(cx, hy, r * 0.6f, col(fill));     /* (the Rust fades this one twice when disabled) */
    ring(cx, hy, r * 0.6f, INK, 0.8f);
}

/* A door, with an arrow going in (towards the door) or out */
static void door(float dx0, float dx1, float ax, float ay, float bx, float by)
{
    rect_(dx0, 2.5f, dx1, 18, HEX(0xE6C08A), BROWN_DARK);
    rect_(dx0 + 1.5f, 4, dx1 - 1.5f, 9.5f, HEX(0xF3D9A6), BROWN_DARK);
    disc(dx1 - 1.8f, 11.5f, 0.8f, BROWN_DARK);
    float g[4] = { ax, ay, bx - sgn(bx - ax) * 2, by };
    line(g, 2, GREEN, 2.2f);
    arrow_head(px_(bx), px_(by), bx - ax, by - ay, 5.0f, GREEN);
}

static void magnifier(void)
{
    /* Handle first, starting under the rim; the lens and rim go on top */
    float c = 8.5f, r = 5.6f, k = 0.70710678f;
    float start = r + 0.4f, collar = r + 2, end = r + 7, split = (collar - start) / (end - start);
    part parts[2] = { { 0, split, 1.15f, SILVER }, { split, 1, 1.6f, BROWN } };
    rod(c + start * k, c + start * k, c + end * k, c + end * k, parts, 2);
    disc(c, c, r, HEX(0xD9ECFB));
    float v[2 * 16];
    int n = arc(v, c, c, r - 1.6f, PI * 1.1f, PI * 1.45f);
    path(v, n, PAPER, 1.0f);
    ring(c, c, r, INK, 1.4f);
}

static void arrow_pointer(float x, float y)
{
    float p[14] = { x, y, x, y + 9, x + 2.2f, y + 6.9f, x + 3.8f, y + 10.2f, x + 5.2f, y + 9.5f, x + 3.6f, y + 6.2f, x + 6.6f, y + 6.0f };
    float a[6] = { p[0], p[1], p[2], p[3], p[12], p[13] };
    poly(a, 3, PAPER, NONE);
    poly(p + 4, 4, PAPER, NONE);
    closed(p, 7, INK, 1.0f);
}

static void picture(float x0, float y0, float x1, float y1)
{
    rect_(x0, y0, x1, y1, LIGHT_BLUE, INK);
    float w = x1 - x0, h = y1 - y0;
    float g[6] = { x0 + 0.5f, y1 - 0.5f, x0 + w * 0.4f, y0 + h * 0.45f, x0 + w * 0.7f, y1 - 0.5f };
    poly(g, 3, GREEN, NONE);
    disc(x0 + w * 0.75f, y0 + h * 0.3f, h * 0.12f, YELLOW);
}

/* Gallery icons: the outline stroked with the canvas's own rasteriser (round joins,
 * exact coverage), straight edges on pixel rows: 1 px lines at 16 px, 2 at 32 */
static void render_shape(int kind, int px, int tint)
{
    float s = px / 16.0f, radius = 0.5f * s;
    pt a = pt_make(1.5f * s, 1.5f * s), b = pt_make(14.5f * s, 14.5f * s);
    mask *m = mask_new(px, px);
    if (!m)
        return;
    if (kind == SHAPE_LINE)
        mask_capsule(m, a, b, radius, 1);
    else
    {
        static pt outline[SHAPE_MAX_CONTOURS * SHAPE_MAX_POINTS];
        int counts[SHAPE_MAX_CONTOURS];
        int n = shape_contours((shape_kind)kind, a, b, outline, counts);
        for (int i = 0; i < n; i++)
            mask_stroke_path(m, outline + i * SHAPE_MAX_POINTS, counts[i], 1, radius);
    }
    for (int y = 0; y < px; y++)
        for (int x = 0; x < px; x++)
        {
            float cov = mask_get(m, x, y);
            if (cov > 0)
                P.s->px[y * P.s->stride + x] = COLOR_A(INK, (int)(cov * tint + 0.5f));
        }
    mask_free(m);
}

/* The app icon (paint/logo.c), drawn the Rust way: at 512 and scaled down */
static void render_logo(int px)
{
    img *im = logo_render(px);
    if (!im)
        return;
    for (int i = 0; i < px * px; i++)
    {
        const uint8_t *q = im->px + (size_t)i * 4;
        P.s->px[(i / px) * P.s->stride + i % px] = COLOR_A(RGB3(q[0], q[1], q[2]), q[3]);
    }
    img_free(im);
}

/* sRGB encoding of a linear channel (egui's Hsva is linear) */
static int srgb(float v)
{
    v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
    return (int)(v * 255 + 0.5f);
}

/* Hue around the circle, saturation towards the rim, with a thin grey edge */
static void render_color_wheel(int px, int tint)
{
    float s = (float)px, c = s / 2, r = s * 0.43f;
    for (int y = 0; y < px; y++)
        for (int x = 0; x < px; x++)
        {
            float acc[3] = { 0, 0, 0 };
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                {
                    float dx = x + (sx + 0.5f) / 4 - c, dy = y + (sy + 0.5f) / 4 - c, d = sqrtf(dx * dx + dy * dy);
                    if (d > r)
                        continue;
                    float hue = fmodf((atan2f(dy, dx) + PI / 2) / TAU + 1, 1) * 6, sat = powf(d / r, 0.8f) * 0.95f;
                    for (int k = 0; k < 3; k++)
                    {
                        /* HSV with value 1: each channel by its distance from the hue */
                        float h = fmodf(hue + (k == 0 ? 5 : k == 1 ? 3 : 1), 6);
                        float f = 1 - sat * fmaxf(0, fminf(fminf(h, 4 - h), 1));
                        acc[k] += srgb(f);
                    }
                    hits++;
                }
            if (hits)
                P.s->px[y * P.s->stride + x] = COLOR_A(RGB3((int)(acc[0] / hits), (int)(acc[1] / hits), (int)(acc[2] / hits)), hits * tint / 16);
        }
    float w = s / 64 > 0.5f ? s / 64 : 0.5f;
    gfx_ring(P.s, c, c, r - 0.4f, 2 * w, COLOR_A(0x6E757E, (int)(0.85f * tint)));
}

/* ---- every icon ---- */

static void render(int icon, uint32_t arg, int px)
{
    P.u = px / 20.0f;
    float u = P.u;
    switch (icon)
    {
    case ICON_PASTE:
        rect_(3.5f, 2.5f, 16.5f, 19, BROWN, BROWN_DARK);
        rect_(5.5f, 5, 14.5f, 17.5f, PAPER, PAPER_LINE);
        for (int i = 0; i < 3; i++)
            rule(7, 13, 8.5f + 2.5f * i, PAPER_LINE);
        rect_(7, 1, 13, 4.4f, SILVER, SILVER_DARK);
        rect_(9, 1.6f, 11, 2.6f, SILVER_DARK, NONE);
        break;
    case ICON_CUT:
    {
        float a[4] = { 5.5f, 1.5f, 12, 12.5f }, b[4] = { 14.5f, 1.5f, 8, 12.5f };
        line(a, 2, SILVER_DARK, 1.7f);
        line(b, 2, SILVER_DARK, 1.7f);
        disc(10, 8.8f, 1, INK);
        ring(6.5f, 15.2f, 2.7f, BLUE, 1.7f);
        ring(13.5f, 15.2f, 2.7f, BLUE, 1.7f);
        break;
    }
    case ICON_COPY:
        page(2.5f, 1.5f, 12.5f, 13.5f);
        page(7.5f, 6, 17.5f, 18.5f);
        for (int i = 0; i < 3; i++)
            rule(9.5f, 15, 10.5f + 2.5f * i, BLUE);
        break;
    case ICON_SELECT:
    {
        float g[8] = { 1.5f, 2.5f, 18, 2.5f, 18, 14.5f, 1.5f, 14.5f };
        dashed(g, 4, INK);
        arrow_pointer(10.5f, 8.5f);
        break;
    }
    case ICON_FREE_SELECT:
    {
        float g[2 * 28];
        for (int i = 0; i < 28; i++)
        {
            float t = i / 28.0f * TAU, r = 1 + 0.12f * sinf(3 * t + 0.6f);
            g[2 * i] = 9.5f + 7.8f * r * cosf(t);
            g[2 * i + 1] = 8 + 5.6f * r * sinf(t);
        }
        dashed(g, 28, INK);
        arrow_pointer(10.5f, 8.5f);
        break;
    }
    case ICON_SELECT_ALL:
    {
        rect_(3, 3.5f, 17, 16.5f, LIGHT_BLUE, NONE);
        float g[8] = { 3, 3.5f, 17, 3.5f, 17, 16.5f, 3, 16.5f };
        dashed(g, 4, INK);
        break;
    }
    case ICON_DELETE:
    {
        float a[4] = { 4.5f, 4.5f, 15.5f, 15.5f }, b[4] = { 15.5f, 4.5f, 4.5f, 15.5f };
        line(a, 2, RED, 2.6f);
        line(b, 2, RED, 2.6f);
        break;
    }
    case ICON_CROP:
    {
        rect_(6, 6, 14, 14, LIGHT_BLUE, NONE);
        float a[6] = { 6, 2, 6, 14, 18, 14 }, b[6] = { 2, 6, 14, 6, 14, 18 };
        line(a, 3, INK, 1.6f);
        line(b, 3, INK, 1.6f);
        break;
    }
    case ICON_RESIZE:
    {
        float g[8] = { 2.5f, 2.5f, 17.5f, 2.5f, 17.5f, 17.5f, 2.5f, 17.5f };
        dashed(g, 4, SILVER_DARK);
        picture(2.5f, 9.5f, 10.5f, 17.5f);
        float a[4] = { 9.5f, 10.5f, 16, 4 }, b[6] = { 11.5f, 4, 16, 4, 16, 8.5f };
        line(a, 2, INK, 1.3f);
        line(b, 3, INK, 1.3f);
        break;
    }
    case ICON_ROTATE:
    case ICON_ROTATE_LEFT:
    {
        int right = icon == ICON_ROTATE;
        if (right)
            picture(7.5f, 8, 17.5f, 17.5f);
        else
            picture(2.5f, 8, 12.5f, 17.5f);
        float v[2 * 32];
        int n = right ? arc(v, 10, 10, 7, PI, PI * 1.62f) : arc(v, 10, 10, 7, PI * 2, PI * 1.38f);
        path(v, n, BLUE, 1.8f);
        arrow_head(v[2 * n - 2], v[2 * n - 1], right ? 1 : -1, 0.25f, 3.4f, BLUE);
        break;
    }
    case ICON_ROTATE_180:
    {
        picture(5, 6.5f, 15, 15.5f);
        float v[2 * 32];
        int n = arc(v, 10, 11, 8, PI * 0.95f, PI * 2.05f);
        path(v, n, BLUE, 1.6f);
        arrow_head(v[2 * n - 2], v[2 * n - 1], 0.1f, 1, 3.2f, BLUE);
        break;
    }
    case ICON_FLIP_VERTICAL:
    {
        float a[6] = { 3, 8.5f, 17, 8.5f, 10, 2 }, b[6] = { 3, 11.5f, 17, 11.5f, 10, 18 };
        poly(a, 3, LIGHT_BLUE, INK);
        poly(b, 3, BLUE, INK);
        break;
    }
    case ICON_FLIP_HORIZONTAL:
    {
        float a[6] = { 8.5f, 3, 8.5f, 17, 2, 10 }, b[6] = { 11.5f, 3, 11.5f, 17, 18, 10 };
        poly(a, 3, LIGHT_BLUE, INK);
        poly(b, 3, BLUE, INK);
        break;
    }
    case ICON_PENCIL:
    {
        part p[5] = { { 0, 0.1f, -0.7f, INK }, { 0.1f, 0.28f, -2.3f, WOOD }, { 0.28f, 0.76f, 2.3f, YELLOW }, { 0.76f, 0.85f, 2.3f, SILVER },
                      { 0.85f, 1, 2.3f, PINK } };
        rod(2.5f, 17.5f, 17.5f, 2.5f, p, 5);
        break;
    }
    case ICON_FILL:
    {
        /* A bucket tipped to the right, its mouth facing down-right, paint pouring from the lip */
        float a[8] = { 7.2f, 1.2f, 17.4f, 8.6f, 14.1f, 12.5f, 4.4f, 4.4f }, b[8] = { 4.4f, 4.4f, 14.1f, 12.5f, 10.8f, 16.4f, 1.6f, 7.6f };
        float c[8] = { 17.4f, 8.6f, 10.8f, 16.4f, 9.9f, 15.3f, 16.3f, 7.8f }, o[8] = { 7.2f, 1.2f, 17.4f, 8.6f, 10.8f, 16.4f, 1.6f, 7.6f };
        float d[8] = { 16, 9.8f, 18.3f, 9.2f, 18.9f, 15.6f, 17.1f, 15.6f };
        poly(a, 4, HEX(0xE9ECEF), NONE);
        poly(b, 4, HEX(0xC2C8CF), NONE);
        poly(c, 4, BLUE, NONE);
        closed(o, 4, INK, 1);
        float v[2 * 16];
        int n = arc(v, 6.6f, 8.6f, 5.6f, PI * 1.08f, PI * 1.72f);
        path(v, n, INK, 1);
        poly(d, 4, BLUE, NONE);
        disc(17.9f, 17, 2, BLUE);
        break;
    }
    case ICON_TEXT:
        text(10, 10.3f, "A", 25, FONT_SERIF | FONT_BOLD, INK);
        break;
    case ICON_ERASER:
    {
        part p[2] = { { 0, 0.5f, 3.6f, PINK }, { 0.5f, 1, 3.6f, HEX(0x6FA8DC) } };
        rod(3, 16, 16.5f, 2.5f, p, 2);
        break;
    }
    case ICON_PICKER:
    {
        part p[3] = { { 0, 0.08f, -0.6f, INK }, { 0.08f, 0.62f, 1.4f, LIGHT_BLUE }, { 0.62f, 0.72f, 2.6f, SILVER } };
        rod(2.5f, 17.5f, 15.5f, 4.5f, p, 3);
        float g[4] = { 13.2f, 6.8f, 15.8f, 4.2f };
        line(g, 2, INK, 4.4f);
        break;
    }
    case ICON_MAGNIFIER:
        magnifier();
        break;
    case ICON_BRUSHES:
    {
        part p[2] = { { 0, 0.62f, 1.3f, BROWN }, { 0.62f, 0.86f, 1.8f, SILVER } };
        rod(18, 2, 9, 11, p, 2);
        float a[8] = { 8.2f, 9.4f, 10.6f, 11.8f, 8, 15.4f, 4.4f, 15.6f }, b[6] = { 4.4f, 15.6f, 8, 15.4f, 3, 18.4f };
        poly(a, 4, INK, NONE);
        poly(b, 3, BLUE, NONE);
        break;
    }
    case ICON_SIZE:
    {
        float rows[4][2] = { { 3.5f, 1 }, { 7.5f, 1.8f }, { 12, 2.8f }, { 16.8f, 4 } };
        for (int i = 0; i < 4; i++)
            rect_(2.5f, rows[i][0] - rows[i][1] / 2, 17.5f, rows[i][0] + rows[i][1] / 2, INK, NONE);
        break;
    }
    case ICON_EDIT_COLORS:
        render_color_wheel(px, P.disabled ? 90 : 255);
        break;
    case ICON_OUTLINE:
        if (arg)
        {
            /* A thin square in the outline colour; very light colours get a grey edge outside it */
            rect outer = rect_make((int)roundf(2.5f * u), (int)roundf(3 * u), 0, 0);
            outer.w = (int)roundf(17.5f * u) - outer.x;
            outer.h = (int)roundf(15 * u) - outer.y;
            float luma = 0.299f * C_R(arg) + 0.587f * C_G(arg) + 0.114f * C_B(arg);
            if (luma > 200)
            {
                gfx_frame(P.s, outer, P.hair, col(SILVER_DARK));
                outer = rect_inset(outer, P.hair);
            }
            gfx_frame(P.s, outer, P.hair, col(arg));
        }
        else
        {
            rect_(2.5f, 3, 17.5f, 15, PAPER, SILVER_DARK);
            no_entry_slash(2.5f, 3, 17.5f, 15);
        }
        break;
    case ICON_FILL_SHAPE:
        rect_(2.5f, 3, 17.5f, 15, arg ? arg : PAPER, INK);
        if (!arg)
            no_entry_slash(2.5f, 3, 17.5f, 15);
        break;
    case ICON_ZOOM_IN:
    case ICON_ZOOM_OUT:
    {
        magnifier();
        float a[4] = { 6, 8.5f, 11, 8.5f }, b[4] = { 8.5f, 6, 8.5f, 11 };
        line(a, 2, INK, 1.5f);
        if (icon == ICON_ZOOM_IN)
            line(b, 2, INK, 1.5f);
        break;
    }
    case ICON_ZOOM_100:
        magnifier();
        text(8.5f, 8.6f, "100", 4.6f, FONT_REGULAR, INK);
        break;
    case ICON_FULL_SCREEN:
    {
        rect_(1.5f, 2.5f, 18.5f, 14, INK, NONE);
        rect_(2.8f, 3.8f, 17.2f, 12.7f, LIGHT_BLUE, NONE);
        float g[6] = { 2.8f, 12.7f, 8, 7, 12, 12.7f };
        poly(g, 3, GREEN, NONE);
        rect_(8.5f, 14, 11.5f, 16.5f, SILVER_DARK, NONE);
        rect_(5.5f, 16.5f, 14.5f, 18, INK, NONE);
        break;
    }
    case ICON_NEW:
        page(4, 1.5f, 16.5f, 18.5f);
        disc(5, 4, 2.2f, YELLOW);
        break;
    case ICON_OPEN:
    {
        float a[12] = { 1.5f, 4, 7.5f, 4, 9, 5.5f, 18, 5.5f, 18, 16.5f, 1.5f, 16.5f }, b[8] = { 1.5f, 16.5f, 4, 8.5f, 19.5f, 8.5f, 17, 16.5f };
        poly(a, 6, FOLDER_BACK, YELLOW_DARK);
        poly(b, 4, FOLDER, YELLOW_DARK);
        break;
    }
    case ICON_SAVE:
    case ICON_FILE_SIZE:
        floppy();
        break;
    case ICON_SAVE_AS:
    {
        floppy();
        part p[3] = { { 0, 0.18f, -1.8f, WOOD }, { 0.18f, 0.8f, 1.8f, YELLOW }, { 0.8f, 1, 1.8f, PINK } };
        rod(9.5f, 18.5f, 19, 9, p, 3);
        break;
    }
    case ICON_PROPERTIES:
    {
        page(3, 1.5f, 15.5f, 18.5f);
        for (int i = 0; i < 3; i++)
            rule(5.5f, 12.5f, 6.5f + 2.5f * i, PAPER_LINE);
        disc(14.5f, 14.5f, 4.2f, GREEN);
        float g[6] = { 12.6f, 14.6f, 14, 16, 16.6f, 13.2f };
        line(g, 3, PAPER, 1.3f);
        break;
    }
    case ICON_TOGETHER:
        kid(6.8f, 18.5f, 5.2f, HEX(0xF4A62A));
        kid(13.6f, 18.5f, 5.6f, HEX(0x4C9BE8));
        break;
    case ICON_JOIN_ROOM:
        door(10.5f, 18, 1, 11, 12.5f, 11);
        break;
    case ICON_LEAVE_ROOM:
        door(2, 9.5f, 7.5f, 11, 19, 11);
        break;
    case ICON_ABOUT:
    case ICON_HELP:
        disc(10, 10, 8.5f, BLUE);
        text(10, 10.2f, icon == ICON_ABOUT ? "i" : "?", 13, FONT_SERIF | FONT_BOLD, PAPER);
        break;
    case ICON_EXIT:
    {
        rect_(3, 2, 12, 18, HEX(0xE8EBEF), INK);
        disc(10, 10.5f, 0.9f, INK);
        float g[4] = { 9.5f, 10, 17, 10 };
        line(g, 2, RED, 1.8f);
        arrow_head(px_(18), px_(10), 1, 0, 3.6f, RED);
        break;
    }
    case ICON_UNDO:
    case ICON_REDO:
    {
        float v[2 * 32];
        v[0] = px_(15.5f);
        v[1] = px_(15.5f);
        int n = 1 + arc(v + 2, 10, 10, 5.5f, 0, -PI);
        if (icon == ICON_REDO)
            for (int i = 0; i < n; i++)
                v[2 * i] = px_(20) - v[2 * i];
        path(v, n, DARK_BLUE, 1.9f);
        arrow_head(v[2 * n - 2], v[2 * n - 1] + 1.3f * u, 0, 1, 4.0f, DARK_BLUE);
        break;
    }
    case ICON_COLLAPSE:
    {
        float g[6] = { 6, 12, 10, 8, 14, 12 };
        line(g, 3, INK, 1.3f);
        break;
    }
    case ICON_DROPDOWN:
    case ICON_SCROLL_DOWN:
    case ICON_SCROLL_UP:
    {
        float d[6] = { 6.5f, 8.5f, 13.5f, 8.5f, 10, 12.5f }, sd[6] = { 6.5f, 8, 13.5f, 8, 10, 12 }, su[6] = { 6.5f, 12, 13.5f, 12, 10, 8 };
        poly(icon == ICON_DROPDOWN ? d : icon == ICON_SCROLL_DOWN ? sd : su, 3, INK, NONE);
        break;
    }
    case ICON_GALLERY_MORE:
    {
        float a[4] = { 6.5f, 6.5f, 13.5f, 6.5f }, b[6] = { 6.5f, 9.5f, 13.5f, 9.5f, 10, 13.5f };
        line(a, 2, INK, 1.2f);
        poly(b, 3, INK, NONE);
        break;
    }
    case ICON_CHECK:
    {
        float g[6] = { 4.5f, 10.5f, 8.2f, 14, 15.5f, 6 };
        line(g, 3, INK, 1.7f);
        break;
    }
    case ICON_CURSOR_POS:
    {
        float a[4] = { 10, 1.5f, 10, 18.5f }, b[4] = { 1.5f, 10, 18.5f, 10 };
        line(a, 2, INK, 1);
        line(b, 2, INK, 1);
        ring(10, 10, 4, INK, 1);
        break;
    }
    case ICON_SELECTION_SIZE:
    {
        float g[8] = { 2.5f, 4, 17.5f, 4, 17.5f, 16, 2.5f, 16 };
        dashed(g, 4, INK);
        break;
    }
    case ICON_CANVAS_SIZE:
        rect_(2.5f, 3.5f, 15, 15, PAPER, INK);
        rect_(14, 14, 18, 18, PAPER, INK);
        break;
    case ICON_OPAQUE:
    case ICON_TRANSPARENT:
        if (icon == ICON_OPAQUE)
            rect_(2, 2, 18, 18, PAPER, SILVER_DARK);
        else
        {
            for (int i = 0; i < 4; i++)
            {
                float x = 2 + 8 * (i % 2), y = 2 + 8 * (i / 2);
                rect_(x, y, x + 8, y + 8, i == 0 || i == 3 ? HEX(0xD4D7DB) : PAPER, NONE);
            }
            float g[8] = { 2, 2, 18, 2, 18, 18, 2, 18 };
            closed(g, 4, SILVER_DARK, 1);
        }
        text(10, 10.5f, "A", 15, FONT_SERIF | FONT_BOLD, BLUE);
        break;
    case ICON_BOLD:
        text(10, 10, "B", 17, FONT_SERIF | FONT_BOLD, INK);
        break;
    case ICON_ITALIC:
        text(10, 10, "I", 17, FONT_SERIF, INK);
        break;
    case ICON_UNDERLINE:
    {
        text(10, 8.5f, "U", 15, FONT_SERIF, INK);
        float g[4] = { 5, 17, 15, 17 };
        line(g, 2, INK, 1.2f);
        break;
    }
    case ICON_STRIKE:
    {
        text(10, 10, "abc", 12, FONT_SERIF, INK);
        float g[4] = { 2.5f, 10.8f, 17.5f, 10.8f };
        line(g, 2, RED, 1.2f);
        break;
    }
    case ICON_FILE_TYPE:
    {
        static const char *const labels[5] = { "PNG", "JPG", "BMP", "GIF", "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" };
        static const color bands[5] = { BLUE, HEX(0xE07B22), GREEN, HEX(0x8E44AD), SILVER_DARK };
        int k = arg < 5 ? (int)arg : FILE_KIND_OTHER;
        page(3.5f, 1.5f, 16.5f, 18.5f);
        rect_(3.5f, 10, 16.5f, 15.5f, bands[k], NONE);
        text(10, 12.8f, labels[k], 4.6f, FONT_REGULAR, PAPER);
        break;
    }
    case ICON_SHAPE:
        render_shape(arg < SHAPE_COUNT ? (int)arg : 0, px, P.disabled ? 90 : 255);
        break;
    case ICON_LOGO:
        render_logo(px);
        break;
    }
}

/* ---- the tile cache ---- */

#define TILES 64

typedef struct
{
    int icon, px, flags, hair;
    uint32_t arg;
    unsigned used;
    uint32_t *pixels;
    int cap;
} tile;

static tile tiles[TILES];
static unsigned ticks;

enum
{
    T_DISABLED = 1,
    T_HALO = 2,
};

static void render_into(uint32_t *pixels, int side, int icon, uint32_t arg, int px, int flags)
{
    surface s = { pixels, side, side, side, { 0, 0, side, side }, 0, 0 };
    memset(pixels, 0, (size_t)side * (size_t)side * 4);
    P.s = &s;
    P.hair = HAIR();
    P.disabled = flags & T_DISABLED;
    P.solid = 0;
    if (!(flags & T_HALO))
    {
        render(icon, arg, px);
        return;
    }
    /* Eight white copies a pixel out, then the icon itself */
    static const int offs[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    for (int i = 0; i <= 8; i++)
    {
        int dx = i < 8 ? offs[i][0] : 0, dy = i < 8 ? offs[i][1] : 0;
        s.ox = -1 - dx;
        s.oy = -1 - dy;
        s.clip = rect_make(s.ox, s.oy, side, side);
        P.solid = i < 8 ? COLOR(0xFFFFFF) : 0;
        render(icon, arg, px);
    }
}

static tile *tile_get(int icon, uint32_t arg, int px, int flags)
{
    int hair = HAIR();
    tile *spare = &tiles[0];
    for (int i = 0; i < TILES; i++)
    {
        tile *t = &tiles[i];
        if (t->pixels && t->icon == icon && t->arg == arg && t->px == px && t->flags == flags && t->hair == hair)
        {
            t->used = ++ticks;
            return t;
        }
        if (t->used < spare->used)
            spare = t;
    }
    int side = flags & T_HALO ? px + 2 : px;
    if (side * side > spare->cap)
    {
        free(spare->pixels);
        spare->cap = side * side;
        spare->pixels = malloc((size_t)spare->cap * 4);
    }
    spare->icon = icon;
    spare->arg = arg;
    spare->px = px;
    spare->flags = flags;
    spare->hair = hair;
    spare->used = ++ticks;
    render_into(spare->pixels, side, icon, arg, px, flags);
    return spare;
}

void icon_draw(surface *dst, int icon, uint32_t arg, int x, int y, int px, int enabled)
{
    if (px < 1 || icon < 0 || icon >= ICON_COUNT)
        return;
    tile *t = tile_get(icon, arg, px, enabled ? 0 : T_DISABLED);
    gfx_blend(dst, x, y, t->pixels, px, px, px);
}

void icon_at(surface *dst, int icon, uint32_t arg, float cx, float cy, int px, int enabled)
{
    icon_draw(dst, icon, arg, (int)roundf(cx - px / 2.0f), (int)roundf(cy - px / 2.0f), px, enabled);
}

void icon_halo(surface *dst, int icon, float cx, float cy, int px)
{
    if (px < 1)
        return;
    tile *t = tile_get(icon, 0, px, T_HALO);
    gfx_blend(dst, (int)roundf(cx - px / 2.0f) - 1, (int)roundf(cy - px / 2.0f) - 1, t->pixels, px + 2, px + 2, px + 2);
}
