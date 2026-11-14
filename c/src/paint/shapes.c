/*
 * Paint (C port) - shape outlines (Rust: shapes.rs)
 *
 * Shapes are made in a unit square and stretched over the box, except the
 * rounded rectangles, whose corner radius follows the box's shorter side.
 */
#include <math.h>
#include "paint/shapes.h"

#define PI 3.14159265358979323846f
#define TAU 6.28318530717958647692f
#define FRAC_PI_2 1.57079632679489661923f
#define FRAC_PI_4 0.78539816339744830962f

static const float ARROW[7][2] = { { 0.0f, 0.25f }, { 0.6f, 0.25f }, { 0.6f, 0.0f }, { 1.0f, 0.5f },
                                   { 0.6f, 1.0f }, { 0.6f, 0.75f }, { 0.0f, 0.75f } };
static const float LIGHTNING[7][2] = { { 0.35f, 0.0f }, { 0.78f, 0.0f }, { 0.56f, 0.36f }, { 0.86f, 0.36f },
                                       { 0.22f, 1.0f }, { 0.4f, 0.55f }, { 0.12f, 0.55f } };

const char *shape_name(shape_kind k)
{
    static const char *const names[SHAPE_COUNT] = {
        "Line", "Oval", "Rectangle", "Rounded rectangle", "Triangle", "Right triangle", "Diamond",
        "Pentagon", "Hexagon", "Right arrow", "Left arrow", "Up arrow", "Down arrow", "Four-point star",
        "Five-point star", "Six-point star", "Rounded rectangular callout", "Oval callout", "Cloud callout",
        "Heart", "Lightning",
    };
    return (unsigned)k < SHAPE_COUNT ? names[k] : "";
}

/* Regular star with n points (inner radius ratio `inner`); inner == 1 gives a regular polygon */
static int star(int n, float inner, pt *out)
{
    if (inner >= 1.0f)
    {
        for (int k = 0; k < n; k++)
        {
            float t = -FRAC_PI_2 + (float)k * TAU / (float)n;
            out[k] = pt_make(cosf(t), sinf(t));
        }
        return n;
    }
    for (int k = 0; k < 2 * n; k++)
    {
        float t = -FRAC_PI_2 + (float)k * TAU / (float)(2 * n);
        float r = k % 2 == 0 ? 1.0f : inner;
        out[k] = pt_make(r * cosf(t), r * sinf(t));
    }
    return 2 * n;
}

static int heart(pt *out)
{
    for (int i = 0; i < 120; i++)
    {
        float t = (float)i / 120.0f * TAU;
        float s = sinf(t);
        float x = 16.0f * (s * s * s);
        float y = -(13.0f * cosf(t) - 5.0f * cosf(2.0f * t) - 2.0f * cosf(3.0f * t) - cosf(4.0f * t));
        out[i] = pt_make(x, y);
    }
    return 120;
}

/* Rescale points so their bounding box is the unit square */
static void normalize(pt *p, int n)
{
    float x0 = 3.40282347e38f, y0 = 3.40282347e38f, x1 = -3.40282347e38f, y1 = -3.40282347e38f;
    for (int i = 0; i < n; i++)
    {
        x0 = fminf(x0, p[i].x);
        y0 = fminf(y0, p[i].y);
        x1 = fmaxf(x1, p[i].x);
        y1 = fmaxf(y1, p[i].y);
    }
    float w = fmaxf(x1 - x0, 1e-6f), h = fmaxf(y1 - y0, 1e-6f);
    for (int i = 0; i < n; i++)
        p[i] = pt_make((p[i].x - x0) / w, (p[i].y - y0) / h);
}

