/*
 * Paint (C port) - carrying messages between the kids' apps over the local
 * network (Rust: collab/net.rs)
 *
 * Threads talk through `chan`s: locked queues with a wake-up event. A chan is
 * reference-counted, and can also own the connection's socket, which is
 * closed only when the last thread using it lets go. A sender `close`s a chan
 * (the receiver drains it, then sees the end); a receiver that goes away
 * `kill`s it (later pushes fail and are dropped), as Rust's channels do when
 * one side is dropped.
 */
#include "collab/net.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "collab/names.h"

static int ping_ms = 2000, give_up_ms = 6000;

void net_set_timing(int ping, int give_up)
{
    ping_ms = ping;
    give_up_ms = give_up;
}

/* ---- chans ---- */
typedef struct node
{
    struct node *next;
    void *item;
} node;

typedef struct
{
    os_mutex *m;
    os_event *ev;
    node *head, *tail;
    int closed, dead, refs;
    os_sock sock;
    void (*free_item)(void *item);
} chan;

static chan *chan_new(void (*free_item)(void *), os_sock sock)
{
    chan *c = calloc(1, sizeof *c);
    c->m = os_mutex_new();
    c->ev = os_event_new();
    c->refs = 1;
    c->sock = sock;
    c->free_item = free_item;
    return c;
}

static chan *chan_ref(chan *c)
{
    os_lock(c->m);
    c->refs++;
    os_unlock(c->m);
    return c;
}

static void drain(chan *c)
{
    while (c->head)
    {
        node *n = c->head;
        c->head = n->next;
        c->free_item(n->item);
        free(n);
    }
    c->tail = NULL;
}

static void chan_unref(chan *c)
{
    os_lock(c->m);
    int last = --c->refs == 0;
    os_unlock(c->m);
    if (!last)
        return;
    drain(c);
    if (c->sock != OS_BAD_SOCK)
        os_sock_close(c->sock);
    os_event_free(c->ev);
    os_mutex_free(c->m);
    free(c);
}

/* 1 queued; 0 the receiver is gone (the item is freed) */
static int chan_push(chan *c, void *item)
{
    os_lock(c->m);
    if (c->dead || c->closed)
    {
        os_unlock(c->m);
        c->free_item(item);
        return 0;
    }
    node *n = malloc(sizeof *n);
    n->next = NULL;
    n->item = item;
    if (c->tail)
        c->tail->next = n;
    else
        c->head = n;
    c->tail = n;
    os_unlock(c->m);
    os_event_signal(c->ev);
    return 1;
}

/* 1 and *item; 0 nothing within timeout_ms (-1: wait); -1 closed and empty */
static int chan_pop(chan *c, int timeout_ms, void **item)
{
    double until = os_now() + timeout_ms / 1000.0;
    for (;;)
    {
        os_lock(c->m);
        node *n = c->head;
        if (n)
        {
            c->head = n->next;
            if (!c->head)
                c->tail = NULL;
        }
        int closed = c->closed;
        os_unlock(c->m);
        if (n)
        {
            *item = n->item;
            free(n);
            return 1;
        }
        if (closed)
            return -1;
        int left = timeout_ms < 0 ? -1 : (int)((until - os_now()) * 1000.0 + 0.999);
        if (timeout_ms >= 0 && left <= 0)
            return 0;
        os_event_wait(c->ev, left);
    }
}

static void chan_close(chan *c)
{
    os_lock(c->m);
    c->closed = 1;
    os_unlock(c->m);
    os_event_signal(c->ev);
}

static void chan_kill(chan *c)
{
    os_lock(c->m);
    c->dead = 1;
    drain(c);
    os_unlock(c->m);
    os_event_signal(c->ev);
}

static int chan_is_closed(chan *c)
{
    os_lock(c->m);
    int closed = c->closed || c->dead;
    os_unlock(c->m);
    return closed;
}

