/*
 * Paint (C port) - carrying messages between the kids' apps over the local
 * network (Rust: collab/net.rs)
 *
 * Each message is one frame: a 4-byte big-endian length, a flag byte (1 =
 * raw-deflated) and the bincode body. Everything that arrives is size-limited
 * and checked before use.
 *
 * The host's app runs the hub on its own thread, with a listener thread that
 * lets friends connect and a reader and a writer thread for each friend; the
 * host's own drawing talks to that hub through a queue, so the host and its
 * friends run exactly the same code. A friend's app has a receiving and a
 * sending thread. The app thread never blocks on the network: it polls
 * link_recv, and the `wake` callback it gives (host_wake) is called from those
 * threads whenever something arrives.
 */
#ifndef COLLAB_NET_H
#define COLLAB_NET_H

#include "collab/hub.h"
#include "os/os.h"

/* The port a hosted drawing listens on (another is picked if it's taken) */
#define COLLAB_PORT 47100
/* The largest frame accepted, before or after decompression */
#define MAX_FRAME ((size_t)128 << 20)
/* Bodies at least this big are deflated */
#define COMPRESS_FROM 2048

/* How long a quiet connection waits before saying it's still there (2 s), and
 * how long without hearing anything means the other side is gone (6 s).
 * Tests shorten them. */
void net_set_timing(int ping_ms, int give_up_ms);

/* ---- frames ---- */
/* The frame for an encoded body: `raw` is used up (its buffer becomes the
 * frame's, or is replaced by its deflated form). Malloc'd, *len bytes. */
uint8_t *net_frame(bytes *raw, size_t *len);
/* Read one frame through `read` (0 ok, -1 failed: it must fill all n bytes) and
 * hand back its body, inflated: 0, or -1 if it's broken or too big */
int net_read_frame(int (*read)(void *ctx, void *buf, int n), void *ctx, uint8_t **body, size_t *len);

/* ---- the app's connection to its room ---- */
enum
{
    LINK_MESSAGE,       /* msg: from the hub */
    LINK_CLOSED,        /* why: the connection is gone (the reason is for the kid to read; may be empty) */
};

typedef struct
{
    int kind;
    from_hub msg;
    char *why;
} link_event;

void link_event_free(link_event *e);

typedef struct link link;

/* Start a room with `picture` (taken), listening for friends on the local
 * network. With ask_first, newcomers wait for me to let them in. NULL (and
 * err filled) if no port could be opened. */
link *link_host(img *picture, const char *name, const uint8_t color[3], int ask_first, void (*wake)(void), char *err, size_t cap);
/* Run `hub` (taken) on a listener that's already open (taking over as host) */
link *link_host_with(os_sock listener, hub *h, const char *name, const uint8_t color[3], void (*wake)(void));
/* Join a friend's room ("192.168.1.4", "192.168.1.4:47100" or a host name).
 * Connecting happens in the background; the result arrives as a Welcome (or
 * LINK_CLOSED) event. `spare` is a listener to keep ready for taking over as
 * host (OS_BAD_SOCK: a new one is opened). NULL (and err filled) on failure. */
link *link_join(const char *addr, const char *name, const uint8_t color[3], os_sock spare, void (*wake)(void), char *err, size_t cap);
void link_send(link *l, to_hub msg);            /* takes ownership */
int link_recv(link *l, link_event *out);        /* 1 if there was an event; never blocks */
/* When hosting: where friends can reach this drawing (0), else -1 */
int link_hosting_at(const link *l, os_addr *out);
/* The listener kept ready for taking over as host (OS_BAD_SOCK if none) */
os_sock link_take_spare(link *l);
/* Hang up: when hosting, the hub and listener stop and friends are disconnected */
void link_free(link *l);

/* A listener on the usual port, or any free one (OS_BAD_SOCK if none) */
os_sock net_listen(void);
/* This computer's address on the local network (127.0.0.1 if none) */
uint32_t net_lan_ip(void);

#endif