static int rounded_rect(frect r, const pt *tail, pt *out)
{
    float rad = fminf(r.x1 - r.x0, r.y1 - r.y0) * 0.18f;
    const pt centres[4] = { { r.x1 - rad, r.y0 + rad }, { r.x1 - rad, r.y1 - rad }, { r.x0 + rad, r.y1 - rad }, { r.x0 + rad, r.y0 + rad } };
    const float starts[4] = { -FRAC_PI_2, 0.0f, FRAC_PI_2, 2.0f * FRAC_PI_2 };
    int n = 0;
    for (int i = 0; i < 4; i++)
    {
        /* The bottom edge runs right to left here; a callout's tail joins it. */
        if (i == 2 && tail)
            for (int k = 0; k < 3; k++)
                out[n++] = tail[k];
        for (int k = 0; k <= 10; k++)
        {
            float t = starts[i] + (float)k / 10.0f * FRAC_PI_2;
            out[n++] = pt_make(centres[i].x + cosf(t) * rad, centres[i].y + sinf(t) * rad);
        }
    }
    return n;
}

static pt oval_at(float t)
{
    return pt_make(0.5f + 0.5f * cosf(t), 0.39f + 0.39f * sinf(t));
}

/* An ellipse in the top of the box with a tail to the bottom-left corner */
static int oval_callout(pt *out)
{
    float t0 = 0.58f * PI, t1 = 0.72f * PI;
    int n = 0, tail_added = 0;
    for (int i = 0; i < 120; i++)
    {
        float t = (float)i / 120.0f * TAU;
        if (t > t0 && t < t1)
            continue;
        if (t >= t1 && !tail_added)
        {
            out[n++] = oval_at(t0);
            out[n++] = pt_make(0.12f, 1.0f);
            out[n++] = oval_at(t1);
            tail_added = 1;
        }
        out[n++] = oval_at(t);
    }
    return n;
}

int shape_outline(shape_kind k, pt a, pt b, pt *out)
{
    frect r = frect_from_two(a, b);
    float w = r.x1 - r.x0, h = r.y1 - r.y0;
    int n = 0;
    switch (k)
    {
    case SHAPE_LINE:
        out[0] = a;
        out[1] = b;
        return 2;
    case SHAPE_ROUNDED_RECTANGLE:
        return rounded_rect(r, NULL, out);
    case SHAPE_ROUNDED_CALLOUT:
    {
        frect body = { r.x0, r.y0, r.x1, r.y0 + h * 0.78f };
        const pt tail[3] = { { r.x0 + 0.40f * w, body.y1 }, { r.x0 + 0.15f * w, r.y1 }, { r.x0 + 0.26f * w, body.y1 } };
        return rounded_rect(body, tail, out);
    }
    case SHAPE_OVAL_CALLOUT:
        n = oval_callout(out);
        break;
    case SHAPE_CLOUD_CALLOUT:
        for (int i = 0; i < 180; i++)
        {
            float t = (float)i / 180.0f * TAU;
            float m = 0.86f + 0.14f * fabsf(sinf(4.5f * t));
            out[n++] = pt_make(0.5f + 0.5f * m * cosf(t), 0.36f + 0.36f * m * sinf(t));
        }
        break;
    case SHAPE_OVAL:
    {
        float c = (w + h) * 0.5f;
        int count = (int)(c < 32.0f ? 32.0f : c > 360.0f ? 360.0f : c);
        for (int i = 0; i < count; i++)
        {
            float t = (float)i / (float)count * TAU;
            out[n++] = pt_make(0.5f + 0.5f * cosf(t), 0.5f + 0.5f * sinf(t));
        }
        break;
    }
    case SHAPE_RECTANGLE:
        out[0] = pt_make(0, 0), out[1] = pt_make(1, 0), out[2] = pt_make(1, 1), out[3] = pt_make(0, 1);
        n = 4;
        break;
    case SHAPE_TRIANGLE:
        out[0] = pt_make(0.5f, 0), out[1] = pt_make(1, 1), out[2] = pt_make(0, 1);
        n = 3;
        break;
    case SHAPE_RIGHT_TRIANGLE:
        out[0] = pt_make(0, 0), out[1] = pt_make(1, 1), out[2] = pt_make(0, 1);
        n = 3;
        break;
    case SHAPE_DIAMOND:
        out[0] = pt_make(0.5f, 0), out[1] = pt_make(1, 0.5f), out[2] = pt_make(0.5f, 1), out[3] = pt_make(0, 0.5f);
        n = 4;
        break;
    case SHAPE_PENTAGON:
        n = star(5, 1.0f, out);
        normalize(out, n);
        break;
    case SHAPE_HEXAGON:
    {
        static const float hex[6][2] = { { 0.25f, 0 }, { 0.75f, 0 }, { 1, 0.5f }, { 0.75f, 1 }, { 0.25f, 1 }, { 0, 0.5f } };
        for (n = 0; n < 6; n++)
            out[n] = pt_make(hex[n][0], hex[n][1]);
        break;
    }
    case SHAPE_RIGHT_ARROW:
    case SHAPE_LEFT_ARROW:
    case SHAPE_UP_ARROW:
    case SHAPE_DOWN_ARROW:
        for (n = 0; n < 7; n++)
        {
            float x = ARROW[n][0], y = ARROW[n][1];
            out[n] = k == SHAPE_RIGHT_ARROW ? pt_make(x, y)
                   : k == SHAPE_LEFT_ARROW  ? pt_make(1.0f - x, y)
                   : k == SHAPE_UP_ARROW    ? pt_make(y, 1.0f - x)
                                            : pt_make(y, x);
        }
        break;
    case SHAPE_STAR4:
        n = star(4, 0.38f, out);
        normalize(out, n);
        break;
    case SHAPE_STAR5:
        n = star(5, 0.4f, out);
        normalize(out, n);
        break;
    case SHAPE_STAR6:
        n = star(6, 0.55f, out);
        normalize(out, n);
        break;
    case SHAPE_HEART:
        n = heart(out);
        normalize(out, n);
        break;
    case SHAPE_LIGHTNING:
    default:
        for (n = 0; n < 7; n++)
            out[n] = pt_make(LIGHTNING[n][0], LIGHTNING[n][1]);
        break;
    }
    for (int i = 0; i < n; i++)
        out[i] = pt_make(r.x0 + out[i].x * w, r.y0 + out[i].y * h);
    return n;
}