/* ---- frames ---- */
uint8_t *net_frame(bytes *raw, size_t *len)
{
    uint8_t flag = 0;
    size_t n = raw->len;
    uint8_t *frame;
    if (n >= COMPRESS_FROM)
    {
        size_t zlen = 0;
        uint8_t *z = zip_deflate(raw->p, n, &zlen);
        bytes_free(raw);
        if (!z)
            return NULL;
        /* The header goes in front, in the same buffer (a welcome's picture isn't held twice) */
        frame = realloc(z, zlen + 5);
        if (!frame)
        {
            free(z);
            return NULL;
        }
        memmove(frame + 5, frame, zlen);
        flag = 1;
        n = zlen;
    }
    else
    {
        /* Small: shift the body up behind the header */
        frame = realloc(raw->p, n + 5);
        memmove(frame + 5, frame, n);
        memset(raw, 0, sizeof *raw);
    }
    if (!frame)
        return NULL;
    uint32_t total = (uint32_t)n + 1;
    frame[0] = (uint8_t)(total >> 24);
    frame[1] = (uint8_t)(total >> 16);
    frame[2] = (uint8_t)(total >> 8);
    frame[3] = (uint8_t)total;
    frame[4] = flag;
    *len = n + 5;
    return frame;
}

int net_read_frame(int (*read)(void *ctx, void *buf, int n), void *ctx, uint8_t **body, size_t *len)
{
    uint8_t head[5];
    *body = NULL;
    *len = 0;
    if (read(ctx, head, 4) != 0)
        return -1;
    size_t total = (size_t)head[0] << 24 | (size_t)head[1] << 16 | (size_t)head[2] << 8 | head[3];
    if (total == 0 || total > MAX_FRAME || read(ctx, head + 4, 1) != 0)
        return -1;
    size_t n = total - 1;
    uint8_t *data = malloc(n ? n : 1);
    if (!data || (n && read(ctx, data, (int)n) != 0) || head[4] > 1)
    {
        free(data);
        return -1;
    }
    if (head[4] == 1)
    {
        uint8_t *raw = zip_inflate(data, n, MAX_FRAME, &n);
        free(data);
        if (!raw)
            return -1;
        data = raw;
    }
    *body = data;
    *len = n;
    return 0;
}

static int sock_read(void *ctx, void *buf, int n)
{
    return os_recv_all(*(os_sock *)ctx, buf, n);
}

/* A message's frame, sent whole; 0 ok */
static int send_frame(os_sock s, bytes *raw)
{
    size_t len;
    uint8_t *frame = net_frame(raw, &len);
    int ok = frame && os_send_all(s, frame, (int)len) == 0;
    free(frame);
    return ok ? 0 : -1;
}

/* ---- addresses ---- */
uint32_t net_lan_ip(void)
{
    uint32_t ip;
    return os_local_ips(&ip, 1) == 1 ? ip : 0x7F000001;
}

os_sock net_listen(void)
{
    os_sock s = os_tcp_listen(COLLAB_PORT, NULL);
    return s != OS_BAD_SOCK ? s : os_tcp_listen(0, NULL);
}

/* "192.168.1.4", "192.168.1.4:47100" or a host name; 0 ok */
static int resolve(const char *text, os_addr *out)
{
    char host[256];
    while (*text == ' ' || *text == '\t')
        text++;
    snprintf(host, sizeof host, "%s", text);
    size_t n = strlen(host);
    while (n && (host[n - 1] == ' ' || host[n - 1] == '\t' || host[n - 1] == '\n' || host[n - 1] == '\r'))
        host[--n] = 0;
    unsigned port = COLLAB_PORT;
    char *colon = strrchr(host, ':');
    if (colon)
    {
        char *end;
        unsigned long p = strtoul(colon + 1, &end, 10);
        if (*end || end == colon + 1 || p > 65535)
            return -1;
        port = (unsigned)p;
        *colon = 0;
    }
    if (os_addr_parse(host, (uint16_t)port, out) == 0)
        return 0;
    out->port = (uint16_t)port;
    return host[0] ? os_resolve(host, &out->ip) : -1;
}

/* ---- events for the app ---- */
void link_event_free(link_event *e)
{
    from_hub_free(&e->msg);
    free(e->why);
    memset(e, 0, sizeof *e);
}

