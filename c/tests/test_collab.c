/*
 * Paint (C port) - tests of drawing together without the network: messages and
 * their bincode bytes, frames, patches, the hub's door, and sessions in a room
 * whose messages are routed by hand (Rust: collab tests in protocol/net/hub.rs)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "collab/hub.h"
#include "collab/names.h"
#include "collab/net.h"
#include "collab/session.h"
#include "paint/raster.h"

static int failures;
#define CHECK(c)                                                                  \
    do                                                                            \
    {                                                                             \
        if (!(c))                                                                 \
        {                                                                         \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                           \
        }                                                                         \
    } while (0)

static char *dup(const char *s)
{
    return strcpy(malloc(strlen(s) + 1), s);
}

static kid mk_kid(peer_id id, const char *name, const char *addr)
{
    kid k = { id, dup(name), { 1, 2, 3 }, dup(addr) };
    return k;
}

static int px_9(int x, int y, uint8_t px[4], void *user)
{
    memcpy(px, user, 4);
    return 1;
}

static int px_even(int x, int y, uint8_t px[4], void *user)
{
    static const uint8_t c[4] = { 1, 2, 3, 4 };
    memcpy(px, c, 4);
    return x % 2 == 0;
}

static patch square(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t c[4] = { r, g, b, 255 };
    patch p;
    patch_build(irect_make(x0, y0, x1, y1), px_9, c, &p);
    return p;
}

static void to_hex(const bytes *b, char *out)
{
    for (size_t i = 0; i < b->len; i++)
        snprintf(out + 2 * i, 3, "%02x", b->p[i]);
}

/* Encode, decode, encode again: the same bytes, and (when given) the expected ones */
static void round_trip_to(const to_hub *m, const char *hex)
{
    bytes a = { 0 }, b = { 0 };
    to_hub back;
    to_hub_encode(m, &a);
    CHECK(to_hub_decode(a.p, a.len, &back) == 0);
    to_hub_encode(&back, &b);
    CHECK(a.len == b.len && !memcmp(a.p, b.p, a.len));
    if (hex)
    {
        char got[4096];
        to_hex(&a, got);
        if (strcmp(got, hex))
            fprintf(stderr, "to_hub %d: %s, want %s\n", m->kind, got, hex);
        CHECK(!strcmp(got, hex));
    }
    /* Anything cut short, or with a byte left over, is refused */
    to_hub junk;
    CHECK(to_hub_decode(a.p, a.len - 1, &junk) == -1);
    bytes_put(&a, "", 1);
    CHECK(to_hub_decode(a.p, a.len, &junk) == -1);
    to_hub_free(&back);
    bytes_free(&a);
    bytes_free(&b);
}

static void round_trip_from(const from_hub *m, const char *hex)
{
    bytes a = { 0 }, b = { 0 };
    from_hub back;
    from_hub_encode(m, &a);
    CHECK(from_hub_decode(a.p, a.len, &back) == 0);
    from_hub_encode(&back, &b);
    CHECK(a.len == b.len && !memcmp(a.p, b.p, a.len));
    if (hex)
    {
        char got[4096];
        to_hex(&a, got);
        if (strcmp(got, hex))
            fprintf(stderr, "from_hub %d: %s, want %s\n", m->kind, got, hex);
        CHECK(!strcmp(got, hex));
    }
    from_hub junk;
    CHECK(from_hub_decode(a.p, a.len - 1, &junk) == -1);
    from_hub_free(&back);
    bytes_free(&a);
    bytes_free(&b);
}

