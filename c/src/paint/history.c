/*
 * Paint (C port) - undo and redo (Rust: history.rs)
 *
 * Two stacks of edits. Pushing clears the redo stack and drops the oldest
 * undo steps while their packed pixels add up to more than the 1 GB budget
 * (always keeping the newest).
 *
 * A step's pixels are packed row by row into ops, each a varint
 * (count << 2 | kind) followed by its pixels:
 *   OP_LITERAL  count pixels, 4 bytes each
 *   OP_RUN      one pixel, repeated count times
 *   OP_SAME     no pixels: the after side's pixels equal the before side's
 *   OP_XOR      one value: the after side's pixels are the before side's
 *               XOR it (an inverted picture is one op a row)
 * (OP_SAME and OP_XOR only in the after side, when both sides are the
 * same size.) A flat fill becomes a few bytes a row, and a brush stroke's
 * after side only the pixels it changed. A run is taken only when it is
 * shorter than spelling its pixels out, so a row never packs bigger than
 * its raw pixels plus one op.
 */
#include <stdlib.h>
#include <string.h>
#include "paint/history.h"

/* Total packed bytes kept for undo before the oldest steps are dropped */
#define BUDGET_BYTES ((size_t)1 << 30)

enum
{
    OP_LITERAL,
    OP_RUN,
    OP_SAME,
    OP_XOR,
};

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
    size_t undo_bytes;      /* packed bytes held by the undo stack */
};

/* ---------------------------------------------------------------- packing */

static inline uint32_t px32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint8_t *put_op(uint8_t *o, size_t n, int op)
{
    size_t v = n << 2 | (size_t)op;
    for (; v > 127; v >>= 7)
        *o++ = (uint8_t)(v | 128);
    *o++ = (uint8_t)v;
    return o;
}

/* Where, from i on, the first run worth an op starts (w if none): three pixels of one colour, or (in the
 * after side) two unchanged pixels or three changed alike. Kept tight: photos are scanned through here. */
static int literal_end(const uint8_t *row, const uint8_t *ref, int i, int w)
{
    for (; i + 2 < w; i++)
    {
        uint32_t p = px32(row + (size_t)i * 4), q = px32(row + (size_t)i * 4 + 4), r = px32(row + (size_t)i * 4 + 8);
        if (p == q && q == r)
            return i;
        if (ref)
        {
            uint32_t x = p ^ px32(ref + (size_t)i * 4);
            if (x == (q ^ px32(ref + (size_t)i * 4 + 4)) && (!x || x == (r ^ px32(ref + (size_t)i * 4 + 8))))
                return i;
        }
    }
    /* The last two pixels may still be unchanged */
    return ref && i + 1 < w && !memcmp(row + (size_t)i * 4, ref + (size_t)i * 4, 8) ? i : w;
}

/* Pack a row of w pixels; ref is the same row before the edit, or NULL (*delta is set if an op uses it) */
static uint8_t *pack_row(uint8_t *o, const uint8_t *row, const uint8_t *ref, int w, int *delta)
{
    for (int i = 0; i < w;)
    {
        int end = literal_end(row, ref, i, w);
        if (end > i)
        {
            size_t n = (size_t)(end - i) * 4;
            o = put_op(o, (size_t)(end - i), OP_LITERAL);
            memcpy(o, row + (size_t)i * 4, n);
            o += n;
            i = end;
            if (i == w)
                break;
        }
        /* Take the longer of the run of one colour and the run of one change (which costs no pixel when
         * the change is none) */
        uint32_t p = px32(row + (size_t)i * 4), x = ref ? p ^ px32(ref + (size_t)i * 4) : 0;
        int d = 0, run = 1;
        if (ref)
            while (i + d < w && (px32(row + (size_t)(i + d) * 4) ^ px32(ref + (size_t)(i + d) * 4)) == x)
                d++;
        while (i + run < w && px32(row + (size_t)(i + run) * 4) == p)
            run++;
        if (d >= (x ? 3 : 2) && d >= run)
        {
            o = put_op(o, (size_t)d, x ? OP_XOR : OP_SAME);
            *delta = 1;
            i += d;
            if (!x)
                continue;
        }
        else
        {
            o = put_op(o, (size_t)run, OP_RUN);
            x = p;
            i += run;
        }
        memcpy(o, &x, 4);
        o += 4;
    }
    return o;
}