static void free_event(void *item)
{
    link_event_free(item);
    free(item);
}

static int push_event(chan *events, int kind, from_hub *msg, const char *why, void (*wake)(void))
{
    link_event *e = calloc(1, sizeof *e);
    e->kind = kind;
    if (msg)
        e->msg = *msg;
    if (why)
        e->why = strcpy(malloc(strlen(why) + 1), why);
    int ok = chan_push(events, e);
    if (wake)
        wake();
    return ok;
}

/* ---- the hub's thread ---- */
enum
{
    EV_JOIN,
    EV_MESSAGE,
    EV_LEFT,
    EV_STOP,
};

/* Where the hub sends a kid's messages: the host's own app (a chan of
 * link_events), or a friend's writer thread (a chan of frames) */
typedef struct
{
    peer_id id;
    int local;
    chan *q;
} sink;

typedef struct
{
    int kind;
    peer_id id;
    char *name, *addr;
    uint8_t color[3];
    sink sink;
    to_hub msg;
} hub_event;

/* A frame on its way to one or more friends' writer threads */
typedef struct
{
    atomic_int refs;
    uint8_t *data;
    size_t len;
} frame;

static void frame_unref(void *item)
{
    frame *f = item;
    if (atomic_fetch_sub(&f->refs, 1) == 1)
    {
        free(f->data);
        free(f);
    }
}

static void free_hub_event(void *item)
{
    hub_event *e = item;
    free(e->name);
    free(e->addr);
    to_hub_free(&e->msg);
    if (e->sink.q)
        chan_unref(e->sink.q);
    free(e);
}

typedef struct
{
    hub *h;
    chan *in;
    sink *sinks;
    int n_sinks;
    void (*wake)(void);
} hub_thread;

static int find_sink(const hub_thread *t, peer_id id)
{
    for (int i = 0; i < t->n_sinks; i++)
        if (t->sinks[i].id == id)
            return i;
    return -1;
}

/* Let go of a sink: a friend's writer sends what's queued, then hangs up */
static void drop_sink(hub_thread *t, int i)
{
    if (!t->sinks[i].local)
        chan_close(t->sinks[i].q);
    chan_unref(t->sinks[i].q);
    t->sinks[i] = t->sinks[--t->n_sinks];
}

/* Pass one of the hub's messages on; 0 when that kid's connection is gone.
 * `f` is the message's frame, made the first time a friend needs it. A message
 * for one kid only is handed over whole to my own app (the welcome's picture isn't copied). */
static int deliver(hub_thread *t, const sink *s, from_hub *msg, int only, frame **f)
{
    if (s->local)
    {
        if (msg->kind == FROM_PING)
            return 1;
        only &= !msg->picture.borrowed;
        from_hub copy = only ? *msg : from_hub_clone(msg);
        if (only)
            memset(msg, 0, sizeof *msg);
        return push_event(s->q, LINK_MESSAGE, &copy, NULL, t->wake);
    }
    if (!*f)
    {
        bytes raw = { 0 };
        from_hub_encode(msg, &raw);
        *f = calloc(1, sizeof **f);
        atomic_init(&(*f)->refs, 1);
        (*f)->data = net_frame(&raw, &(*f)->len);
    }
    if (!(*f)->data)
        return 0;
    atomic_fetch_add(&(*f)->refs, 1);
    return chan_push(s->q, *f);
}

