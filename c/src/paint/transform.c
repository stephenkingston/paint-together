/*
 * Paint (C port) - rotate, flip and resize (Rust: imageops::rotate90 ... resize)
 *
 * The resize matches imageops::resize sample for sample: a vertical pass into
 * f32 and a horizontal pass that clamps and rounds. The image crate keeps the
 * whole vertical pass as an f32 picture (16 bytes a pixel); since each output
 * row needs only its own row of that pass, it is done here a row at a time.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "paint/transform.h"

static img *blank(int w, int h)
{
    return img_new(w, h, (rgba){ 0, 0, 0, 0 });
}

img *img_rotate90(const img *src)
{
    img *out = blank(src->h, src->w);
    if (out)
        for (int y = 0; y < src->h; y++)
            for (int x = 0; x < src->w; x++)
                memcpy(img_at(out, src->h - 1 - y, x), img_at(src, x, y), 4);
    return out;
}

img *img_rotate180(const img *src)
{
    img *out = blank(src->w, src->h);
    if (out)
        for (int y = 0; y < src->h; y++)
            for (int x = 0; x < src->w; x++)
                memcpy(img_at(out, src->w - 1 - x, src->h - 1 - y), img_at(src, x, y), 4);
    return out;
}

img *img_rotate270(const img *src)
{
    img *out = blank(src->h, src->w);
    if (out)
        for (int y = 0; y < src->h; y++)
            for (int x = 0; x < src->w; x++)
                memcpy(img_at(out, y, src->w - 1 - x), img_at(src, x, y), 4);
    return out;
}

img *img_flip_horizontal(const img *src)
{
    img *out = blank(src->w, src->h);
    if (out)
        for (int y = 0; y < src->h; y++)
            for (int x = 0; x < src->w; x++)
                memcpy(img_at(out, src->w - 1 - x, y), img_at(src, x, y), 4);
    return out;
}

img *img_flip_vertical(const img *src)
{
    img *out = blank(src->w, src->h);
    if (out)
        for (int y = 0; y < src->h; y++)
            memcpy(img_at(out, 0, src->h - 1 - y), img_at(src, 0, y), (size_t)src->w * 4);
    return out;
}

#define PI_F 3.14159265358979323846f

static float sinc(float t)
{
    float a = t * PI_F;
    return t == 0.0f ? 1.0f : sinf(a) / a;
}

static float kernel(resize_filter f, float x)
{
    if (f == RESIZE_LANCZOS3)
        return fabsf(x) < 3.0f ? sinc(x) * sinc(x / 3.0f) : 0.0f;
    return fabsf(x) < 1.0f ? 1.0f - fabsf(x) : 0.0f;
}

/* The taps for one output sample along an axis of `in` pixels resampled to `out` */
typedef struct
{
    int left, n;
    float *w;
} taps;

static taps taps_for(resize_filter f, int in, int out, int o, float *buf)
{
    float support = f == RESIZE_LANCZOS3 ? 3.0f : 1.0f;
    float ratio = (float)in / (float)out;
    float sratio = ratio < 1.0f ? 1.0f : ratio;
    float src_support = support * sratio;
    float input = ((float)o + 0.5f) * ratio;
    long long left = (long long)floorf(input - src_support);
    left = left < 0 ? 0 : left > in - 1 ? in - 1 : left;
    long long right = (long long)ceilf(input + src_support);
    right = right < left + 1 ? left + 1 : right > in ? in : right;
    input -= 0.5f;
    float sum = 0.0f;
    taps t = { (int)left, (int)(right - left), buf };
    for (int i = 0; i < t.n; i++)
    {
        buf[i] = kernel(f, ((float)(left + i) - input) / sratio);
        sum += buf[i];
    }
    for (int i = 0; i < t.n; i++)
        buf[i] /= sum;
    return t;
}

static int max_taps(resize_filter f, int in, int out)
{
    float support = f == RESIZE_LANCZOS3 ? 3.0f : 1.0f;
    float ratio = (float)in / (float)out;
    return (int)ceilf(2.0f * support * (ratio < 1.0f ? 1.0f : ratio)) + 3;
}

img *img_resize(const img *src, int w, int h, resize_filter filter)
{
    if (src->w == 0 || src->h == 0 || (w == src->w && h == src->h))
        return src->w == 0 || src->h == 0 ? blank(w, h) : img_dup(src);
    img *out = blank(w, h);
    int vmax = max_taps(filter, src->h, h), hmax = max_taps(filter, src->w, w);
    float *row = malloc((size_t)src->w * 4 * sizeof(float));
    float *vw = malloc((size_t)vmax * sizeof(float));
    float *hw = malloc((size_t)w * hmax * sizeof(float));
    taps *ht = malloc((size_t)w * sizeof(taps));
    if (!out || !row || !vw || !hw || !ht)
    {
        img_free(out);
        out = NULL;
        goto done;
    }
    for (int x = 0; x < w; x++)
        ht[x] = taps_for(filter, src->w, w, x, hw + (size_t)x * hmax);
    for (int y = 0; y < h; y++)
    {
        /* The vertical pass for this output row, unrounded */
        taps vt = taps_for(filter, src->h, h, y, vw);
        for (int x = 0; x < src->w; x++)
        {
            float t[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < vt.n; i++)
            {
                const uint8_t *p = img_at(src, x, vt.left + i);
                for (int c = 0; c < 4; c++)
                    t[c] += (float)p[c] * vt.w[i];
            }
            memcpy(row + (size_t)x * 4, t, sizeof t);
        }
        /* The horizontal pass, clamped and rounded */
        for (int x = 0; x < w; x++)
        {
            float t[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < ht[x].n; i++)
            {
                const float *p = row + (size_t)(ht[x].left + i) * 4;
                for (int c = 0; c < 4; c++)
                    t[c] += p[c] * ht[x].w[i];
            }
            uint8_t *o = img_at(out, x, y);
            for (int c = 0; c < 4; c++)
                o[c] = (uint8_t)roundf(t[c] < 0.0f ? 0.0f : t[c] > 255.0f ? 255.0f : t[c]);
        }
    }
done:
    free(row);
    free(vw);
    free(hw);
    free(ht);
    return out;
}
