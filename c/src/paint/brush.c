/*
 * Paint (C port) - the nine brushes (Rust: brush.rs)
 */
#include <math.h>
#include "paint/brush.h"

#define FRAC_1_SQRT_2 0.70710678118654752440f
#define TAU 6.28318530717958647692f

const char *brush_name(brush_kind k)
{
    static const char *const names[BRUSH_COUNT] = {
        "Brush", "Calligraphy brush 1", "Calligraphy brush 2", "Airbrush", "Oil brush",
        "Crayon", "Marker", "Natural pencil", "Watercolor brush",
    };
    return (unsigned)k < BRUSH_COUNT ? names[k] : "";
}

float brush_opacity(brush_kind k)
{
    switch (k)
    {
    case BRUSH_MARKER: return 0.5f;
    case BRUSH_WATERCOLOR: return 0.45f;
    case BRUSH_NATURAL_PENCIL: return 0.9f;
    default: return 1.0f;
    }
}

/* xorshift64*, mapped to [0, 1) */
static float next(uint64_t *state)
{
    *state ^= *state >> 12;
    *state ^= *state << 25;
    *state ^= *state >> 27;
    return (float)((*state * 0x2545F4914F6CDD1Dull) >> 40) / (float)(1u << 24);
}

/* Stable per-pixel noise in [0, 1] */
static float hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u ^ (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)(h ^ (h >> 16)) / (float)UINT32_MAX;
}

static float hash1(int i)
{
    return hash2(i, 0x5bd1);
}

/* Rust's `as i32` for the values the brushes see */
static int f2i(float v)
{
    if (v != v)
        return 0;
    if (v >= 2147483647.0f)
        return 2147483647;
    if (v <= -2147483648.0f)
        return -2147483647 - 1;
    return (int)v;
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

irect brush_spray(mask *m, pt c, float size, uint64_t *rng, int n)
{
    float radius = fmaxf(size * 0.5f, 2.0f);
    for (int i = 0; i < n; i++)
    {
        float t = next(rng) * TAU;
        float d = sqrtf(next(rng)) * radius;
        mask_max_at(m, f2i(floorf(c.x + cosf(t) * d)), f2i(floorf(c.y + sinf(t) * d)), 1.0f);
    }
    return irect_intersect(irect_around(c.x, c.y, c.x, c.y, radius + 1.0f), mask_bounds(m));
}

static float oil(int x, int y, float d, float perp, void *user)
{
    float r = *(const float *)user;
    float edge = clamp01(r + 0.5f - d);
    float bristle = hash1(f2i(floorf((perp + r) * 0.8f)));
    return edge * (0.55f + 0.45f * bristle);
}

static float crayon(int x, int y, float d, float perp, void *user)
{
    float r = *(const float *)user;
    return clamp01(r + 0.5f - d) * (hash2(x, y) > 0.38f ? 0.95f : 0.2f);
}

static float pencil(int x, int y, float d, float perp, void *user)
{
    float r = *(const float *)user;
    return clamp01(r + 0.5f - d) * (0.45f + 0.55f * hash2(x, y));
}

static float watercolor(int x, int y, float d, float perp, void *user)
{
    float r = *(const float *)user;
    float soft = powf(clamp01(1.0f - d / r), 0.6f);
    /* Rust's `/` on i32 truncates toward zero, as C's does */
    return soft * (0.75f + 0.25f * hash2(x / 3, y / 3));
}

irect brush_segment(brush_kind k, mask *m, pt a, pt b, float size, uint64_t *rng)
{
    float r = fmaxf(size * 0.5f, 0.5f);
    switch (k)
    {
    case BRUSH_CALLIGRAPHY1:
    case BRUSH_CALLIGRAPHY2:
    {
        float s = k == BRUSH_CALLIGRAPHY1 ? -1.0f : 1.0f;
        float nx = FRAC_1_SQRT_2 * r, ny = s * FRAC_1_SQRT_2 * r;
        pt a0 = { a.x - nx, a.y - ny }, a1 = { a.x + nx, a.y + ny };
        pt b0 = { b.x - nx, b.y - ny }, b1 = { b.x + nx, b.y + ny };
        pt quad[4] = { a0, a1, b1, b0 };
        irect q = mask_fill_polygon(m, quad, 4, 1.0f);
        q = irect_union(q, mask_capsule(m, a0, a1, 0.7f, 1.0f));
        return irect_union(q, mask_capsule(m, b0, b1, 0.7f, 1.0f));
    }
    case BRUSH_AIRBRUSH:
    {
        float n = size * 1.5f;
        n = n < 4.0f ? 4.0f : n > 300.0f ? 300.0f : n;
        return brush_spray(m, b, size, rng, (int)n);
    }
    case BRUSH_OIL:
        return mask_sweep(m, a, b, r, oil, &r);
    case BRUSH_CRAYON:
        return mask_sweep(m, a, b, r, crayon, &r);
    case BRUSH_NATURAL_PENCIL:
    {
        float pr = fmaxf(size * 0.3f, 0.6f);
        return mask_sweep(m, a, b, pr, pencil, &pr);
    }
    case BRUSH_WATERCOLOR:
        return mask_sweep(m, a, b, r, watercolor, &r);
    default:            /* the plain brush and the marker */
        return mask_capsule(m, a, b, r, 1.0f);
    }
}
