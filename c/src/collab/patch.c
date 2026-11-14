/*
 * Paint (C port) - edits as they travel between kids (Rust: collab/patch.rs)
 */
#include "collab/patch.h"

#include <stdlib.h>
#include <string.h>

void patch_free(patch *p)
{
    free(p->mask);
    free(p->pixels);
    memset(p, 0, sizeof *p);
}

void picture_free(picture *pic)
{
    free(pic->rgba);
    memset(pic, 0, sizeof *pic);
}

void edit_free(edit *e)
{
    patch_free(&e->p);
    picture_free(&e->pic);
}

static uint8_t *dup_bytes(const uint8_t *src, size_t len)
{
    if (!len)
        return NULL;
    uint8_t *d = malloc(len);
    if (d)
        memcpy(d, src, len);
    return d;
}

patch patch_clone(const patch *p)
{
    patch c = *p;
    c.mask = dup_bytes(p->mask, p->mask_len);
    c.pixels = dup_bytes(p->pixels, p->pixels_len);
    return c;
}

edit edit_clone(const edit *e)
{
    edit c = { e->kind, patch_clone(&e->p), e->pic };
    c.pic.rgba = dup_bytes(e->pic.rgba, e->pic.len);
    return c;
}

int patch_eq(const patch *a, const patch *b)
{
    return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h && a->mask_len == b->mask_len
           && a->pixels_len == b->pixels_len && (!a->mask_len || !memcmp(a->mask, b->mask, a->mask_len))
           && (!a->pixels_len || !memcmp(a->pixels, b->pixels, a->pixels_len));
}

int picture_valid(const picture *pic)
{
    return pic->w >= 1 && pic->w <= MAX_SIDE && pic->h >= 1 && pic->h <= MAX_SIDE
           && (uint64_t)pic->len == (uint64_t)pic->w * pic->h * 4;
}

picture picture_of(const img *im)
{
    size_t len = (size_t)im->w * im->h * 4;
    picture pic = { (uint32_t)im->w, (uint32_t)im->h, dup_bytes(im->px, len), len };
    return pic;
}

img *picture_to_image(const picture *pic)
{
    if (!picture_valid(pic))
        return NULL;
    img *im = img_new((int)pic->w, (int)pic->h, (rgba){ 0, 0, 0, 0 });
    if (im)
        memcpy(im->px, pic->rgba, pic->len);
    return im;
}

static int popcount8(uint8_t b)
{
    int n = 0;
    for (; b; b &= (uint8_t)(b - 1))
        n++;
    return n;
}

int patch_valid(const patch *p)
{
    uint64_t n = (uint64_t)p->w * p->h, set = 0;
    if (p->w > MAX_SIDE || p->h > MAX_SIDE || p->x < -MAX_SIDE || p->x > MAX_SIDE || p->y < -MAX_SIDE || p->y > MAX_SIDE)
        return 0;
    if ((uint64_t)p->mask_len != (n + 7) / 8)
        return 0;
    for (size_t i = 0; i < p->mask_len; i++)
        set += (uint64_t)popcount8(p->mask[i]);
    return (uint64_t)p->pixels_len == set * 4;
}

irect patch_rect(const patch *p)
{
    return irect_make(p->x, p->y, p->x + (int)p->w, p->y + (int)p->h);
}

static int bit(const patch *p, size_t i)
{
    return p->mask[i / 8] >> (i % 8) & 1;
}

/* Collects a patch's pixels as they're found, row by row over its rectangle */
typedef struct
{
    patch p;
    size_t cap;
} builder;

static void builder_start(builder *b, irect r)
{
    memset(b, 0, sizeof *b);
    b->p.x = r.x0;
    b->p.y = r.y0;
    b->p.w = (uint32_t)irect_w(r);
    b->p.h = (uint32_t)irect_h(r);
    b->p.mask_len = ((size_t)b->p.w * b->p.h + 7) / 8;
    b->p.mask = calloc(b->p.mask_len ? b->p.mask_len : 1, 1);
}

static void builder_add(builder *b, size_t i, const uint8_t px[4])
{
    if (b->p.pixels_len + 4 > b->cap)
    {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->p.pixels = realloc(b->p.pixels, b->cap);
    }
    b->p.mask[i / 8] |= (uint8_t)(1 << (i % 8));
    memcpy(b->p.pixels + b->p.pixels_len, px, 4);
    b->p.pixels_len += 4;
}

/* Hand over the patch, or nothing if no pixel was picked */
static int builder_finish(builder *b, patch *out)
{
    if (!b->p.pixels_len)
    {
        patch_free(&b->p);
        memset(out, 0, sizeof *out);
        return 0;
    }
    b->p.pixels = realloc(b->p.pixels, b->p.pixels_len);
    *out = b->p;
    return 1;
}

int patch_build(irect r, int (*pick)(int x, int y, uint8_t px[4], void *user), void *user, patch *out)
{
    builder b;
    builder_start(&b, r);
    size_t i = 0;
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++, i++)
        {
            uint8_t px[4];
            if (pick(x, y, px, user))
                builder_add(&b, i, px);
        }
    return builder_finish(&b, out);
}

