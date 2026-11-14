/*
 * Paint (C port) - undo and redo (Rust: history.rs)
 *
 * Most edits store only the before/after pixels of the rectangle they
 * touched; edits that change the canvas size store whole pictures. The
 * pixels are kept packed (runs of one colour, and "unchanged" runs in the
 * after side), so a fill or a brush stroke costs a small part of its
 * rectangle; read them back with change_rows or change_image. The oldest
 * steps are dropped beyond a 1 GB budget of packed bytes. Each step has an
 * id, so drawing together can tell which steps are still "mine".
 */
#ifndef PAINT_HISTORY_H
#define PAINT_HISTORY_H

#include <stdint.h>
#include "core/img.h"

enum
{
    CHANGE_PATCH,       /* x y: before and after are the same size, placed at x y */
    CHANGE_REPLACE,     /* before and after are whole pictures (sizes may differ) */
};

typedef struct
{
    int kind;
    int x, y;
    int bw, bh, aw, ah;         /* sizes of the before and after pixels */
    int delta;                  /* the after side is packed against the before side */
    uint8_t *before, *after;    /* owned, packed (history.c) */
    size_t bytes;               /* what the packed pixels take */
} change;

/* The difference between two same-sized pictures within r, or NULL if nothing changed */
change *change_patch(const img *before, const img *after, irect r);
/* The same from rows handed out by callbacks (for a picture before that isn't kept whole): row y of the
 * picture, from r.x0. A row from one callback must stay valid during a call to the other. */
typedef const uint8_t *(*change_row_fn)(int y, void *user);
change *change_patch_rows(irect r, change_row_fn before, change_row_fn after, void *user);
change *change_replace(img *before, const img *after);    /* takes ownership of before */
void change_free(change *c);
void change_apply(const change *c, img **picture, int undo);  /* may replace *picture (CHANGE_REPLACE) */
/* Unpack a step a row at a time: fn gets row y (0 = top) of the pixels before and after it.
 * which: 1 before, 2 after, 3 both (only when they are the same size); the side not asked for is NULL */
void change_rows(const change *c, int which, void (*fn)(int y, const uint8_t *before, const uint8_t *after, void *user),
                 void *user);
img *change_image(const change *c, int after);     /* one side unpacked, for the caller to free; NULL if out of memory */

typedef struct history history;

history *history_new(void);
void history_free(history *h);
void history_push(history *h, change *c);          /* takes ownership; clears redo */
int history_undo(history *h, img **picture);        /* 1 if a step was undone */
int history_redo(history *h, img **picture);
/* Step back (undo) or forward without applying: `apply` decides and returns 1 if it
 * did it (the step then moves to the other stack), 0 to leave the stacks alone */
int history_step_with(history *h, int back, int (*apply)(const change *c, void *user), void *user);
uint64_t history_redo_id(const history *h);         /* id of the step a redo would bring back, 0 none */
int history_can_undo(const history *h);
int history_can_redo(const history *h);
uint64_t history_state_id(const history *h);        /* id of the newest applied step (0 = none): "is it saved?" */
void history_clear(history *h);

#endif
