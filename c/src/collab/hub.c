/*
 * Paint (C port) - the room's coordinator (Rust: collab/hub.rs)
 */
#include "collab/hub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct hub
{
    img *picture;
    uint64_t seq;
    kid_list kids;
    peer_id host;           /* the kid whose app runs this hub (the first to join it); 0 none yet */
    int has_host;
    int ask_first;          /* whether newcomers wait for the host to let them in */
    kid_list waiting;       /* kids waiting to be let in, earliest first */
    kid_list known;         /* kids in the room before this hub took it over: they come straight back in */
    char **kicked;          /* where kicked-out kids connected from: coming back, they always wait */
    int n_kicked;
};

void hub_out_push(hub_out *o, int to, peer_id id, from_hub msg)
{
    if (o->n == o->cap)
    {
        o->cap = o->cap ? o->cap * 2 : 4;
        o->v = realloc(o->v, sizeof(routed) * (size_t)o->cap);
    }
    o->v[o->n].to = to;
    o->v[o->n].id = id;
    o->v[o->n].msg = msg;
    o->n++;
}

void hub_out_free(hub_out *o)
{
    for (int i = 0; i < o->n; i++)
        from_hub_free(&o->v[i].msg);
    free(o->v);
    memset(o, 0, sizeof *o);
}

hub *hub_new(img *picture)
{
    hub *h = calloc(1, sizeof *h);
    h->picture = picture;
    return h;
}

void hub_free(hub *h)
{
    if (!h)
        return;
    img_free(h->picture);
    kid_list_free(&h->kids);
    kid_list_free(&h->waiting);
    kid_list_free(&h->known);
    for (int i = 0; i < h->n_kicked; i++)
        free(h->kicked[i]);
    free(h->kicked);
    free(h);
}

void hub_set_door(hub *h, int ask_first, kid_list known)
{
    h->ask_first = ask_first;
    kid_list_free(&h->known);
    h->known = known;
}

const img *hub_picture(const hub *h)
{
    return h->picture;
}

static int find(const kid_list *l, peer_id id)
{
    for (int i = 0; i < l->n; i++)
        if (l->v[i].id == id)
            return i;
    return -1;
}

/* Take kid i out of a list, handing it over */
static kid take(kid_list *l, int i)
{
    kid k = l->v[i];
    memmove(&l->v[i], &l->v[i + 1], sizeof(kid) * (size_t)(l->n - i - 1));
    l->n--;
    return k;
}

int hub_is_in(const hub *h, peer_id id)
{
    return find(&h->kids, id) >= 0;
}

int hub_knows(const hub *h, peer_id id)
{
    return hub_is_in(h, id) || find(&h->waiting, id) >= 0;
}

int hub_wants(const hub *h, int to, peer_id id, peer_id kid)
{
    switch (to)
    {
    case TO_ONE:
        return id == kid;
    case TO_ALL:
        return hub_is_in(h, kid);
    case TO_OTHERS:
        return id != kid && hub_is_in(h, kid);
    default:
        return 1;
    }
}

/* The computer part of "ip:port" (its length) */
static size_t ip_len(const char *addr)
{
    const char *colon = strrchr(addr, ':');
    return colon ? (size_t)(colon - addr) : strlen(addr);
}

static int same_ip(const char *a, const char *b)
{
    size_t n = ip_len(a);
    return n == ip_len(b) && !memcmp(a, b, n);
}

static int was_kicked(const hub *h, const char *addr)
{
    for (int i = 0; i < h->n_kicked; i++)
        if (same_ip(h->kicked[i], addr))
            return 1;
    return 0;
}

static from_hub kids_msg(int kind, const kid_list *kids)
{
    from_hub m = { 0 };
    m.kind = kind;
    m.kids = kid_list_clone(kids);
    return m;
}

static void admit(hub *h, kid k, hub_out *out)
{
    peer_id id = k.id;
    kid_list_push(&h->kids, k);
    if (!h->has_host)
    {
        h->has_host = 1;
        h->host = id;
    }
    from_hub w = kids_msg(FROM_WELCOME, &h->kids);
    w.you = id;
    w.host = h->host;
    w.seq = h->seq;
    w.picture = picture_of(h->picture);
    w.yes = h->ask_first;
    hub_out_push(out, TO_ONE, id, w);
    hub_out_push(out, TO_OTHERS, id, kids_msg(FROM_KIDS, &h->kids));
}

/* Tell the host who's waiting at the door */
static void door(hub *h, hub_out *out)
{
    hub_out_push(out, TO_ONE, h->host, kids_msg(FROM_WAITING, &h->waiting));
}

static const char *name_of(const hub *h, peer_id id)
{
    int i = find(&h->kids, id);
    return i >= 0 ? h->kids.v[i].name : "The host";
}

static char *str_dup(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(malloc(n), s, n);
}

