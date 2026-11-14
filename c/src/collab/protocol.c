/*
 * Paint (C port) - the drawing-together messages and their bincode form
 * (Rust: collab/protocol.rs, plus what the `bincode` crate does for them)
 *
 * Decoding follows bincode's limit as well as its layout: every value "claims"
 * the bytes its Rust type would occupy in memory (a u64 claims 8 even when its
 * varint is 1 byte; a vector claims its length times its item's size before any
 * item is read), and decoding fails once the claims pass 128 MB. The item sizes
 * are those of the 64-bit Rust structs (Kid: 56 bytes, Tile: 72).
 */
#include "collab/protocol.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- the byte buffer ---- */
void bytes_put(bytes *b, const void *data, size_t n)
{
    if (b->len + n > b->cap)
    {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n)
            cap *= 2;
        b->p = realloc(b->p, cap);
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

void bytes_free(bytes *b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}

/* ---- freeing and copying ---- */
static char *str_dup(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    return memcpy(malloc(n), s, n);
}

void kid_free(kid *k)
{
    free(k->name);
    free(k->addr);
    memset(k, 0, sizeof *k);
}

kid kid_clone(const kid *k)
{
    kid c = *k;
    c.name = str_dup(k->name);
    c.addr = str_dup(k->addr);
    return c;
}

void kid_list_free(kid_list *l)
{
    for (int i = 0; i < l->n; i++)
        kid_free(&l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

kid_list kid_list_clone(const kid_list *l)
{
    kid_list c = { l->n ? malloc(sizeof(kid) * (size_t)l->n) : NULL, l->n };
    for (int i = 0; i < l->n; i++)
        c.v[i] = kid_clone(&l->v[i]);
    return c;
}

void kid_list_push(kid_list *l, kid k)
{
    l->v = realloc(l->v, sizeof(kid) * (size_t)(l->n + 1));
    l->v[l->n++] = k;
}

void tile_list_free(tile_list *l)
{
    for (int i = 0; i < l->n; i++)
        patch_free(&l->v[i].patch);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static tile_list tile_list_clone(const tile_list *l)
{
    tile_list c = { l->n ? malloc(sizeof(tile) * (size_t)l->n) : NULL, l->n };
    for (int i = 0; i < l->n; i++)
    {
        c.v[i] = l->v[i];
        c.v[i].patch = patch_clone(&l->v[i].patch);
    }
    return c;
}

void hello_free(hello *h)
{
    free(h->name);
    memset(h, 0, sizeof *h);
}

void to_hub_free(to_hub *m)
{
    edit_free(&m->edit);
    tile_list_free(&m->tiles);
    memset(m, 0, sizeof *m);
}

void from_hub_free(from_hub *m)
{
    picture_free(&m->picture);
    kid_list_free(&m->kids);
    edit_free(&m->edit);
    free(m->text);
    tile_list_free(&m->tiles);
    memset(m, 0, sizeof *m);
}

from_hub from_hub_clone(const from_hub *m)
{
    from_hub c = *m;
    c.picture.rgba = NULL;
    c.picture.borrowed = 0;
    if (m->picture.len)
        c.picture.rgba = memcpy(malloc(m->picture.len), m->picture.rgba, m->picture.len);
    c.kids = kid_list_clone(&m->kids);
    c.edit = edit_clone(&m->edit);
    c.text = str_dup(m->text);
    c.tiles = tile_list_clone(&m->tiles);
    return c;
}

to_hub to_hub_clone(const to_hub *m)
{
    to_hub c = *m;
    c.edit = edit_clone(&m->edit);
    c.tiles = tile_list_clone(&m->tiles);
    return c;
}

/* ---- checks ---- */
irect tile_rect(uint16_t tx, uint16_t ty)
{
    int x = tx * TILE, y = ty * TILE;
    return irect_make(x, y, x + TILE, y + TILE);
}

static int tiles_valid(const tile_list *l)
{
    if (l->n > MAX_TILES)
        return 0;
    for (int i = 0; i < l->n; i++)
    {
        const tile *t = &l->v[i];
        if (!t->has_patch)
            continue;
        irect outer = tile_rect(t->tx, t->ty), r = patch_rect(&t->patch);
        if (!patch_valid(&t->patch) || r.x0 < outer.x0 || r.y0 < outer.y0 || r.x1 > outer.x1 || r.y1 > outer.y1)
            return 0;
    }
    return 1;
}

static int presence_valid(const presence *p)
{
    return (!p->has_at || (isfinite(p->x) && isfinite(p->y))) && p->size >= 0.0f && p->size <= 1000.0f;
}

int to_hub_valid(const to_hub *m)
{
    switch (m->kind)
    {
    case TO_EDIT:
        return edit_valid(&m->edit);
    case TO_PREVIEW:
        return tiles_valid(&m->tiles);
    case TO_PRESENCE:
        return presence_valid(&m->presence);
    default:
        return 1;
    }
}

int from_hub_valid(const from_hub *m)
{
    switch (m->kind)
    {
    case FROM_WELCOME:
        return picture_valid(&m->picture);
    case FROM_SEQUENCED:
        return edit_valid(&m->edit);
    case FROM_PREVIEW:
        return tiles_valid(&m->tiles);
    case FROM_PRESENCE:
        return presence_valid(&m->presence);
    default:
        return 1;
    }
}

/* ---- encoding ---- */
static void put_u8(bytes *b, uint8_t v)
{
    bytes_put(b, &v, 1);
}

static void put_le(bytes *b, uint64_t v, int n)
{
    uint8_t le[8];
    for (int i = 0; i < n; i++)
        le[i] = (uint8_t)(v >> (8 * i));
    bytes_put(b, le, (size_t)n);
}

static void put_var(bytes *b, uint64_t v)
{
    if (v <= 250)
        put_u8(b, (uint8_t)v);
    else if (v <= 0xFFFF)
        put_u8(b, 251), put_le(b, v, 2);
    else if (v <= 0xFFFFFFFFu)
        put_u8(b, 252), put_le(b, v, 4);
    else
        put_u8(b, 253), put_le(b, v, 8);
}

static void put_i32(bytes *b, int32_t v)
{
    put_var(b, v < 0 ? (uint64_t)(~(uint32_t)v) * 2 + 1 : (uint64_t)(uint32_t)v * 2);
}

static void put_f32(bytes *b, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    put_le(b, u, 4);
}

static void put_vec(bytes *b, const void *data, size_t n)
{
    put_var(b, n);
    bytes_put(b, data, n);
}

static void put_str(bytes *b, const char *s)
{
    put_vec(b, s ? s : "", s ? strlen(s) : 0);
}

static void put_patch(bytes *b, const patch *p)
{
    put_i32(b, p->x);
    put_i32(b, p->y);
    put_var(b, p->w);
    put_var(b, p->h);
    put_vec(b, p->mask, p->mask_len);
    put_vec(b, p->pixels, p->pixels_len);
}

static void put_picture(bytes *b, const picture *pic)
{
    put_var(b, pic->w);
    put_var(b, pic->h);
    put_vec(b, pic->rgba, pic->len);
}

static void put_edit(bytes *b, const edit *e)
{
    put_var(b, (uint64_t)e->kind);
    if (e->kind == EDIT_PATCH)
        put_patch(b, &e->p);
    else
        put_picture(b, &e->pic);
}

static void put_kids(bytes *b, const kid_list *l)
{
    put_var(b, (uint64_t)l->n);
    for (int i = 0; i < l->n; i++)
    {
        put_var(b, l->v[i].id);
        put_str(b, l->v[i].name);
        bytes_put(b, l->v[i].color, 3);
        put_str(b, l->v[i].addr);
    }
}

static void put_tiles(bytes *b, const tile_list *l)
{
    put_var(b, (uint64_t)l->n);
    for (int i = 0; i < l->n; i++)
    {
        put_var(b, l->v[i].tx);
        put_var(b, l->v[i].ty);
        put_u8(b, (uint8_t)(l->v[i].has_patch != 0));
        if (l->v[i].has_patch)
            put_patch(b, &l->v[i].patch);
    }
}

static void put_presence(bytes *b, const presence *p)
{
    put_u8(b, (uint8_t)(p->has_at != 0));
    if (p->has_at)
    {
        put_f32(b, p->x);
        put_f32(b, p->y);
    }
    put_var(b, (uint64_t)p->tool);
    bytes_put(b, p->color, 4);
    put_f32(b, p->size);
}

void hello_encode(const hello *h, bytes *out)
{
    bytes_put(out, h->magic, 4);
    put_var(out, h->version);
    put_str(out, h->name);
    bytes_put(out, h->color, 3);
    put_var(out, h->listen);
}

void to_hub_encode(const to_hub *m, bytes *out)
{
    put_var(out, (uint64_t)m->kind);
    switch (m->kind)
    {
    case TO_EDIT:
        put_var(out, m->id);
        put_edit(out, &m->edit);
        break;
    case TO_PREVIEW:
        put_tiles(out, &m->tiles);
        break;
    case TO_PRESENCE:
        put_presence(out, &m->presence);
        break;
    case TO_KICK:
        put_var(out, m->kid);
        break;
    case TO_LET_IN:
        put_var(out, m->kid);
        put_u8(out, (uint8_t)(m->yes != 0));
        break;
    case TO_ASK_FIRST:
        put_u8(out, (uint8_t)(m->yes != 0));
        break;
    }
}

void from_hub_encode(const from_hub *m, bytes *out)
{
    put_var(out, (uint64_t)m->kind);
    switch (m->kind)
    {
    case FROM_WELCOME:
        put_var(out, m->you);
        put_var(out, m->host);
        put_var(out, m->seq);
        put_picture(out, &m->picture);
        put_kids(out, &m->kids);
        put_u8(out, (uint8_t)(m->yes != 0));
        break;
    case FROM_SEQUENCED:
        put_var(out, m->seq);
        put_var(out, m->from);
        put_var(out, m->id);
        put_edit(out, &m->edit);
        break;
    case FROM_KIDS:
    case FROM_WAITING:
        put_kids(out, &m->kids);
        break;
    case FROM_KICKED_OUT:
        put_var(out, m->kid);
        put_var(out, m->by);
        put_kids(out, &m->kids);
        break;
    case FROM_ASKING:
    case FROM_REJECTED:
        put_str(out, m->text);
        break;
    case FROM_ASK_FIRST:
        put_u8(out, (uint8_t)(m->yes != 0));
        break;
    case FROM_PREVIEW:
        put_var(out, m->from);
        put_tiles(out, &m->tiles);
        break;
    case FROM_PRESENCE:
        put_var(out, m->from);
        put_presence(out, &m->presence);
        break;
    }
}

/* ---- decoding ---- */
typedef struct
{
    const uint8_t *p;
    size_t len, pos;
    uint64_t claimed;
    int bad;
} reader;

/* Sizes of the Rust types that bincode claims for each item of a vector */
#define KID_SIZE 56
#define TILE_SIZE 72

static int claim(reader *r, uint64_t n)
{
    if (n > WIRE_LIMIT || (r->claimed += n) > WIRE_LIMIT)
        r->bad = 1;
    return !r->bad;
}

static const uint8_t *take(reader *r, size_t n)
{
    if (r->bad || r->len - r->pos < n)
    {
        r->bad = 1;
        return NULL;
    }
    r->pos += n;
    return r->p + r->pos - n;
}

static uint64_t get_le(reader *r, int n)
{
    const uint8_t *s = take(r, (size_t)n);
    uint64_t v = 0;
    for (int i = 0; s && i < n; i++)
        v |= (uint64_t)s[i] << (8 * i);
    return v;
}

static uint8_t get_u8(reader *r)
{
    claim(r, 1);
    return (uint8_t)get_le(r, 1);
}

static int get_bool(reader *r)
{
    uint8_t v = get_u8(r);
    if (v > 1)
        r->bad = 1;
    return v;
}

/* A varint of a type `size` bytes wide (2, 4 or 8): wider encodings are refused */
static uint64_t get_var(reader *r, int size)
{
    claim(r, (uint64_t)size);
    uint8_t tag = (uint8_t)get_le(r, 1);
    if (tag <= 250)
        return tag;
    if (tag == 251)
        return get_le(r, 2);
    if (tag == 252 && size >= 4)
        return get_le(r, 4);
    if (tag == 253 && size >= 8)
        return get_le(r, 8);
    r->bad = 1;
    return 0;
}

static int32_t get_i32(reader *r)
{
    uint32_t n = (uint32_t)get_var(r, 4);
    return (int32_t)(n & 1 ? ~(n >> 1) : n >> 1);
}

static float get_f32(reader *r)
{
    claim(r, 4);
    uint32_t u = (uint32_t)get_le(r, 4);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void get_array(reader *r, uint8_t *out, size_t n)
{
    claim(r, n);
    const uint8_t *s = take(r, n);
    if (s)
        memcpy(out, s, n);
}

/* A vector's length, with `item` bytes claimed for each of its items */
static size_t get_len(reader *r, uint64_t item)
{
    uint64_t n = get_var(r, 8);
    if (r->bad || (item && n > WIRE_LIMIT / item) || !claim(r, n * item))
    {
        r->bad = 1;
        return 0;
    }
    return (size_t)n;
}

static uint8_t *get_vec(reader *r, size_t *len)
{
    size_t n = get_len(r, 1);
    const uint8_t *s = take(r, n);
    *len = s ? n : 0;
    if (!s || !n)
        return NULL;
    uint8_t *d = malloc(n);
    if (!d)
    {
        r->bad = 1;
        *len = 0;
        return NULL;
    }
    return memcpy(d, s, n);
}

/* Strict UTF-8, as Rust's String::from_utf8 wants it */
static int utf8_ok(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n)
    {
        uint8_t c = s[i];
        size_t k;
        uint32_t cp, min;
        if (c < 0x80)
        {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0)
            k = 1, cp = c & 0x1F, min = 0x80;
        else if ((c & 0xF0) == 0xE0)
            k = 2, cp = c & 0x0F, min = 0x800;
        else if ((c & 0xF8) == 0xF0)
            k = 3, cp = c & 0x07, min = 0x10000;
        else
            return 0;
        if (n - i <= k)
            return 0;
        for (size_t j = 1; j <= k; j++)
        {
            if ((s[i + j] & 0xC0) != 0x80)
                return 0;
            cp = cp << 6 | (s[i + j] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += k + 1;
    }
    return 1;
}

static char *get_str(reader *r)
{
    size_t n = get_len(r, 1);
    const uint8_t *s = take(r, n);
    if (!s || !utf8_ok(s, n))
    {
        r->bad = 1;
        return NULL;
    }
    char *d = malloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static void get_patch(reader *r, patch *p)
{
    p->x = get_i32(r);
    p->y = get_i32(r);
    p->w = (uint32_t)get_var(r, 4);
    p->h = (uint32_t)get_var(r, 4);
    p->mask = get_vec(r, &p->mask_len);
    p->pixels = get_vec(r, &p->pixels_len);
}

static void get_picture(reader *r, picture *pic)
{
    pic->w = (uint32_t)get_var(r, 4);
    pic->h = (uint32_t)get_var(r, 4);
    pic->rgba = get_vec(r, &pic->len);
}

static void get_edit(reader *r, edit *e)
{
    uint64_t kind = get_var(r, 4);
    e->kind = (int)kind;
    if (kind == EDIT_PATCH)
        get_patch(r, &e->p);
    else if (kind == EDIT_REPLACE)
        get_picture(r, &e->pic);
    else
        r->bad = 1;
}

static void get_kids(reader *r, kid_list *l)
{
    size_t n = get_len(r, KID_SIZE);
    if (r->bad || !n)
        return;
    l->v = calloc(n, sizeof(kid));
    for (size_t i = 0; i < n && !r->bad; i++)
    {
        kid *k = &l->v[l->n++];
        r->claimed -= KID_SIZE;
        k->id = (peer_id)get_var(r, 4);
        k->name = get_str(r);
        get_array(r, k->color, 3);
        k->addr = get_str(r);
    }
}

static void get_tiles(reader *r, tile_list *l)
{
    size_t n = get_len(r, TILE_SIZE);
    if (r->bad || !n)
        return;
    l->v = calloc(n, sizeof(tile));
    for (size_t i = 0; i < n && !r->bad; i++)
    {
        tile *t = &l->v[l->n++];
        r->claimed -= TILE_SIZE;
        t->tx = (uint16_t)get_var(r, 2);
        t->ty = (uint16_t)get_var(r, 2);
        t->has_patch = get_bool(r);
        if (t->has_patch)
            get_patch(r, &t->patch);
    }
}

static void get_presence(reader *r, presence *p)
{
    p->has_at = get_bool(r);
    if (p->has_at)
    {
        p->x = get_f32(r);
        p->y = get_f32(r);
    }
    uint64_t tool = get_var(r, 4);
    if (tool > HELD_OTHER)
        r->bad = 1;
    p->tool = (int)tool;
    get_array(r, p->color, 4);
    p->size = get_f32(r);
}

/* Whether the whole input was one good message */
static int finished(reader *r)
{
    return !r->bad && r->pos == r->len;
}

int hello_decode(const uint8_t *data, size_t len, hello *out)
{
    reader r = { data, len, 0, 0, 0 };
    memset(out, 0, sizeof *out);
    get_array(&r, out->magic, 4);
    out->version = (uint32_t)get_var(&r, 4);
    out->name = get_str(&r);
    get_array(&r, out->color, 3);
    out->listen = (uint16_t)get_var(&r, 2);
    if (finished(&r))
        return 0;
    hello_free(out);
    return -1;
}

int to_hub_decode(const uint8_t *data, size_t len, to_hub *out)
{
    reader r = { data, len, 0, 0, 0 };
    memset(out, 0, sizeof *out);
    uint64_t kind = get_var(&r, 4);
    out->kind = (int)kind;
    switch (kind)
    {
    case TO_EDIT:
        out->id = get_var(&r, 8);
        get_edit(&r, &out->edit);
        break;
    case TO_PREVIEW:
        get_tiles(&r, &out->tiles);
        break;
    case TO_PRESENCE:
        get_presence(&r, &out->presence);
        break;
    case TO_PING:
        break;
    case TO_KICK:
        out->kid = (peer_id)get_var(&r, 4);
        break;
    case TO_LET_IN:
        out->kid = (peer_id)get_var(&r, 4);
        out->yes = get_bool(&r);
        break;
    case TO_ASK_FIRST:
        out->yes = get_bool(&r);
        break;
    default:
        r.bad = 1;
    }
    if (finished(&r))
        return 0;
    to_hub_free(out);
    return -1;
}

int from_hub_decode(const uint8_t *data, size_t len, from_hub *out)
{
    reader r = { data, len, 0, 0, 0 };
    memset(out, 0, sizeof *out);
    uint64_t kind = get_var(&r, 4);
    out->kind = (int)kind;
    switch (kind)
    {
    case FROM_WELCOME:
        out->you = (peer_id)get_var(&r, 4);
        out->host = (peer_id)get_var(&r, 4);
        out->seq = get_var(&r, 8);
        get_picture(&r, &out->picture);
        get_kids(&r, &out->kids);
        out->yes = get_bool(&r);
        break;
    case FROM_SEQUENCED:
        out->seq = get_var(&r, 8);
        out->from = (peer_id)get_var(&r, 4);
        out->id = get_var(&r, 8);
        get_edit(&r, &out->edit);
        break;
    case FROM_KIDS:
    case FROM_WAITING:
        get_kids(&r, &out->kids);
        break;
    case FROM_KICKED_OUT:
        out->kid = (peer_id)get_var(&r, 4);
        out->by = (peer_id)get_var(&r, 4);
        get_kids(&r, &out->kids);
        break;
    case FROM_ASKING:
    case FROM_REJECTED:
        out->text = get_str(&r);
        break;
    case FROM_ASK_FIRST:
        out->yes = get_bool(&r);
        break;
    case FROM_PREVIEW:
        out->from = (peer_id)get_var(&r, 4);
        get_tiles(&r, &out->tiles);
        break;
    case FROM_PRESENCE:
        out->from = (peer_id)get_var(&r, 4);
        get_presence(&r, &out->presence);
        break;
    case FROM_PING:
        break;
    default:
        r.bad = 1;
    }
    if (finished(&r))
        return 0;
    from_hub_free(out);
    return -1;
}