static void route(hub_thread *t, hub_out *out)
{
    while (out->n)
    {
        frame **frames = calloc((size_t)out->n, sizeof(frame *));
        peer_id *gone = malloc(sizeof(peer_id) * (size_t)(t->n_sinks + 1));
        int n_gone = 0;
        for (int k = 0; k < t->n_sinks; k++)
        {
            const sink *s = &t->sinks[k];
            for (int i = 0; i < out->n; i++)
                if (hub_wants(t->h, out->v[i].to, out->v[i].id, s->id)
                    && !deliver(t, s, &out->v[i].msg, out->v[i].to == TO_ONE, &frames[i]))
                {
                    gone[n_gone++] = s->id;
                    break;
                }
        }
        for (int i = 0; i < out->n; i++)
            if (frames[i])
                frame_unref(frames[i]);
        free(frames);
        /* Those sent away (or not let in) hang up once their news is on its way */
        for (int k = t->n_sinks - 1; k >= 0; k--)
        {
            int is_gone = !hub_knows(t->h, t->sinks[k].id);
            for (int g = 0; g < n_gone; g++)
                is_gone |= gone[g] == t->sinks[k].id;
            if (is_gone)
                drop_sink(t, k);
        }
        hub_out_free(out);
        for (int g = 0; g < n_gone; g++)
            hub_leave(t->h, gone[g], out);
        free(gone);
    }
}

/* One event at a time, so every edit gets one place in the order */
static void hub_main(void *arg)
{
    hub_thread *t = arg;
    double next_ping = os_now() + ping_ms / 1000.0;
    for (int running = 1; running;)
    {
        hub_out out = { 0 };
        void *item;
        int wait = (int)((next_ping - os_now()) * 1000.0);
        int got = chan_pop(t->in, wait > 0 ? wait : 0, &item);
        if (got < 0)
            break;
        if (got)
        {
            hub_event *e = item;
            int i = find_sink(t, e->id);
            switch (e->kind)
            {
            case EV_JOIN:
                if (i >= 0)
                    drop_sink(t, i);
                t->sinks = realloc(t->sinks, sizeof(sink) * (size_t)(t->n_sinks + 1));
                t->sinks[t->n_sinks++] = e->sink;
                e->sink.q = NULL;
                hub_join(t->h, e->id, e->name, e->color, e->addr, &out);
                break;
            case EV_MESSAGE:
                if (i >= 0)
                    hub_handle(t->h, e->id, &e->msg, &out);
                break;
            case EV_LEFT:
                if (i >= 0)
                {
                    drop_sink(t, i);
                    hub_leave(t->h, e->id, &out);
                }
                break;
            case EV_STOP:
                running = 0;
                break;
            }
            free_hub_event(e);
        }
        /* Everyone hears from the room regularly, however busy it is with others,
         * so a quiet connection (or one waiting at the door) isn't mistaken for a lost one */
        if (running && os_now() >= next_ping)
        {
            from_hub ping = { 0 };
            ping.kind = FROM_PING;
            hub_out_push(&out, TO_EVERYONE, 0, ping);
            next_ping = os_now() + ping_ms / 1000.0;
        }
        route(t, &out);
        hub_out_free(&out);
    }
    while (t->n_sinks)
        drop_sink(t, t->n_sinks - 1);
    free(t->sinks);
    chan_kill(t->in);
    chan_unref(t->in);
    hub_free(t->h);
    free(t);
}

/* ---- one friend's connection, on the host ---- */
typedef struct
{
    os_sock sock;
    os_addr peer;
    peer_id id;
    chan *hub;
    void (*wake)(void);
} kid_conn;

/* Send the hub's frames to one friend until the hub lets go of them (or the connection fails) */
static void kid_writer(void *arg)
{
    chan *q = arg;
    void *item;
    while (chan_pop(q, -1, &item) > 0)
    {
        frame *f = item;
        int ok = os_send_all(q->sock, f->data, (int)f->len) == 0;
        frame_unref(f);
        if (!ok)
            break;
    }
    chan_kill(q);
    os_sock_shutdown(q->sock);
    chan_unref(q);
}

static void post(chan *hub, int kind, peer_id id)
{
    hub_event *e = calloc(1, sizeof *e);
    e->kind = kind;
    e->id = id;
    chan_push(hub, e);
}

