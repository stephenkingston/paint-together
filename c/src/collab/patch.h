/*
 * Paint (C port) - edits as they travel between kids (Rust: collab/patch.rs)
 *
 * Edits carry the pixels that changed, never the commands that changed them:
 * a brush, a font or a maths library that differs between two computers can't
 * make their pictures drift apart. A patch is a rectangle with one mask bit per
 * pixel (set where the pixel changed) and the new RGBA of each set bit, so two
 * kids drawing side by side never wipe out each other's work.
 *
 * The structs own their buffers; a zeroed struct is empty and safe to free.
 */
#ifndef COLLAB_PATCH_H
#define COLLAB_PATCH_H

#include <stddef.h>
#include <stdint.h>
#include "core/img.h"
#include "paint/history.h"

/* The largest picture anyone may share (in pixels per side) */
#define MAX_SIDE 16384

typedef struct
{
    int32_t x, y;
    uint32_t w, h;
    uint8_t *mask;          /* (w * h + 7) / 8 bytes, bit i % 8 of byte i / 8 */
    size_t mask_len;
    uint8_t *pixels;        /* RGBA for each set bit, in the same order */
    size_t pixels_len;
} patch;

/* A whole picture (for edits that change its size, or replace it outright) */
typedef struct
{
    uint32_t w, h;
    uint8_t *rgba;
    size_t len;
    int borrowed;           /* rgba is someone else's (a hub's picture, while its welcome goes out) */
} picture;

enum
{
    EDIT_PATCH,
    EDIT_REPLACE,
};

typedef struct
{
    int kind;
    patch p;                /* EDIT_PATCH */
    picture pic;            /* EDIT_REPLACE */
} edit;

/* Which pixels an edit touched: a rectangle, or the whole picture (possibly at a new size) */
typedef struct
{
    int whole;
    irect r;
} region;

void patch_free(patch *p);
void picture_free(picture *pic);
void edit_free(edit *e);
patch patch_clone(const patch *p);
edit edit_clone(const edit *e);
int patch_eq(const patch *a, const patch *b);

/* Holds exactly the bytes its size needs, and a size Paint can open */
int picture_valid(const picture *pic);
img *picture_to_image(const picture *pic);          /* NULL if not valid */
img *picture_take(picture *pic);                    /* the same, taking pic's pixels (pic is emptied) */

/* Its mask and pixels agree with its size (anything from the network is checked) */
int patch_valid(const patch *p);
irect patch_rect(const patch *p);
/* A patch over r holding pick(x, y) wherever it returns 1; 0 (and *out empty) if none did */
int patch_build(irect r, int (*pick)(int x, int y, uint8_t px[4], void *user), void *user, patch *out);
/* Pixels inside r that differ between two same-sized images */
int patch_diff(const img *before, const img *after, irect r, patch *out);
/* Write the changed pixels that fall inside clip (and the image) */
void patch_apply_within(const patch *p, img *im, irect clip);
/* Each changed pixel with its new value */
void patch_each(const patch *p, void (*fn)(int x, int y, const uint8_t px[4], void *user), void *user);

int edit_valid(const edit *e);
/* The edit a finished local change sends to everyone else; 0 if it changed nothing */
int edit_from_change(const change *c, edit *out);
region edit_region(const edit *e, const img *im);
/* Apply to *im; a whole-picture edit replaces *im */
void edit_apply(const edit *e, img **im);

/* Undo (or redo) one of my own changes without disturbing anyone else's: a pixel
 * goes back only while the picture still shows what my change left there.
 * Whole-picture changes come back whole; the caller decides whether that's allowed. */
int collab_revert(const change *c, const img *current, int undo, edit *out);

#endif