/* Unpack a row of w pixels; for OP_SAME and OP_XOR, `row` must already hold the row before the edit */
static const uint8_t *unpack_row(const uint8_t *s, uint8_t *row, int w)
{
    for (int i = 0; i < w;)
    {
        size_t v = 0;
        for (int shift = 0;; shift += 7)
        {
            v |= (size_t)(*s & 127) << shift;
            if (!(*s++ & 128))
                break;
        }
        int n = (int)(v >> 2), op = (int)(v & 3);
        uint8_t *o = row + (size_t)i * 4;
        if (op == OP_LITERAL)
        {
            memcpy(o, s, (size_t)n * 4);
            s += (size_t)n * 4;
        }
        else if (op == OP_RUN)
        {
            /* Doubling copies: a whole-canvas fill unpacks at memcpy speed */
            memcpy(o, s, 4);
            for (size_t k = 1; k < (size_t)n; k *= 2)
                memcpy(o + k * 4, o, ((size_t)n - k < k ? (size_t)n - k : k) * 4);
            s += 4;
        }
        else if (op == OP_XOR)
        {
            uint32_t x = px32(s);
            for (int k = 0; k < n; k++)
            {
                uint32_t q = px32(o + (size_t)k * 4) ^ x;
                memcpy(o + (size_t)k * 4, &q, 4);
            }
            s += 4;
        }
        i += n;
    }
    return s;
}

/* Rows of a picture, from column x0 (the row source the packing reads) */
typedef struct
{
    const img *im;
    int x0;
} img_rows;

static const uint8_t *img_row(int y, void *user)
{
    const img_rows *s = user;
    return img_at(s->im, s->x0, y);
}

/* Pack rows y0.. of w pixels (against the same rows of ref, if given) into a buffer that fits;
 * it starts at the worst case, given back once the size is known */
static uint8_t *pack_side(int w, int y0, int h, change_row_fn row, void *ru, change_row_fn ref, void *fu, int *delta, size_t *len)
{
    uint8_t *buf = malloc((size_t)h * ((size_t)w * 4 + 5) + 1), *o = buf;
    if (!buf)
        return NULL;
    for (int j = 0; j < h; j++)
        o = pack_row(o, row(y0 + j, ru), ref ? ref(y0 + j, fu) : NULL, w, delta);
    *len = (size_t)(o - buf);
    uint8_t *fit = realloc(buf, *len + 1);
    return fit ? fit : buf;
}

/* rb of the before rows, ra of the after rows (packed against the before rows when the sizes match) */
static change *pack_change(int kind, int x, int y, irect rb, change_row_fn bf, void *bu, irect ra, change_row_fn af, void *au)
{
    int bw = irect_w(rb), bh = irect_h(rb), aw = irect_w(ra), ah = irect_h(ra), delta = 0;
    size_t blen = 0, alen = 0;
    change *c = calloc(1, sizeof *c);
    uint8_t *pb = c ? pack_side(bw, rb.y0, bh, bf, bu, NULL, NULL, &delta, &blen) : NULL;
    uint8_t *pa = pb ? pack_side(aw, ra.y0, ah, af, au, bw == aw && bh == ah ? bf : NULL, bu, &delta, &alen) : NULL;
    if (!pa)
    {
        free(pb);
        free(c);
        return NULL;
    }
    *c = (change){ kind, x, y, bw, bh, aw, ah, delta, pb, pa, blen + alen };
    return c;
}