static void test_bincode(void)
{
    /* Bytes worked out by hand from bincode's rules (and checked against the Rust Paint) */
    to_hub m = { 0 };
    m.kind = TO_PING;
    round_trip_to(&m, "03");
    m.kind = TO_KICK;
    m.kid = 300;                        /* 251, then 300 little-endian */
    round_trip_to(&m, "04fb2c01");
    m.kind = TO_LET_IN;
    m.kid = 7;
    m.yes = 1;
    round_trip_to(&m, "050701");
    m.kind = TO_ASK_FIRST;
    m.yes = 0;
    round_trip_to(&m, "0600");
    memset(&m, 0, sizeof m);
    m.kind = TO_PRESENCE;
    m.presence = (presence){ 1, 1.5f, -2.0f, HELD_ERASER, { 1, 2, 3, 4 }, 12.0f };
    round_trip_to(&m, "02010000c03f000000c0030102030400004041");
    m.presence.has_at = 0;
    m.presence.x = m.presence.y = 0;
    round_trip_to(&m, "0200030102030400004041");

    /* A patch at negative coordinates (zigzag), with a big id (u64 varint) */
    memset(&m, 0, sizeof m);
    m.kind = TO_EDIT;
    m.id = 0x123456789ull;
    m.edit.kind = EDIT_PATCH;
    m.edit.p = square(-3, 70000, -1, 70001, 9, 8, 7);
    round_trip_to(&m, "00fd896745230100000000" "05fce02202000201010308090807ff090807ff");
    to_hub_free(&m);

    m.kind = TO_PREVIEW;
    m.tiles.n = 2;
    m.tiles.v = calloc(2, sizeof(tile));
    m.tiles.v[0] = (tile){ 1, 2, 1, square(64, 128, 66, 129, 5, 5, 5) };
    m.tiles.v[1] = (tile){ 3, 4, 0, { 0 } };
    round_trip_to(&m, "0102""010201""80fb00010201010308050505ff050505ff""030400");
    to_hub_free(&m);

    from_hub f = { 0 };
    f.kind = FROM_PING;
    round_trip_from(&f, "0a");
    f.kind = FROM_ASKING;
    f.text = dup("Héllo");
    round_trip_from(&f, "040648c3a96c6c6f");
    from_hub_free(&f);
    f.kind = FROM_WELCOME;
    f.you = 2;
    f.host = 1;
    f.seq = 5;
    f.picture = (picture){ 1, 1, (uint8_t *)dup("abcd"), 4 };
    f.yes = 1;
    kid_list_push(&f.kids, mk_kid(1, "Host", "10.0.0.1:47100"));
    round_trip_from(&f, "00020105010104616263640101" "04486f7374" "010203" "0e31302e302e302e313a3437313030" "01");
    from_hub_free(&f);
    f.kind = FROM_SEQUENCED;
    f.seq = 1;
    f.from = 2;
    f.id = 1;
    f.edit.kind = EDIT_REPLACE;
    f.edit.pic = (picture){ 1, 1, (uint8_t *)dup("wxyz"), 4 };
    round_trip_from(&f, "0101020101010104" "7778797a");
    from_hub_free(&f);
    f.kind = FROM_KICKED_OUT;
    f.kid = 3;
    f.by = 1;
    round_trip_from(&f, "03030100");
    f.kind = FROM_PRESENCE;
    f.from = 4;
    f.presence = (presence){ 0, 0, 0, HELD_OTHER, { 0, 0, 0, 0 }, 0 };
    round_trip_from(&f, "0904000500000000" "00000000");
    memset(&f, 0, sizeof f);

    /* The decoders refuse what bincode refuses */
    static const uint8_t bad_bool[] = { 6, 2 }, bad_kind[] = { 7 }, bad_held[] = { 2, 0, 6, 0, 0, 0, 0, 0, 0, 0, 0 },
                         bad_utf8[] = { 4, 2, 0xC3, 0x28 }, wide_u16[] = { 1, 1, 252, 1, 0, 0, 0, 0, 0 },
                         huge_vec[] = { 2, 253, 0, 0, 0, 0, 0, 1, 0, 0 };
    to_hub t;
    from_hub g;
    CHECK(to_hub_decode(bad_bool, sizeof bad_bool, &t) == -1);
    CHECK(to_hub_decode(bad_kind, sizeof bad_kind, &t) == -1);
    CHECK(to_hub_decode(bad_held, sizeof bad_held, &t) == -1);
    CHECK(from_hub_decode(bad_utf8, sizeof bad_utf8, &g) == -1);
    CHECK(to_hub_decode(wide_u16, sizeof wide_u16, &t) == -1);   /* a tile's tx is a u16: no 4-byte form */
    CHECK(from_hub_decode(huge_vec, sizeof huge_vec, &g) == -1); /* claims far past the limit */
}

