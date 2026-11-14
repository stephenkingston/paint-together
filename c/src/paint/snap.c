/*
 * Paint (C port) - the picture under an operation, kept by tile where it changed
 */
#include <stdlib.h>
#include <string.h>
#include "paint/snap.h"

snap *snap_new(const img *pic)
{
    snap *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->w = pic->w;
    s->h = pic->h;
    s->tw = (pic->w + SNAP_TILE - 1) >> SNAP_SHIFT;
    s->th = (pic->h + SNAP_TILE - 1) >> SNAP_SHIFT;
    s->tiles = calloc((size_t)s->tw * s->th + 1, sizeof(uint8_t *));
    if (!s->tiles)
    {
        free(s);
        return NULL;
    }
    return s;
}

void snap_free(snap *s)
{
    if (!s)
        return;
    for (int i = 0; i < s->tw * s->th; i++)
        free(s->tiles[i]);
    free(s->tiles);
    free(s);
}

/* Call fn for each tile under r (clipped to the picture) with the part of r in it; kept: only kept tiles */
static void each_tile(snap *s, irect r, int kept, void (*fn)(snap *s, uint8_t **t, irect tr, irect part, void *user), void *user)
{
    r = irect_intersect(r, irect_make(0, 0, s->w, s->h));
    if (irect_empty(r))
        return;
    for (int ty = r.y0 >> SNAP_SHIFT; ty <= (r.y1 - 1) >> SNAP_SHIFT; ty++)
        for (int tx = r.x0 >> SNAP_SHIFT; tx <= (r.x1 - 1) >> SNAP_SHIFT; tx++)
        {
            uint8_t **t = &s->tiles[ty * s->tw + tx];
            if (kept && !*t)
                continue;
            irect tr = irect_make(tx << SNAP_SHIFT, ty << SNAP_SHIFT, (tx + 1) << SNAP_SHIFT, (ty + 1) << SNAP_SHIFT);
            fn(s, t, tr, irect_intersect(tr, r), user);
        }
}

static inline uint8_t *tile_at(uint8_t *t, irect tr, int x, int y)
{
    return t + ((size_t)(y - tr.y0) * SNAP_TILE + (size_t)(x - tr.x0)) * 4;
}

/* Copy part between a tile and a picture whose top-left is at (ox, oy), either way */
static void copy_part(uint8_t *t, irect tr, irect part, const img *pic, int ox, int oy, int to_tile)
{
    for (int y = part.y0; y < part.y1; y++)
    {
        uint8_t *a = tile_at(t, tr, part.x0, y), *b = img_at(pic, part.x0 - ox, y - oy);
        memcpy(to_tile ? a : b, to_tile ? b : a, (size_t)irect_w(part) * 4);
    }
}

int snap_mask_has(const mask *m, irect tile)
{
    return m->tiles[(tile.y0 >> MASK_TILE_SHIFT) * m->tw + (tile.x0 >> MASK_TILE_SHIFT)] != NULL;
}

typedef struct
{
    const img *pic;
    const mask *m;
} keep_ctx;

static void keep_tile(snap *s, uint8_t **t, irect tr, irect part, void *user)
{
    keep_ctx *c = user;
    if (*t || (c->m && !snap_mask_has(c->m, tr)) || !(*t = malloc((size_t)SNAP_TILE * SNAP_TILE * 4)))
        return;
    s->n_kept++;
    copy_part(*t, tr, irect_intersect(tr, irect_make(0, 0, s->w, s->h)), c->pic, 0, 0, 1);
}

void snap_keep(snap *s, const img *pic, irect r, const mask *m)
{
    keep_ctx c = { pic, m };
    each_tile(s, r, 0, keep_tile, &c);
}

static void restore_tile(snap *s, uint8_t **t, irect tr, irect part, void *user)
{
    copy_part(*t, tr, part, user, 0, 0, 0);
}

void snap_restore(const snap *s, img *pic, irect r)
{
    each_tile((snap *)s, r, 1, restore_tile, pic);
}

typedef struct
{
    int (*needed)(irect tile, void *user);
    void *user;
} drop_ctx;

static void drop_tile(snap *s, uint8_t **t, irect tr, irect part, void *user)
{
    drop_ctx *c = user;
    if (c->needed(tr, c->user))
        return;
    free(*t);
    *t = NULL;
    s->n_kept--;
}

void snap_drop(snap *s, irect r, int (*needed)(irect tile, void *user), void *user)
{
    drop_ctx c = { needed, user };
    each_tile(s, r, 1, drop_tile, &c);
}

typedef struct
{
    const img *src;
    int x, y;
} put_ctx;

static void put_tile(snap *s, uint8_t **t, irect tr, irect part, void *user)
{
    put_ctx *c = user;
    copy_part(*t, tr, part, c->src, c->x, c->y, 1);
}

void snap_put(snap *s, const img *src, int x, int y)
{
    put_ctx c = { src, x, y };
    each_tile(s, irect_make(x, y, x + src->w, y + src->h), 1, put_tile, &c);
}

void snap_rebase(snap *s, const img *base, irect r)
{
    put_ctx c = { base, 0, 0 };
    each_tile(s, r, 1, put_tile, &c);
}

static void fill_tile(snap *s, uint8_t **t, irect tr, irect part, void *user)
{
    for (int y = part.y0; y < part.y1; y++)
        for (int x = part.x0; x < part.x1; x++)
            memcpy(tile_at(*t, tr, x, y), user, 4);
}

void snap_fill(snap *s, irect r, rgba c)
{
    each_tile(s, r, 1, fill_tile, &c);
}

typedef struct
{
    const snap *s;
    const img *pic;
    irect r;
    uint8_t *row;       /* a row put together */
} rows_ctx;

/* Row y of r as it was: the picture's, with the kept tiles' pixels over it */
static const uint8_t *before_row(int y, void *user)
{
    rows_ctx *c = user;
    const snap *s = c->s;
    irect r = c->r;
    uint8_t *row = NULL;
    for (int tx = r.x0 >> SNAP_SHIFT; tx <= (r.x1 - 1) >> SNAP_SHIFT; tx++)
    {
        uint8_t *t = s->tiles[(y >> SNAP_SHIFT) * s->tw + tx];
        if (!t)
            continue;
        if (!row)
            row = memcpy(c->row, img_at(c->pic, r.x0, y), (size_t)irect_w(r) * 4);
        irect tr = irect_make(tx << SNAP_SHIFT, y & ~(SNAP_TILE - 1), (tx + 1) << SNAP_SHIFT, (y | (SNAP_TILE - 1)) + 1);
        irect part = irect_intersect(tr, r);
        memcpy(row + (size_t)(part.x0 - r.x0) * 4, tile_at(t, tr, part.x0, y), (size_t)irect_w(part) * 4);
    }
    return row ? row : img_at(c->pic, r.x0, y);
}

static const uint8_t *after_row(int y, void *user)
{
    rows_ctx *c = user;
    return img_at(c->pic, c->r.x0, y);
}

change *snap_change(const snap *s, const img *pic, irect r)
{
    r = irect_intersect(r, irect_make(0, 0, s->w, s->h));
    rows_ctx c = { s, pic, r, irect_empty(r) ? NULL : malloc((size_t)irect_w(r) * 4) };
    change *ch = c.row ? change_patch_rows(r, before_row, after_row, &c) : NULL;
    free(c.row);
    return ch;
}