int patch_diff(const img *before, const img *after, irect r, patch *out)
{
    r = irect_intersect(irect_intersect(r, irect_of(before)), irect_of(after));
    memset(out, 0, sizeof *out);
    if (irect_empty(r))
        return 0;
    builder b;
    builder_start(&b, r);
    size_t i = 0;
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++, i++)
        {
            const uint8_t *a = img_at(after, x, y);
            if (memcmp(img_at(before, x, y), a, 4))
                builder_add(&b, i, a);
        }
    return builder_finish(&b, out);
}

void patch_apply_within(const patch *p, img *im, irect clip)
{
    clip = irect_intersect(irect_intersect(clip, irect_of(im)), patch_rect(p));
    if (irect_empty(clip))
        return;
    size_t src = 0, i = 0;
    for (int j = 0; j < (int)p->h; j++)
    {
        int y = p->y + j;
        for (int k = 0; k < (int)p->w; k++, i++)
        {
            if (!bit(p, i))
                continue;
            int x = p->x + k;
            if (x >= clip.x0 && x < clip.x1 && y >= clip.y0 && y < clip.y1)
                memcpy(img_at(im, x, y), p->pixels + src, 4);
            src += 4;
        }
    }
}

void patch_each(const patch *p, void (*fn)(int x, int y, const uint8_t px[4], void *user), void *user)
{
    size_t src = 0, i = 0;
    for (int j = 0; j < (int)p->h; j++)
        for (int k = 0; k < (int)p->w; k++, i++)
            if (bit(p, i))
            {
                fn(p->x + k, p->y + j, p->pixels + src, user);
                src += 4;
            }
}

int edit_valid(const edit *e)
{
    return e->kind == EDIT_PATCH ? patch_valid(&e->p) : picture_valid(&e->pic);
}

typedef struct
{
    builder b;
    irect r;                /* where the patch goes: within the change's rectangle */
    int x, y, undo;         /* the change's corner; undo: from its after side to its before side */
    const img *current;
    size_t i;
} diff_ctx;

static void diff_row(int y, const uint8_t *before, const uint8_t *after, void *user)
{
    diff_ctx *d = user;
    int py = d->y + y;
    if (py < d->r.y0 || py >= d->r.y1)
        return;
    const uint8_t *from = d->undo ? after : before, *to = d->undo ? before : after;
    for (int x = d->r.x0; x < d->r.x1; x++, d->i++)
    {
        const uint8_t *f = from + (size_t)(x - d->x) * 4, *t = to + (size_t)(x - d->x) * 4;
        if (memcmp(f, t, 4) && (!d->current || !memcmp(img_at(d->current, x, py), f, 4)))
            builder_add(&d->b, d->i, t);
    }
}

/* A patch over r (within the change's rectangle): each pixel the change's `to` side shows where it
 * differs from its `from` side (and, when `current` is given, only where current still shows `from`) */
static int change_patch_edit(const change *c, irect r, int undo, const img *current, edit *out)
{
    memset(out, 0, sizeof *out);
    if (irect_empty(r))
        return 0;
    diff_ctx d = { .r = r, .x = c->x, .y = c->y, .undo = undo, .current = current };
    builder_start(&d.b, r);
    change_rows(c, 3, diff_row, &d);
    out->kind = EDIT_PATCH;
    return builder_finish(&d.b, &out->p);
}

/* A whole-picture edit showing one side of the change */
static int change_picture_edit(const change *c, int after, edit *out)
{
    memset(out, 0, sizeof *out);
    img *im = change_image(c, after);
    if (!im)
        return 0;
    out->kind = EDIT_REPLACE;
    out->pic = (picture){ (uint32_t)im->w, (uint32_t)im->h, im->px, (size_t)im->w * im->h * 4 };
    free(im);               /* the pixels now belong to the edit */
    return 1;
}

int edit_from_change(const change *c, edit *out)
{
    if (c->kind == CHANGE_REPLACE)
        return change_picture_edit(c, 1, out);
    return change_patch_edit(c, irect_make(c->x, c->y, c->x + c->aw, c->y + c->ah), 0, NULL, out);
}

region edit_region(const edit *e, const img *im)
{
    region g = { 1, IRECT_EMPTY };
    if (e->kind == EDIT_PATCH)
    {
        g.whole = 0;
        g.r = irect_intersect(patch_rect(&e->p), irect_of(im));
    }
    return g;
}

void edit_apply(const edit *e, img **im)
{
    if (e->kind == EDIT_PATCH)
    {
        patch_apply_within(&e->p, *im, irect_of(*im));
        return;
    }
    img *now = picture_to_image(&e->pic);
    if (now)
    {
        img_free(*im);
        *im = now;
    }
}

int collab_revert(const change *c, const img *current, int undo, edit *out)
{
    if (c->kind == CHANGE_REPLACE)
        return change_picture_edit(c, !undo, out);
    irect r = irect_intersect(irect_make(c->x, c->y, c->x + c->aw, c->y + c->ah), irect_of(current));
    return change_patch_edit(c, r, undo, current, out);
}