static void test_frames(void)
{
    /* Round trip, and big ones are compressed */
    from_hub w = { 0 };
    w.kind = FROM_WELCOME;
    w.you = 3;
    w.host = 1;
    w.seq = 9;
    w.picture.w = 300;
    w.picture.h = 200;
    w.picture.len = 300 * 200 * 4;
    w.picture.rgba = malloc(w.picture.len);
    memset(w.picture.rgba, 255, w.picture.len);
    bytes raw = { 0 }, again = { 0 };
    from_hub_encode(&w, &raw);
    size_t len;
    uint8_t *frame = net_frame(&raw, &len);
    CHECK(frame && len < 10000);
    CHECK(frame[4] == 1);
    struct
    {
        const uint8_t *p;
        size_t left;
    } src = { frame, len };
    int read_mem(void *ctx, void *buf, int n);
    uint8_t *body;
    size_t blen;
    CHECK(net_read_frame(read_mem, &src, &body, &blen) == 0);
    from_hub back;
    CHECK(from_hub_decode(body, blen, &back) == 0);
    from_hub_encode(&back, &again);
    from_hub_encode(&w, &raw);
    CHECK(again.len == raw.len && !memcmp(again.p, raw.p, raw.len));
    free(body);
    free(frame);
    from_hub_free(&back);
    from_hub_free(&w);
    bytes_free(&raw);
    bytes_free(&again);

    /* A small one isn't */
    to_hub e = { 0 };
    e.kind = TO_EDIT;
    e.id = 1;
    e.edit.kind = EDIT_PATCH;
    patch_build(irect_make(5, 5, 9, 7), px_even, NULL, &e.edit.p);
    to_hub_encode(&e, &raw);
    size_t raw_len = raw.len;
    frame = net_frame(&raw, &len);
    CHECK(frame[4] == 0 && len == raw_len + 5 && frame[3] == raw_len + 1);
    src.p = frame;
    src.left = len;
    CHECK(net_read_frame(read_mem, &src, &body, &blen) == 0);
    to_hub te;
    CHECK(to_hub_decode(body, blen, &te) == 0 && to_hub_valid(&te) && patch_eq(&te.edit.p, &e.edit.p));
    free(body);
    free(frame);
    to_hub_free(&te);
    to_hub_free(&e);

    /* Bad frames are refused: claims to be enormous; garbage body */
    static const uint8_t huge[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0 }, garbage[] = { 0, 0, 0, 3, 0, 9, 9 }, flag[] = { 0, 0, 0, 2, 7, 3 };
    src.p = huge;
    src.left = sizeof huge;
    CHECK(net_read_frame(read_mem, &src, &body, &blen) == -1);
    src.p = garbage;
    src.left = sizeof garbage;
    CHECK(net_read_frame(read_mem, &src, &body, &blen) == 0 && to_hub_decode(body, blen, &te) == -1);
    free(body);
    src.p = flag;
    src.left = sizeof flag;
    CHECK(net_read_frame(read_mem, &src, &body, &blen) == -1);

    /* A patch whose pixels don't match its mask decodes, but isn't valid */
    e = (to_hub){ 0 };
    e.kind = TO_EDIT;
    e.edit.p = (patch){ 0, 0, 4, 4, (uint8_t *)dup("\xFF\xFF"), 2, calloc(8, 1), 8 };
    CHECK(!to_hub_valid(&e));
    to_hub_free(&e);
    e.edit.kind = EDIT_REPLACE;
    e.edit.pic = (picture){ 10, 10, calloc(12, 1), 12 };
    CHECK(!to_hub_valid(&e));
    to_hub_free(&e);
    /* Preview tiles must stay inside their tile */
    e.kind = TO_PREVIEW;
    e.tiles.n = 1;
    e.tiles.v = calloc(1, sizeof(tile));
    e.tiles.v[0] = (tile){ 0, 0, 1, square(60, 0, 70, 2, 1, 1, 1) };
    CHECK(!to_hub_valid(&e));
    to_hub_free(&e);
}