/* Say hello, then pass messages both ways */
static void serve_kid(void *arg)
{
    kid_conn *k = arg;
    os_sock_nodelay(k->sock);
    os_sock_timeout(k->sock, 5000, 0);
    uint8_t *body;
    size_t len;
    hello h;
    chan *q = chan_new(frame_unref, k->sock);
    if (net_read_frame(sock_read, &k->sock, &body, &len) != 0)
        goto done;
    int ok = hello_decode(body, len, &h) == 0 && !memcmp(h.magic, HELLO_MAGIC, 4);
    free(body);
    if (!ok)
        goto done;
    if (h.version != COLLAB_VERSION)
    {
        from_hub no = { 0 };
        bytes raw = { 0 };
        no.kind = FROM_REJECTED;
        no.text = "Your friend has a different version of Paint. Use the same version to draw together.";
        from_hub_encode(&no, &raw);
        send_frame(k->sock, &raw);
        hello_free(&h);
        goto done;
    }
    os_sock_timeout(k->sock, give_up_ms, 0);

    hub_event *join = calloc(1, sizeof *join);
    char addr[32], name[100];
    utf8_prefix(h.name, 24, name, sizeof name);
    /* Where this kid's app could be reached, should it take over as host */
    os_addr_format((os_addr){ k->peer.ip, h.listen }, addr, sizeof addr);
    join->kind = EV_JOIN;
    join->id = k->id;
    join->name = strcpy(malloc(strlen(name) + 1), name);
    join->addr = strcpy(malloc(strlen(addr) + 1), addr);
    memcpy(join->color, h.color, 3);
    join->sink = (sink){ k->id, 0, chan_ref(q) };
    hello_free(&h);
    if (!chan_push(k->hub, join))
        goto done;
    os_thread_detach(os_thread_start(kid_writer, chan_ref(q)));
    while (net_read_frame(sock_read, &k->sock, &body, &len) == 0)
    {
        hub_event *e = calloc(1, sizeof *e);
        e->kind = EV_MESSAGE;
        e->id = k->id;
        ok = to_hub_decode(body, len, &e->msg) == 0 && to_hub_valid(&e->msg);
        free(body);
        if (!ok || !chan_push(k->hub, e))
        {
            if (!ok)
                free_hub_event(e);
            break;
        }
    }
    os_sock_shutdown(k->sock);
    post(k->hub, EV_LEFT, k->id);
    if (k->wake)
        k->wake();
done:
    chan_unref(q);      /* the socket closes once the writer is done with it too */
    chan_unref(k->hub);
    free(k);
}

/* ---- the listener ---- */
typedef struct
{
    os_sock sock;
    chan *hub;
    peer_id next_id;
    void (*wake)(void);
} listener;

static void listen_main(void *arg)
{
    listener *L = arg;
    for (;;)
    {
        os_addr peer;
        os_sock s = os_tcp_accept(L->sock, &peer);
        if (chan_is_closed(L->hub))
        {
            os_sock_close(s);
            break;
        }
        if (s == OS_BAD_SOCK)
        {
            os_sleep_ms(50);
            continue;
        }
        kid_conn *k = calloc(1, sizeof *k);
        k->sock = s;
        k->peer = peer;
        k->id = L->next_id++;
        k->hub = chan_ref(L->hub);
        k->wake = L->wake;
        os_thread *th = os_thread_start(serve_kid, k);
        if (th)
            os_thread_detach(th);
        else
        {
            os_sock_close(s);
            chan_unref(k->hub);
            free(k);
        }
    }
    os_sock_close(L->sock);
    chan_unref(L->hub);
    free(L);
}

/* ---- links ---- */
struct link
{
    chan *events;           /* link_events for the app */
    void (*wake)(void);
    /* hosting */
    chan *hub;
    peer_id me;
    uint16_t port;
    /* joined */
    chan *up;               /* to_hubs for the sending thread (it owns the socket) */
    os_sock spare;
    uint16_t spare_port;
};

static void free_to_hub(void *item)
{
    to_hub_free(item);
    free(item);
}

link *link_host(img *picture, const char *name, const uint8_t color[3], int ask_first, void (*wake)(void), char *err, size_t cap)
{
    os_sock s = net_listen();
    if (s == OS_BAD_SOCK)
    {
        img_free(picture);
        snprintf(err, cap, "No network port was free for friends to connect to.");
        return NULL;
    }
    hub *h = hub_new(picture);
    hub_set_door(h, ask_first, (kid_list){ 0 });
    return link_host_with(s, h, name, color, wake);
}

