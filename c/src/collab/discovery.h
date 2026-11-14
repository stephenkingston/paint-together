/*
 * Paint (C port) - finding drawings on the same network with mDNS, the way
 * printers are found (Rust: collab/discovery.rs, which uses the mdns-sd crate)
 *
 * A hosted drawing is the service "paint-xxxxxxxx._paintparty._tcp.local."
 * on host "paint-xxxxxxxx.local.", with TXT name=<host kid>, kids=<count> and
 * v=<protocol version>. One thread with one UDP socket on port 5353 (shared
 * with avahi, systemd-resolved, Bonjour or another Paint) answers queries for
 * it, announces it and says goodbye, and browses for other drawings.
 */
#ifndef COLLAB_DISCOVERY_H
#define COLLAB_DISCOVERY_H

#include "os/os.h"

/* A drawing someone nearby is hosting */
typedef struct
{
    char fullname[96];      /* its name on the network */
    char name[100];         /* the host kid's name (at most 24 characters) */
    int kids;
    os_addr addr;
    int compatible;         /* made by the same version of Paint (others can't be joined) */
} nearby;

typedef struct discovery discovery;

/* NULL if mDNS can't be used here. `wake` (may be NULL) is called from the
 * discovery thread when the list of drawings nearby changes. */
discovery *discovery_new(void (*wake)(void));
void discovery_free(discovery *d);          /* stops advertising first */
/* Start looking for drawings (once) */
void discovery_browse(discovery *d);
/* Take in what the network has said; 1 if the list changed */
int discovery_poll(discovery *d);
/* The drawings found so far, as of the last poll */
const nearby *discovery_nearby(const discovery *d, int *n);
/* Tell the network about my drawing (again, when the number of kids changes) */
void discovery_advertise(discovery *d, const char *name, int kids, uint16_t port);
void discovery_stop_advertising(discovery *d);

#endif
