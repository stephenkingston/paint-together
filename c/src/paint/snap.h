/*
 * Paint (C port) - the picture as it was when an operation began, kept only where it has changed since
 *
 * A stroke, a live shape and the text box redraw themselves over the picture
 * as it was when they started (Rust kept a whole copy for that). A snap keeps
 * that picture in 64 x 64 tiles, copied from the picture just before the
 * operation first draws on them (snap_keep); everywhere else the picture
 * itself still holds those pixels. So the operation must call snap_keep
 * before every change it makes to the picture, and nothing else may change
 * the picture while the snap is in use, except as snap_rebase says. The
 * floating selection keeps the canvas under it this way too.
 */
#ifndef PAINT_SNAP_H
#define PAINT_SNAP_H

#include "paint/history.h"
#include "paint/raster.h"

/* The same tiles as the coverage masks', so a mask's tiles say which of ours a drawing touches */
#define SNAP_SHIFT MASK_TILE_SHIFT
#define SNAP_TILE (1 << SNAP_SHIFT)

typedef struct
{
    int w, h, tw, th;
    uint8_t **tiles;    /* tw * th: the tile as it was, or NULL while the picture still holds it */
    int n_kept;
} snap;

snap *snap_new(const img *pic);             /* nothing kept yet; NULL if out of memory */
void snap_free(snap *s);
/* pic is about to change in r (only where m has tiles, if given): keep its tiles there as they are now */
void snap_keep(snap *s, const img *pic, irect r, const mask *m);
/* Put the pixels of r back as they were */
void snap_restore(const snap *s, img *pic, irect r);
/* r was just put back, and only the tiles needed() says will be drawn on again: let go of the rest of r's */
void snap_drop(snap *s, irect r, int (*needed)(irect tile, void *user), void *user);
int snap_mask_has(const mask *m, irect tile);      /* the mask has a tile there */
/* The picture under the operation changed in r (a friend drew there): base's pixels there become the
 * snapshot's. The caller then puts base's pixels of r into the picture too. */
void snap_rebase(snap *s, const img *base, irect r);
/* Change the snapshot itself (only its kept tiles): src's pixels with their top-left at (x, y), or c over r */
void snap_put(snap *s, const img *src, int x, int y);
void snap_fill(snap *s, irect r, rgba c);
/* A pixel of the snapshot, or NULL where the picture still holds it */
static inline uint8_t *snap_px(const snap *s, int x, int y)
{
    uint8_t *t = s->tiles[(y >> SNAP_SHIFT) * s->tw + (x >> SNAP_SHIFT)];
    return t ? t + ((size_t)(y & (SNAP_TILE - 1)) * SNAP_TILE + (size_t)(x & (SNAP_TILE - 1))) * 4 : NULL;
}
/* The undo step from the snapshot to pic within r, or NULL if nothing changed */
change *snap_change(const snap *s, const img *pic, irect r);

#endif