int read_mem(void *ctx, void *buf, int n)
{
    struct
    {
        const uint8_t *p;
        size_t left;
    } *src = ctx;
    if ((size_t)n > src->left)
        return -1;
    memcpy(buf, src->p, (size_t)n);
    src->p += n;
    src->left -= (size_t)n;
    return 0;
}

static img *white(int w, int h)
{
    return img_new(w, h, (rgba){ 255, 255, 255, 255 });
}

static void count_px(int x, int y, const uint8_t px[4], void *user)
{
    (*(int *)user)++;
}

static void test_patches(void)
{
    img *before = white(8, 8), *after = img_dup(before);
    memcpy(img_at(after, 2, 3), "\x01\x02\x03\xFF", 4);
    memcpy(img_at(after, 5, 5), "\x04\x05\x06\xFF", 4);
    patch p;
    CHECK(patch_diff(before, after, irect_make(0, 0, 8, 8), &p) == 1);
    CHECK(patch_valid(&p) && p.pixels_len == 8);
    int n = 0;
    patch_each(&p, count_px, &n);
    CHECK(n == 2);
    img *copy = white(8, 8);
    patch_apply_within(&p, copy, irect_make(0, 0, 4, 8));      /* only the left half */
    CHECK(!memcmp(img_at(copy, 2, 3), "\x01\x02\x03\xFF", 4) && !memcmp(img_at(copy, 5, 5), "\xFF\xFF\xFF\xFF", 4));
    patch_free(&p);
    CHECK(patch_diff(before, before, irect_make(0, 0, 8, 8), &p) == 0);

    /* A change made by me, then a friend paints over part of it: undo restores only my own pixels */
    img *mine = white(8, 8);
    raster_fill_rect(mine, irect_make(2, 3, 6, 4), (rgba){ 9, 9, 9, 255 });
    change *c = change_patch(before, mine, irect_make(2, 3, 6, 4));
    edit e;
    CHECK(edit_from_change(c, &e) == 1 && e.kind == EDIT_PATCH && e.p.pixels_len == 16);
    img *current = white(8, 8);
    edit_apply(&e, &current);
    memcpy(img_at(current, 3, 3), "\x00\x00\x00\xFF", 4);       /* the friend's pixel */
    edit undo;
    CHECK(collab_revert(c, current, 1, &undo) == 1);
    edit_apply(&undo, &current);
    CHECK(!memcmp(img_at(current, 2, 3), "\xFF\xFF\xFF\xFF", 4) && !memcmp(img_at(current, 3, 3), "\x00\x00\x00\xFF", 4));
    CHECK(!memcmp(img_at(current, 5, 3), "\xFF\xFF\xFF\xFF", 4));
    region g = edit_region(&e, current);
    CHECK(!g.whole && g.r.x0 == 2 && g.r.x1 == 6 && g.r.y0 == 3 && g.r.y1 == 4);
    edit_free(&undo);
    edit_free(&e);
    change_free(c);
    img_free(mine);
    img_free(current);
    img_free(copy);
    img_free(before);
    img_free(after);
}

/* ---- the hub ---- */
static int welcomed(const hub_out *o, peer_id id)
{
    for (int i = 0; i < o->n; i++)
        if (o->v[i].to == TO_ONE && o->v[i].id == id && o->v[i].msg.kind == FROM_WELCOME)
            return 1;
    return 0;
}

static hub *door_hub(int ask_first)
{
    hub *h = hub_new(img_new(8, 8, (rgba){ 0, 0, 0, 0 }));
    hub_set_door(h, ask_first, (kid_list){ 0 });
    hub_out o = { 0 };
    hub_join(h, 1, "Host", (uint8_t[3]){ 0 }, "10.0.0.1:47100", &o);
    hub_out_free(&o);
    return h;
}

