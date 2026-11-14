/*
 * Paint (C port) - what the kids' apps say to the host's hub, and what it says
 * back (Rust: collab/protocol.rs)
 *
 * Messages travel as bincode 2 (`config::standard()`), byte for byte as the
 * Rust Paint writes them, so the two can draw together: integers are varints
 * (one byte below 251; else 251/252/253 then 2/4/8 little-endian bytes), signed
 * ones zigzagged first; strings and vectors are a varint length then the items;
 * fixed arrays are just their bytes; an Option is a 0/1 byte; an enum is its
 * variant number as a varint, then its fields; f32 is 4 little-endian bytes.
 *
 * The message structs are flat (every field of every kind, only those of
 * `kind` used) and own their buffers; a zeroed one is empty and safe to free.
 */
#ifndef COLLAB_PROTOCOL_H
#define COLLAB_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include "collab/patch.h"

typedef uint32_t peer_id;

/* Bumped whenever the messages change; apps only talk to the same version */
#define COLLAB_VERSION 2
/* The first thing a kid's app sends, so stray connections are turned away */
#define HELLO_MAGIC "PNT!"
/* Side length of a preview tile */
#define TILE 64
/* The most tiles one preview may carry */
#define MAX_TILES 4096

/* A growable byte buffer (what the encoders write into) */
typedef struct
{
    uint8_t *p;
    size_t len, cap;
} bytes;

void bytes_put(bytes *b, const void *data, size_t n);
void bytes_free(bytes *b);

/* The first thing a kid's app sends after connecting */
typedef struct
{
    uint8_t magic[4];
    uint32_t version;
    char *name;
    uint8_t color[3];
    uint16_t listen;        /* the port this app keeps ready, to take over as host if the host leaves */
} hello;

/* Someone in the room */
typedef struct
{
    peer_id id;
    char *name;
    uint8_t color[3];
    char *addr;             /* where their app can be reached if it takes over as host ("ip:port") */
} kid;

typedef struct
{
    kid *v;
    int n;
} kid_list;

/* Part of a drawing still in progress: the pixels of one 64x64 tile that differ
 * from the shared picture (none: the tile is back to normal) */
typedef struct
{
    uint16_t tx, ty;
    int has_patch;
    patch patch;
} tile;

typedef struct
{
    tile *v;
    int n;
} tile_list;

/* The tool a kid is holding, for drawing their pointer */
enum
{
    HELD_PENCIL,
    HELD_FILL,
    HELD_BRUSH,
    HELD_ERASER,
    HELD_PICKER,
    HELD_OTHER,
};

/* Where a kid's pointer is and what they're holding */
typedef struct
{
    int has_at;             /* the pointer is over the picture, at x y (picture pixels) */
    float x, y;
    int tool;               /* HELD_* */
    uint8_t color[4];       /* Color 1 (for the eraser, Color 2: what it paints with) */
    float size;             /* the eraser's size, in picture pixels */
} presence;

/* From a kid's app to the hub */
enum
{
    TO_EDIT,                /* id, edit: a finished change; id counts up per sender so the echo can be matched */
    TO_PREVIEW,             /* tiles: what I'm drawing right now (passed on, not kept) */
    TO_PRESENCE,            /* presence */
    TO_PING,                /* still here (sent when there's nothing else to say) */
    TO_KICK,                /* kid: from the host, send this kid out */
    TO_LET_IN,              /* kid, yes: from the host, let a waiting kid in, or not */
    TO_ASK_FIRST,           /* yes: from the host, whether newcomers wait to be let in */
};

typedef struct
{
    int kind;
    uint64_t id;
    edit edit;
    tile_list tiles;
    presence presence;
    peer_id kid;
    int yes;
} to_hub;

/* From the hub to the kids' apps */
enum
{
    FROM_WELCOME,           /* you, host, seq, picture, kids, yes (ask first): you're in */
    FROM_SEQUENCED,         /* seq, from, id, edit: an edit in its final place in the order (to its author too) */
    FROM_KIDS,              /* kids: who's in the room now */
    FROM_KICKED_OUT,        /* kid, by, kids: the host sent `kid` out; `kids` is who's still in */
    FROM_ASKING,            /* text: you're waiting for the host (named) to let you in */
    FROM_WAITING,           /* kids: to the host, the kids waiting to be let in, earliest first */
    FROM_ASK_FIRST,         /* yes: the host changed whether newcomers wait */
    FROM_REJECTED,          /* text: not let in (a different version, or the host said no) */
    FROM_PREVIEW,           /* from, tiles: what a friend is drawing right now */
    FROM_PRESENCE,          /* from, presence */
    FROM_PING,              /* still here */
};

typedef struct
{
    int kind;
    peer_id you, host, from, kid, by;
    uint64_t seq, id;
    picture picture;
    kid_list kids;
    int yes;
    edit edit;
    char *text;
    tile_list tiles;
    presence presence;
} from_hub;

void kid_free(kid *k);
kid kid_clone(const kid *k);
void kid_list_free(kid_list *l);
kid_list kid_list_clone(const kid_list *l);
void kid_list_push(kid_list *l, kid k);             /* takes ownership */
void tile_list_free(tile_list *l);
void hello_free(hello *h);
void to_hub_free(to_hub *m);
void from_hub_free(from_hub *m);
from_hub from_hub_clone(const from_hub *m);
to_hub to_hub_clone(const to_hub *m);

/* The picture pixels a tile covers */
irect tile_rect(uint16_t tx, uint16_t ty);

/* Checks on what arrives (decoding already checked the layout) */
int to_hub_valid(const to_hub *m);
int from_hub_valid(const from_hub *m);

/* bincode: the encoders append to `out`; the decoders take exactly `len` bytes
 * (anything malformed, over the 128 MB limit, or with bytes left over fails)
 * and return 0, or -1 with *out zeroed */
void hello_encode(const hello *h, bytes *out);
void to_hub_encode(const to_hub *m, bytes *out);
void from_hub_encode(const from_hub *m, bytes *out);
int hello_decode(const uint8_t *data, size_t len, hello *out);
int to_hub_decode(const uint8_t *data, size_t len, to_hub *out);
int from_hub_decode(const uint8_t *data, size_t len, from_hub *out);

/* Decoding gives up once it has claimed this much, as the Rust does (its MAX_FRAME) */
#define WIRE_LIMIT ((size_t)128 << 20)

#endif
