/*
 * Paint (C port) - drawing together: one app's view of the shared picture
 * (Rust: collab/mod.rs)
 *
 * Kids on the same network share one picture in real time, with one kid's app
 * coordinating (no server, no internet). Every app keeps three pictures in step:
 * - `confirmed`: every edit the hub has put in order, applied in that order;
 * - `base`: `confirmed` plus my own edits the hub hasn't confirmed yet;
 * - the picture on screen (the app's): `base` plus whatever I'm in the middle of.
 *
 * When someone else's edit arrives, it goes under my unconfirmed edits and under
 * whatever I'm drawing right now (the app re-renders that on top), which is
 * exactly the order the hub gave everyone, so every screen ends up identical.
 *
 * A session exists only while drawing together; freeing it frees its pictures.
 */
#ifndef COLLAB_SESSION_H
#define COLLAB_SESSION_H

#include "collab/protocol.h"

/* Patches by tile, (tx, ty) -> patch (an open-addressing table) */
typedef struct
{
    uint32_t key;           /* tx << 16 | ty */
    int used;
    patch p;
} tile_slot;

typedef struct
{
    tile_slot *v;
    int cap, n;
} tile_map;

/* A friend's drawing in progress */
typedef struct
{
    peer_id id;
    tile_map tiles;
} friend_preview;

/* A friend's pointer, with when it last changed (the app's clock) */
typedef struct
{
    peer_id id;
    presence p;
    double at;
} pointer;

/* What a message from the hub changed, for the app to follow up on */
enum
{
    RECEIVED_NOTHING,
    RECEIVED_PICTURE,       /* `base` changed in `region`: rebuild the screen from it */
    RECEIVED_KIDS,
};

typedef struct
{
    int kind;
    region region;
} received;

typedef struct
{
    edit e;
    uint64_t id;
} pending_edit;

typedef struct
{
    peer_id me;
    peer_id host;           /* the kid whose app runs the room */
    kid_list kids;
    int ask_first;          /* whether newcomers wait for the host to let them in */
    kid_list waiting;       /* when hosting: kids waiting to be let in */
    img *base;              /* `confirmed` plus my unconfirmed edits */
    uint64_t foreign;       /* how many edits from other kids have arrived */
    uint64_t version;       /* changes with every edit to the picture, from anyone (for "unsaved changes") */
    /* All friends' previews drawn together, shown over the picture (transparent
     * elsewhere). Only there while some friend is drawing: NULL otherwise. */
    img *overlay;
    irect overlay_dirty;    /* where `overlay` changed since the app last looked */
    pointer *pointers;      /* friends' pointers, by id */
    int n_pointers;
    double now;             /* the app's clock, for timing pointers */

    /* private */
    img *confirmed_;
    uint64_t seq_;
    pending_edit *pending_;
    int n_pending_;
    uint64_t next_id_;
    to_hub *outbox_;
    int n_outbox_;
    friend_preview *previews_;  /* by id */
    int n_previews_;
    int ow_, oh_;           /* the overlay's size (kept while it isn't allocated) */
    tile_map sent_;         /* my drawing in progress as friends last saw it */
    int resend_all_;        /* compare every tile next time (my preview was cleared everywhere) */
    int has_last_presence_;
    presence last_presence_;
} session;

/* Takes ownership of picture and kids */
session *session_new(peer_id me, peer_id host, uint64_t seq, img *picture, kid_list kids);
void session_free(session *s);
/* The room moved to a new host: start again from its picture, with my
 * unconfirmed edits on top and sent again (my undo history stays valid) */
void session_rejoin(session *s, peer_id me, peer_id host, uint64_t seq, img *picture, kid_list kids);
/* Whether any friend is in the middle of drawing something */
int session_has_previews(const session *s);
/* The tiles of what I'm drawing right now (`screen` minus the shared picture)
 * that changed since friends last saw them, within `changed`; 0 if none */
int session_preview_tiles(session *s, const img *screen, irect changed, tile_list *out);
/* Where my pointer is: 1 and the message for the hub, if that changed */
int session_presence(session *s, presence p, to_hub *out);
/* The picture the hub has confirmed (everything except my unconfirmed edits) */
const img *session_confirmed(const session *s);
int session_unconfirmed(const session *s);  /* my edits still on their way to the hub and back */
/* A finished edit of mine (already on screen): send it, and count it as part of
 * `base` until the hub confirms it. Takes ownership. */
void session_commit(session *s, edit e);
/* Hand over what's queued for the hub (a malloc'd array, *n of them) */
to_hub *session_take_outbox(session *s, int *n);
/* Queue a message for the hub, in order with my edits (takes ownership) */
void session_send(session *s, to_hub msg);
/* Whether this message replaces the whole picture under someone else's hands;
 * the app first finishes whatever it's doing */
int session_replaces_picture(const session *s, const from_hub *msg);
/* Take in a message from the hub (its contents are used up) */
received session_receive(session *s, from_hub *msg);

#endif