static void handle(hub *h, peer_id from, int kind, peer_id kid, int yes, hub_out *o)
{
    to_hub m = { 0 };
    m.kind = kind;
    m.kid = kid;
    m.yes = yes;
    hub_handle(h, from, &m, o);
}

static void test_hub(void)
{
    static const uint8_t c0[3] = { 0 };
    hub_out o = { 0 };

    /* A kicked kid has to ask to come back */
    hub *h = door_hub(0);
    hub_join(h, 2, "Fox", c0, "10.0.0.2:5000", &o);
    CHECK(welcomed(&o, 2));
    hub_out_free(&o);
    handle(h, 1, TO_KICK, 2, 0, &o);
    CHECK(o.n == 2 && o.v[0].to == TO_ONE && o.v[0].id == 2 && o.v[0].msg.kind == FROM_KICKED_OUT && o.v[0].msg.kid == 2
          && o.v[0].msg.by == 1 && o.v[0].msg.kids.n == 1);
    CHECK(!hub_knows(h, 2));
    hub_out_free(&o);
    hub_join(h, 3, "Not Fox", c0, "10.0.0.2:5001", &o);     /* from the same computer, under any name */
    CHECK(o.v[0].to == TO_ONE && o.v[0].id == 3 && o.v[0].msg.kind == FROM_ASKING && !strcmp(o.v[0].msg.text, "Host"));
    CHECK(o.v[1].to == TO_ONE && o.v[1].id == 1 && o.v[1].msg.kind == FROM_WAITING && o.v[1].msg.kids.n == 1);
    hub_out_free(&o);
    hub_join(h, 4, "Owl", c0, "10.0.0.4:5000", &o);         /* anyone else still comes straight in */
    CHECK(welcomed(&o, 4));
    hub_out_free(&o);
    hub_free(h);

    /* Only the host kicks and lets in */
    h = door_hub(1);
    hub_join(h, 2, "Fox", c0, "10.0.0.2:5000", &o);
    handle(h, 1, TO_LET_IN, 2, 1, &o);
    hub_join(h, 3, "Owl", c0, "10.0.0.3:5000", &o);
    hub_out_free(&o);
    handle(h, 2, TO_LET_IN, 3, 1, &o);
    CHECK(o.n == 0);
    handle(h, 2, TO_KICK, 1, 0, &o);
    CHECK(o.n == 0);
    handle(h, 1, TO_KICK, 1, 0, &o);                        /* not the host themselves */
    CHECK(o.n == 0);
    handle(h, 3, TO_PING, 0, 0, &o);
    CHECK(o.n == 0 && !hub_is_in(h, 3));                    /* still waiting */
    handle(h, 1, TO_LET_IN, 3, 0, &o);
    CHECK(o.v[0].to == TO_ONE && o.v[0].id == 3 && o.v[0].msg.kind == FROM_REJECTED
          && !strcmp(o.v[0].msg.text, "Host didn't let you in this time."));
    CHECK(!hub_knows(h, 3));
    hub_out_free(&o);
    hub_free(h);

    /* Letting friends in without asking lets in those waiting */
    h = door_hub(1);
    hub_join(h, 2, "Fox", c0, "10.0.0.2:5000", &o);
    hub_join(h, 3, "Owl", c0, "10.0.0.3:5000", &o);
    hub_leave(h, 3, &o);
    hub_out_free(&o);
    handle(h, 1, TO_ASK_FIRST, 0, 0, &o);
    CHECK(welcomed(&o, 2) && hub_is_in(h, 2) && !hub_knows(h, 3));
    CHECK(o.v[o.n - 1].to == TO_ONE && o.v[o.n - 1].id == 1 && o.v[o.n - 1].msg.kind == FROM_WAITING && o.v[o.n - 1].msg.kids.n == 0);
    hub_out_free(&o);
    hub_free(h);

    /* After a takeover, everyone who was in comes straight back */
    kid_list known = { 0 };
    kid_list_push(&known, mk_kid(1, "Host", "10.0.0.1:47100"));
    kid_list_push(&known, mk_kid(2, "Fox", "10.0.0.2:5000"));
    kid_list_push(&known, mk_kid(3, "Owl", "10.0.0.3:5000"));
    h = hub_new(img_new(8, 8, (rgba){ 0, 0, 0, 0 }));
    hub_set_door(h, 1, known);
    hub_join(h, 1, "Fox", c0, "10.0.0.2:5000", &o);
    CHECK(welcomed(&o, 1));
    hub_out_free(&o);
    hub_join(h, 2, "Host", c0, "10.0.0.1:47101", &o);
    CHECK(welcomed(&o, 2));
    hub_out_free(&o);
    hub_join(h, 3, "Newt", c0, "10.0.0.9:5000", &o);
    CHECK(o.v[0].to == TO_ONE && o.v[0].id == 3 && o.v[0].msg.kind == FROM_ASKING);
    hub_out_free(&o);
    hub_join(h, 4, "Owl", c0, "10.0.0.8:5000", &o);
    CHECK(o.v[0].msg.kind == FROM_ASKING);
    hub_out_free(&o);
    hub_join(h, 5, "Owl", c0, "10.0.0.3:6000", &o);
    CHECK(welcomed(&o, 5) && o.v[0].msg.kind == FROM_WELCOME && o.v[0].msg.yes == 1);   /* the rule carries on */
    hub_out_free(&o);
    hub_free(h);
}

