/*
 * Paint (C port) - the room's coordinator (Rust: collab/hub.rs)
 *
 * Run by the host kid's app: it puts every edit in one order, passes it on to
 * everyone, and keeps the true picture for newcomers. It also keeps the door:
 * newcomers can wait for the host to let them in, and the host can send a kid
 * out. This is the pure logic; the network thread (net.c) feeds it messages,
 * and tests can route its output by hand.
 */
#ifndef COLLAB_HUB_H
#define COLLAB_HUB_H

#include "collab/protocol.h"

/* Who a message is for */
enum
{
    TO_ONE,         /* the kid `id` */
    TO_ALL,         /* everyone in the room (not those still waiting to be let in) */
    TO_OTHERS,      /* everyone in the room except the kid `id` */
    TO_EVERYONE,    /* every connection, those waiting to be let in too */
};

typedef struct
{
    int to;
    peer_id id;
    from_hub msg;
} routed;

/* What the hub says in answer to something: messages and who they're for */
typedef struct
{
    routed *v;
    int n, cap;
} hub_out;

void hub_out_push(hub_out *o, int to, peer_id id, from_hub msg);   /* takes ownership of msg */
void hub_out_free(hub_out *o);                                      /* frees the messages too */

typedef struct hub hub;

hub *hub_new(img *picture);                 /* takes ownership */
void hub_free(hub *h);
/* Whether newcomers wait to be let in, and (taking over a room) who was in it already */
void hub_set_door(hub *h, int ask_first, kid_list known);          /* takes ownership of known */
const img *hub_picture(const hub *h);
int hub_is_in(const hub *h, peer_id id);    /* in the room (not just waiting at the door) */
int hub_knows(const hub *h, peer_id id);    /* in the room or waiting to be let in */
/* Whether a message sent `to` (with its id) goes to the kid `kid` */
int hub_wants(const hub *h, int to, peer_id id, peer_id kid);

/* A kid arrives; `id` is theirs for as long as they stay. `addr` is where their
 * app could be reached to take over as host. They come in, or wait to be let in. */
void hub_join(hub *h, peer_id id, const char *name, const uint8_t color[3], const char *addr, hub_out *out);
void hub_leave(hub *h, peer_id id, hub_out *out);
void hub_handle(hub *h, peer_id from, to_hub *msg, hub_out *out);  /* takes ownership of msg's contents */

#endif
