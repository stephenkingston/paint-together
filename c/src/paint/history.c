/*
 * Paint (C port) - undo and redo (Rust: history.rs)
 *
 * Two stacks of edits. Pushing clears the redo stack and drops the oldest
 * undo steps while their pixels add up to more than the 1 GB budget (always
 * keeping the newest).
 */
#include <stdlib.h>
#include <string.h>
#include "paint/history.h"
#include "paint/raster.h"

/* Total pixel bytes kept for undo before the oldest steps are dropped */
#define BUDGET_BYTES ((size_t)1 << 30)

typedef struct
{
    uint64_t id;
    change *c;
} edit;

typedef struct
{
    edit *v;
    size_t n, cap;
} stack;

struct history
{
    stack undo, redo;
    uint64_t next_id;
    size_t undo_bytes;      /* pixel bytes held by the undo stack */
};

change *change_patch(const img *before, const img *after, irect r)
{
    r = irect_intersect(irect_intersect(r, irect_of(before)), irect_of(after));
    if (irect_empty(r))
        return NULL;
    int same = 1;
    for (int y = r.y0; y < r.y1 && same; y++)
        same = !memcmp(img_at(before, r.x0, y), img_at(after, r.x0, y), (size_t)irect_w(r) * 4);
    if (same)
        return NULL;
    change *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->kind = CHANGE_PATCH;
    c->x = r.x0;
    c->y = r.y0;
    c->before = raster_crop(before, r);
    c->after = raster_crop(after, r);
    if (!c->before || !c->after)
    {
        change_free(c);
        return NULL;
    }
    return c;
}

change *change_replace(img *before, img *after)
{
    change *c = calloc(1, sizeof *c);
    if (!c)
    {
        img_free(before);
        img_free(after);
        return NULL;
    }
    c->kind = CHANGE_REPLACE;
    c->before = before;
    c->after = after;
    return c;
}

void change_free(change *c)
{
    if (c)
    {
        img_free(c->before);
        img_free(c->after);
        free(c);
    }
}

static size_t change_bytes(const change *c)
{
    return ((size_t)c->before->w * c->before->h + (size_t)c->after->w * c->after->h) * 4;
}

void change_apply(const change *c, img **picture, int undo)
{
    const img *src = undo ? c->before : c->after;
    if (c->kind == CHANGE_PATCH)
        raster_replace(*picture, src, c->x, c->y);
    else
    {
        img *copy = img_dup(src);
        if (copy)
        {
            img_free(*picture);
            *picture = copy;
        }
    }
}

static void stack_push(stack *s, edit e)
{
    if (s->n == s->cap)
    {
        s->cap = s->cap ? s->cap * 2 : 32;
        s->v = realloc(s->v, s->cap * sizeof(edit));
    }
    s->v[s->n++] = e;
}

static void stack_clear(stack *s)
{
    for (size_t i = 0; i < s->n; i++)
        change_free(s->v[i].c);
    s->n = 0;
}

history *history_new(void)
{
    return calloc(1, sizeof(history));
}

void history_free(history *h)
{
    if (!h)
        return;
    stack_clear(&h->undo);
    stack_clear(&h->redo);
    free(h->undo.v);
    free(h->redo.v);
    free(h);
}

void history_push(history *h, change *c)
{
    if (!c)
        return;
    stack_clear(&h->redo);
    h->next_id++;
    stack_push(&h->undo, (edit){ h->next_id, c });
    h->undo_bytes += change_bytes(c);
    size_t drop = 0;
    while (h->undo.n - drop > 1 && h->undo_bytes > BUDGET_BYTES)
    {
        h->undo_bytes -= change_bytes(h->undo.v[drop].c);
        change_free(h->undo.v[drop].c);
        drop++;
    }
    if (drop)
    {
        memmove(h->undo.v, h->undo.v + drop, (h->undo.n - drop) * sizeof(edit));
        h->undo.n -= drop;
    }
}

/* Move the top of `from` to `to`, keeping the undo stack's byte count */
static void move_top(history *h, stack *from, stack *to)
{
    edit e = from->v[--from->n];
    if (from == &h->undo)
        h->undo_bytes -= change_bytes(e.c);
    else
        h->undo_bytes += change_bytes(e.c);
    stack_push(to, e);
}

int history_undo(history *h, img **picture)
{
    if (!h->undo.n)
        return 0;
    change_apply(h->undo.v[h->undo.n - 1].c, picture, 1);
    move_top(h, &h->undo, &h->redo);
    return 1;
}

int history_redo(history *h, img **picture)
{
    if (!h->redo.n)
        return 0;
    change_apply(h->redo.v[h->redo.n - 1].c, picture, 0);
    move_top(h, &h->redo, &h->undo);
    return 1;
}

int history_step_with(history *h, int back, int (*apply)(const change *c, void *user), void *user)
{
    stack *from = back ? &h->undo : &h->redo, *to = back ? &h->redo : &h->undo;
    if (!from->n || !apply(from->v[from->n - 1].c, user))
        return 0;
    move_top(h, from, to);
    return 1;
}

uint64_t history_redo_id(const history *h)
{
    return h->redo.n ? h->redo.v[h->redo.n - 1].id : 0;
}

int history_can_undo(const history *h)
{
    return h->undo.n > 0;
}

int history_can_redo(const history *h)
{
    return h->redo.n > 0;
}

uint64_t history_state_id(const history *h)
{
    return h->undo.n ? h->undo.v[h->undo.n - 1].id : 0;
}

void history_clear(history *h)
{
    stack_clear(&h->undo);
    stack_clear(&h->redo);
    h->undo_bytes = 0;
}