/* ---- a room routed by hand ---- */
typedef struct
{
    hub *h;
    session *s[4];
    peer_id id[4];
    from_hub down[4][64];
    int n_down[4];
    int n;
} room;

static void route(room *r, hub_out *o)
{
    for (int i = 0; i < o->n; i++)
        for (int k = 0; k < r->n; k++)
            if (hub_wants(r->h, o->v[i].to, o->v[i].id, r->id[k]))
                r->down[k][r->n_down[k]++] = from_hub_clone(&o->v[i].msg);
    hub_out_free(o);
}

/* Kid k takes in what the hub sent: a welcome starts its session */
static void deliver_all(room *r, int k)
{
    for (int i = 0; i < r->n_down[k]; i++)
    {
        from_hub *m = &r->down[k][i];
        if (m->kind == FROM_WELCOME)
        {
            img *pic = picture_to_image(&m->picture);
            session_free(r->s[k]);
            r->s[k] = session_new(m->you, m->host, m->seq, pic, m->kids);
            m->kids = (kid_list){ 0 };
            from_hub_free(m);
        }
        else if (r->s[k])
            session_receive(r->s[k], m);
        else
            from_hub_free(m);
    }
    r->n_down[k] = 0;
}

static void send_up(room *r, int k)
{
    int n;
    to_hub *out = session_take_outbox(r->s[k], &n);
    for (int i = 0; i < n; i++)
    {
        hub_out o = { 0 };
        hub_handle(r->h, r->id[k], &out[i], &o);
        route(r, &o);
    }
    free(out);
}