link *link_host_with(os_sock sock, hub *h, const char *name, const uint8_t color[3], void (*wake)(void))
{
    link *l = calloc(1, sizeof *l);
    l->wake = wake;
    l->spare = OS_BAD_SOCK;
    l->port = os_sock_local(sock).port;
    l->events = chan_new(free_event, OS_BAD_SOCK);
    l->hub = chan_new(free_hub_event, OS_BAD_SOCK);
    l->me = 1;

    hub_thread *t = calloc(1, sizeof *t);
    t->h = h;
    t->in = chan_ref(l->hub);
    t->wake = wake;
    os_thread_detach(os_thread_start(hub_main, t));

    /* I join first, so the hub makes me its host: on a takeover, friends may already be waiting at the door */
    char addr[32];
    os_addr_format((os_addr){ net_lan_ip(), l->port }, addr, sizeof addr);
    hub_event *join = calloc(1, sizeof *join);
    join->kind = EV_JOIN;
    join->id = l->me;
    join->name = strcpy(malloc(strlen(name) + 1), name);
    join->addr = strcpy(malloc(strlen(addr) + 1), addr);
    memcpy(join->color, color, 3);
    join->sink = (sink){ l->me, 1, chan_ref(l->events) };
    chan_push(l->hub, join);

    listener *L = calloc(1, sizeof *L);
    L->sock = sock;
    L->hub = chan_ref(l->hub);
    L->next_id = 2;
    L->wake = wake;
    os_thread_detach(os_thread_start(listen_main, L));
    return l;
}

/* Joining: what the receiving thread needs */
typedef struct
{
    char *addr;
    hello hello;
    chan *up, *events;
    void (*wake)(void);
} joiner;

/* Send my messages (and a ping whenever there's been nothing to send for a while) */
static void send_main(void *arg)
{
    chan *up = arg;
    for (;;)
    {
        void *item = NULL;
        int got = chan_pop(up, ping_ms, &item);
        if (got < 0)
            break;
        to_hub ping = { 0 };
        ping.kind = TO_PING;
        to_hub *m = got ? item : &ping;
        bytes raw = { 0 };
        to_hub_encode(m, &raw);
        if (got)
            free_to_hub(m);
        if (send_frame(up->sock, &raw) != 0)
            break;
    }
    chan_kill(up);
    os_sock_shutdown(up->sock);
    chan_unref(up);
}

/* Connect and say hello; the connection's socket, or OS_BAD_SOCK and why not */
static os_sock connect_to(joiner *j, char *why, size_t cap)
{
    os_addr to;
    if (resolve(j->addr, &to) != 0)
    {
        snprintf(why, cap, "Couldn't find \"%s\" on the network.", j->addr);
        return OS_BAD_SOCK;
    }
    char where[32];
    os_addr_format(to, where, sizeof where);
    snprintf(why, cap, "Couldn't reach a drawing at %s. Check the address, and that you're on the same Wi-Fi.", where);
    os_sock s = os_tcp_connect(to, 4000);
    if (s == OS_BAD_SOCK)
        return s;
    os_sock_nodelay(s);
    bytes raw = { 0 };
    hello_encode(&j->hello, &raw);
    if (send_frame(s, &raw) != 0)
    {
        os_sock_close(s);
        return OS_BAD_SOCK;
    }
    return s;
}

/* Pass the hub's messages to the app until the connection ends; why it did */
static const char *receive(os_sock s, joiner *j)
{
    os_sock_timeout(s, give_up_ms, 0);
    uint8_t *body;
    size_t len;
    while (net_read_frame(sock_read, &s, &body, &len) == 0)
    {
        from_hub m;
        int ok = from_hub_decode(body, len, &m) == 0;
        free(body);
        if (ok && m.kind == FROM_PING)
            continue;
        if (!ok || !from_hub_valid(&m))
        {
            from_hub_free(&m);
            return "Your friend's Paint sent something this one can't use.";
        }
        int rejected = m.kind == FROM_REJECTED;
        if (!push_event(j->events, LINK_MESSAGE, &m, NULL, j->wake) || rejected)
            return "";
    }
    return "The drawing ended: the host left, or the network dropped.";
}