change *change_patch_rows(irect r, change_row_fn before, change_row_fn after, void *user)
{
    if (irect_empty(r))
        return NULL;
    int same = 1;
    for (int y = r.y0; y < r.y1 && same; y++)
        same = !memcmp(before(y, user), after(y, user), (size_t)irect_w(r) * 4);
    return same ? NULL : pack_change(CHANGE_PATCH, r.x0, r.y0, r, before, user, r, after, user);
}

change *change_patch(const img *before, const img *after, irect r)
{
    r = irect_intersect(irect_intersect(r, irect_of(before)), irect_of(after));
    img_rows b = { before, r.x0 }, a = { after, r.x0 };
    if (irect_empty(r))
        return NULL;
    int same = 1;
    for (int y = r.y0; y < r.y1 && same; y++)
        same = !memcmp(img_at(before, r.x0, y), img_at(after, r.x0, y), (size_t)irect_w(r) * 4);
    if (same)
        return NULL;
    return pack_change(CHANGE_PATCH, r.x0, r.y0, r, img_row, &b, r, img_row, &a);
}

change *change_replace(img *before, const img *after)
{
    img_rows b = { before, 0 }, a = { after, 0 };
    change *c = pack_change(CHANGE_REPLACE, 0, 0, irect_of(before), img_row, &b, irect_of(after), img_row, &a);
    img_free(before);
    return c;
}

void change_free(change *c)
{
    if (c)
    {
        free(c->before);
        free(c->after);
        free(c);
    }
}

void change_rows(const change *c, int which, void (*fn)(int y, const uint8_t *before, const uint8_t *after, void *user),
                 void *user)
{
    int h = which & 1 ? c->bh : c->ah;
    /* The after side needs the before row under it when it was packed against it */
    uint8_t *b = which & 1 || c->delta ? malloc((size_t)c->bw * 4 + 4) : NULL;
    uint8_t *a = which == 2 && c->delta ? b : which & 2 ? malloc((size_t)c->aw * 4 + 4) : NULL;
    const uint8_t *sb = c->before, *sa = c->after;
    for (int y = 0; y < h; y++)
    {
        if (b)
            sb = unpack_row(sb, b, c->bw);
        if (a)
        {
            if (a != b && c->delta)
                memcpy(a, b, (size_t)c->aw * 4);
            sa = unpack_row(sa, a, c->aw);
        }
        fn(y, which & 1 ? b : NULL, a, user);
    }
    if (a != b)
        free(a);
    free(b);
}

typedef struct
{
    img *dst;
    int x, y, w;
} put_ctx;

static void put_row(int y, const uint8_t *before, const uint8_t *after, void *user)
{
    put_ctx *p = user;
    const uint8_t *row = before ? before : after;
    int py = p->y + y, x0 = p->x > 0 ? p->x : 0, x1 = p->x + p->w < p->dst->w ? p->x + p->w : p->dst->w;
    if (py >= 0 && py < p->dst->h && x0 < x1)
        memcpy(img_at(p->dst, x0, py), row + (size_t)(x0 - p->x) * 4, (size_t)(x1 - x0) * 4);
}

img *change_image(const change *c, int after)
{
    img *im = img_new(after ? c->aw : c->bw, after ? c->ah : c->bh, (rgba){ 0, 0, 0, 0 });
    if (im)
    {
        put_ctx p = { im, 0, 0, im->w };
        change_rows(c, after ? 2 : 1, put_row, &p);
    }
    return im;
}

void change_apply(const change *c, img **picture, int undo)
{
    if (c->kind == CHANGE_PATCH)
    {
        put_ctx p = { *picture, c->x, c->y, c->bw };
        change_rows(c, undo ? 1 : 2, put_row, &p);
        return;
    }
    img *now = change_image(c, !undo);
    if (now)
    {
        img_free(*picture);
        *picture = now;
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
    h->undo_bytes += c->bytes;
    size_t drop = 0;
    while (h->undo.n - drop > 1 && h->undo_bytes > BUDGET_BYTES)
    {
        h->undo_bytes -= h->undo.v[drop].c->bytes;
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
        h->undo_bytes -= e.c->bytes;
    else
        h->undo_bytes += e.c->bytes;
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