static void test_room(void)
{
    room r = { 0 };
    r.h = hub_new(white(100, 70));
    r.n = 3;
    for (int k = 0; k < r.n; k++)
    {
        char name[16], addr[32];
        snprintf(name, sizeof name, "Kid %d", k + 1);
        snprintf(addr, sizeof addr, "10.0.0.%d:47100", k + 1);
        r.id[k] = (peer_id)k + 1;
        hub_out o = { 0 };
        hub_join(r.h, r.id[k], name, KID_COLORS[k], addr, &o);
        route(&r, &o);
    }
    for (int k = 0; k < r.n; k++)
        deliver_all(&r, k);
    CHECK(r.s[0] && r.s[1] && r.s[2] && r.s[2]->kids.n == 3 && r.s[0]->kids.n == 3 && r.s[0]->host == 1);

    /* Kids 1 and 2 draw overlapping squares at the same time; kid 3 previews something */
    for (int k = 0; k < 2; k++)
    {
        edit e = { EDIT_PATCH, square(10 + 10 * k, 10, 40 + 10 * k, 40, (uint8_t)(50 * k), 0, 0), { 0 } };
        session_commit(r.s[k], e);
    }
    img *screen = img_dup(r.s[2]->base);
    memcpy(img_at(screen, 70, 5), "\x00\x00\xFF\xFF", 4);
    tile_list tiles;
    CHECK(session_preview_tiles(r.s[2], screen, irect_make(70, 5, 71, 6), &tiles) == 1 && tiles.n == 1 && tiles.v[0].tx == 1);
    to_hub pv = { 0 };
    pv.kind = TO_PREVIEW;
    pv.tiles = tiles;
    session_send(r.s[2], pv);
    CHECK(session_preview_tiles(r.s[2], screen, irect_make(70, 5, 71, 6), &tiles) == 0);   /* nothing new */
    presence where = { 1, 3, 4, HELD_PENCIL, { 1, 2, 3, 255 }, 0 };
    to_hub pres;
    CHECK(session_presence(r.s[2], where, &pres) == 1);
    session_send(r.s[2], pres);
    CHECK(session_presence(r.s[2], where, &pres) == 0);

    send_up(&r, 1);
    send_up(&r, 0);
    send_up(&r, 2);
    for (int k = 0; k < r.n; k++)
        deliver_all(&r, k);
    CHECK(session_has_previews(r.s[0]) && r.s[0]->overlay && !memcmp(img_at(r.s[0]->overlay, 70, 5), "\x00\x00\xFF\xFF", 4));
    CHECK(r.s[0]->n_pointers == 1 && r.s[0]->pointers[0].id == 3);
    CHECK(!session_has_previews(r.s[2]) && !r.s[2]->overlay);

    /* Kid 3 finishes: the edit replaces the preview everywhere */
    edit fin = { EDIT_PATCH, square(70, 5, 71, 6, 0, 0, 255), { 0 } };
    session_commit(r.s[2], fin);
    send_up(&r, 2);
    for (int k = 0; k < r.n; k++)
        deliver_all(&r, k);
    CHECK(!session_has_previews(r.s[0]) && !r.s[0]->overlay);

    /* Everyone matches the hub, in the order the hub chose (kid 2 first, then kid 1 on top) */
    const img *truth = hub_picture(r.h);
    for (int k = 0; k < r.n; k++)
    {
        CHECK(session_unconfirmed(r.s[k]) == 0);
        CHECK(!memcmp(session_confirmed(r.s[k])->px, truth->px, (size_t)truth->w * truth->h * 4));
        CHECK(!memcmp(r.s[k]->base->px, truth->px, (size_t)truth->w * truth->h * 4));
    }
    CHECK(!memcmp(img_at(truth, 35, 20), "\x00\x00\x00\xFF", 4));  /* kid 1's square landed last */
    CHECK(r.s[0]->foreign == 2 && r.s[2]->foreign == 2);

    /* A friend leaving takes their pointer away */
    hub_out o = { 0 };
    hub_leave(r.h, 3, &o);
    route(&r, &o);
    deliver_all(&r, 0);
    CHECK(r.s[0]->kids.n == 2 && r.s[0]->n_pointers == 0);

    img_free(screen);
    for (int k = 0; k < r.n; k++)
    {
        deliver_all(&r, k);
        session_free(r.s[k]);
    }
    hub_free(r.h);
}

static void test_names(void)
{
    char out[100];
    names_tidy("  \tBlue\x01 Fox  ", out, sizeof out);
    CHECK(!strcmp(out, "Blue Fox"));
    names_tidy("   ", out, sizeof out);
    CHECK(!strcmp(out, "Friend"));
    names_tidy("ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ", out, sizeof out);
    CHECK(strlen(out) == 40);                       /* 20 characters, 2 bytes each */
    uint8_t c[3];
    names_random(out, sizeof out, c);
    CHECK(strchr(out, ' ') != NULL);
}

int main(void)
{
    test_bincode();
    test_frames();
    test_patches();
    test_hub();
    test_room();
    test_names();
    if (failures)
    {
        fprintf(stderr, "test_collab: %d failed\n", failures);
        return 1;
    }
    printf("test_collab: ok\n");
    return 0;
}