static void receive_main(void *arg)
{
    joiner *j = arg;
    char why[300];
    os_sock s = connect_to(j, why, sizeof why);
    if (s != OS_BAD_SOCK)
    {
        os_lock(j->up->m);
        j->up->sock = s;    /* closed with the chan, once both threads are done */
        os_unlock(j->up->m);
        os_thread_detach(os_thread_start(send_main, chan_ref(j->up)));
        snprintf(why, sizeof why, "%s", receive(s, j));
        os_sock_shutdown(s);
    }
    push_event(j->events, LINK_CLOSED, NULL, why, j->wake);
    chan_unref(j->up);
    chan_unref(j->events);
    hello_free(&j->hello);
    free(j->addr);
    free(j);
}

link *link_join(const char *addr, const char *name, const uint8_t color[3], os_sock spare, void (*wake)(void), char *err, size_t cap)
{
    if (spare == OS_BAD_SOCK)
        spare = net_listen();
    if (spare == OS_BAD_SOCK)
    {
        snprintf(err, cap, "No network port was free to keep ready.");
        return NULL;
    }
    link *l = calloc(1, sizeof *l);
    l->wake = wake;
    l->spare = spare;
    l->spare_port = os_sock_local(spare).port;
    l->events = chan_new(free_event, OS_BAD_SOCK);
    l->up = chan_new(free_to_hub, OS_BAD_SOCK);

    joiner *j = calloc(1, sizeof *j);
    j->addr = strcpy(malloc(strlen(addr) + 1), addr);
    memcpy(j->hello.magic, HELLO_MAGIC, 4);
    j->hello.version = COLLAB_VERSION;
    j->hello.name = strcpy(malloc(strlen(name) + 1), name);
    memcpy(j->hello.color, color, 3);
    j->hello.listen = l->spare_port;
    j->up = chan_ref(l->up);
    j->events = chan_ref(l->events);
    j->wake = wake;
    os_thread *th = os_thread_start(receive_main, j);
    if (!th)
    {
        snprintf(err, cap, "Paint couldn't start a thread.");
        chan_unref(j->up);
        chan_unref(j->events);
        hello_free(&j->hello);
        free(j->addr);
        free(j);
        link_free(l);
        return NULL;
    }
    os_thread_detach(th);
    return l;
}

void link_send(link *l, to_hub msg)
{
    if (l->hub)
    {
        hub_event *e = calloc(1, sizeof *e);
        e->kind = EV_MESSAGE;
        e->id = l->me;
        e->msg = msg;
        chan_push(l->hub, e);
        return;
    }
    to_hub *m = malloc(sizeof *m);
    *m = msg;
    chan_push(l->up, m);
}

int link_recv(link *l, link_event *out)
{
    void *item;
    if (chan_pop(l->events, 0, &item) <= 0)
        return 0;
    *out = *(link_event *)item;
    free(item);
    return 1;
}

int link_hosting_at(const link *l, os_addr *out)
{
    if (!l->hub)
        return -1;
    out->ip = net_lan_ip();
    out->port = l->port;
    return 0;
}

os_sock link_take_spare(link *l)
{
    os_sock s = l->spare;
    l->spare = OS_BAD_SOCK;
    return s;
}

void link_free(link *l)
{
    if (!l)
        return;
    if (l->hub)
    {
        post(l->hub, EV_STOP, 0);
        chan_close(l->hub);
        /* Wake the listener so it notices */
        os_sock s = os_tcp_connect((os_addr){ 0x7F000001, l->port }, 200);
        os_sock_close(s);
        chan_unref(l->hub);
    }
    if (l->up)
    {
        /* The sending thread sends what's queued, then hangs up */
        chan_close(l->up);
        chan_unref(l->up);
    }
    chan_kill(l->events);
    chan_unref(l->events);
    os_sock_close(l->spare);
    free(l);
}