void hub_join(hub *h, peer_id id, const char *name, const uint8_t color[3], const char *addr, hub_out *out)
{
    kid k = { id, str_dup(name), { color[0], color[1], color[2] }, str_dup(addr) };
    int kicked = was_kicked(h, addr), known = -1;
    for (int i = 0; i < h->known.n && known < 0; i++)
        if (!strcmp(h->known.v[i].name, name) && same_ip(h->known.v[i].addr, addr))
            known = i;
    if (known >= 0)
    {
        kid gone = take(&h->known, known);
        kid_free(&gone);
    }
    if (!h->has_host || (!kicked && (!h->ask_first || known >= 0)))
    {
        admit(h, k, out);
        return;
    }
    kid_list_push(&h->waiting, k);
    from_hub asking = { 0 };
    asking.kind = FROM_ASKING;
    asking.text = str_dup(name_of(h, h->host));
    hub_out_push(out, TO_ONE, id, asking);
    door(h, out);
}

void hub_leave(hub *h, peer_id id, hub_out *out)
{
    int i = find(&h->waiting, id);
    if (i >= 0)
    {
        kid gone = take(&h->waiting, i);
        kid_free(&gone);
        door(h, out);
        return;
    }
    i = find(&h->kids, id);
    if (i < 0)
        return;
    kid gone = take(&h->kids, i);
    kid_free(&gone);
    hub_out_push(out, TO_ALL, 0, kids_msg(FROM_KIDS, &h->kids));
}

void hub_handle(hub *h, peer_id from, to_hub *msg, hub_out *out)
{
    int is_host = h->has_host && h->host == from;
    from_hub m = { 0 };
    if (!hub_is_in(h, from))
    {
        to_hub_free(msg);
        return;
    }
    switch (msg->kind)
    {
    case TO_EDIT:
        h->seq++;
        edit_apply(&msg->edit, &h->picture);
        m.kind = FROM_SEQUENCED;
        m.seq = h->seq;
        m.from = from;
        m.id = msg->id;
        m.edit = msg->edit;
        memset(&msg->edit, 0, sizeof msg->edit);
        hub_out_push(out, TO_ALL, 0, m);
        break;
    case TO_PREVIEW:
        m.kind = FROM_PREVIEW;
        m.from = from;
        m.tiles = msg->tiles;
        memset(&msg->tiles, 0, sizeof msg->tiles);
        hub_out_push(out, TO_OTHERS, from, m);
        break;
    case TO_PRESENCE:
        m.kind = FROM_PRESENCE;
        m.from = from;
        m.presence = msg->presence;
        hub_out_push(out, TO_OTHERS, from, m);
        break;
    case TO_KICK:
    {
        int i = find(&h->kids, msg->kid);
        if (!is_host || msg->kid == from || i < 0)
            break;
        kid gone = take(&h->kids, i);
        h->kicked = realloc(h->kicked, sizeof(char *) * (size_t)(h->n_kicked + 1));
        h->kicked[h->n_kicked++] = gone.addr;     /* only the ip part is compared */
        gone.addr = NULL;
        kid_free(&gone);
        m = kids_msg(FROM_KICKED_OUT, &h->kids);
        m.kid = msg->kid;
        m.by = from;
        hub_out_push(out, TO_ONE, msg->kid, from_hub_clone(&m));
        hub_out_push(out, TO_ALL, 0, m);
        break;
    }
    case TO_LET_IN:
    {
        int i = find(&h->waiting, msg->kid);
        if (!is_host || i < 0)
            break;
        kid newcomer = take(&h->waiting, i);
        if (msg->yes)
            admit(h, newcomer, out);
        else
        {
            const char *name = name_of(h, from);
            m.kind = FROM_REJECTED;
            size_t cap = strlen(name) + 40;
            m.text = malloc(cap);
            snprintf(m.text, cap, "%s didn't let you in this time.", name);
            hub_out_push(out, TO_ONE, newcomer.id, m);
            kid_free(&newcomer);
        }
        door(h, out);
        break;
    }
    case TO_ASK_FIRST:
        if (!is_host)
            break;
        h->ask_first = msg->yes;
        m.kind = FROM_ASK_FIRST;
        m.yes = msg->yes;
        hub_out_push(out, TO_ALL, 0, m);
        if (!msg->yes)
        {
            /* Everyone waiting who'd now get in without asking comes in */
            kid_list wait = { 0 }, all = h->waiting;
            memset(&h->waiting, 0, sizeof h->waiting);
            for (int i = 0; i < all.n; i++)
                if (was_kicked(h, all.v[i].addr))
                    kid_list_push(&wait, all.v[i]);
            h->waiting = wait;
            for (int i = 0; i < all.n; i++)
                if (!was_kicked(h, all.v[i].addr))
                    admit(h, all.v[i], out);
            free(all.v);
            door(h, out);
        }
        break;
    }
    to_hub_free(msg);
}