int shape_contours(shape_kind k, pt a, pt b, pt *out, int *counts)
{
    counts[0] = shape_outline(k, a, b, out);
    if (k != SHAPE_CLOUD_CALLOUT)
        return 1;
    frect r = frect_from_two(a, b);
    float w = r.x1 - r.x0, h = r.y1 - r.y0;
    static const float bubbles[2][4] = { { 0.22f, 0.83f, 0.07f, 0.06f }, { 0.1f, 0.95f, 0.045f, 0.04f } };
    for (int c = 0; c < 2; c++)
    {
        pt *o = out + (c + 1) * SHAPE_MAX_POINTS;
        for (int i = 0; i < 24; i++)
        {
            float t = (float)i / 24.0f * TAU;
            o[i] = pt_make(r.x0 + (bubbles[c][0] + bubbles[c][2] * cosf(t)) * w, r.y0 + (bubbles[c][1] + bubbles[c][3] * sinf(t)) * h);
        }
        counts[c + 1] = 24;
    }
    return 3;
}

pt shape_constrain(shape_kind k, pt a, pt b, int shift)
{
    if (!shift)
        return b;
    float dx = b.x - a.x, dy = b.y - a.y;
    if (k == SHAPE_LINE)
    {
        float angle = roundf(atan2f(dy, dx) / FRAC_PI_4) * FRAC_PI_4;
        float cx = cosf(angle), cy = sinf(angle);
        float d = dx * cx + dy * cy;
        return pt_make(a.x + cx * d, a.y + cy * d);
    }
    float side = fmaxf(fabsf(dx), fabsf(dy));
    return pt_make(a.x + copysignf(side, dx), a.y + copysignf(side, dy));
}
