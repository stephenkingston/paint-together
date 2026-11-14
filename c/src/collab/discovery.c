/*
 * Paint (C port) - finding drawings on the same network with mDNS
 * (Rust: collab/discovery.rs and what the mdns-sd crate does for it)
 *
 * Advertising, as mdns-sd does: the drawing is announced twice, a second
 * apart, as one response per network interface holding PTR (service type ->
 * instance), SRV (instance -> host and port), TXT and A (host -> that
 * interface's address) records; queries for any of them are answered (a PTR
 * query gets the others as additional records); stopping sends them all again
 * with TTL 0 ("goodbye"). Queries from a port other than 5353 get a unicast
 * answer, as RFC 6762 says.
 *
 * Browsing: a PTR query for the service type, repeated at growing intervals
 * (up to a minute, which keeps the 2-minute host records fresh); instances
 * missing their SRV, TXT or address are asked about directly. A drawing is
 * listed once it has a PTR, an SRV and an address (the lowest, if it has several,
 * as the Rust picks); it goes when any of those
 * expire, or on a goodbye (TTL 0, kept for one more second as RFC 6762 says).
 * Every record field is bounds-checked; compressed names are followed with a
 * hop limit.
 */
#include "collab/discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "collab/names.h"
#include "collab/protocol.h"

#define MDNS_GROUP 0xE00000FBu      /* 224.0.0.251 */
#define MDNS_PORT 5353
#define SERVICE "_paintparty._tcp.local."
#define META_QUERY "_services._dns-sd._udp.local."
#define HOST_TTL 120                /* SRV and address records (mdns-sd's) */
#define OTHER_TTL 4500              /* PTR and TXT records */
#define T_A 1
#define T_PTR 12
#define T_TXT 16
#define T_SRV 33
#define T_ANY 255
#define FLUSH 0x8000                /* the cache-flush bit of a record's class */
#define MAX_IPS 8
#define MAX_SEEN 32
#define MAX_ADDRS 64
#define PKT_CAP 1500                /* what we send fits an Ethernet frame */
#define RECV_CAP 9000               /* the most an mDNS packet may be (RFC 6762) */

/* What another drawing has said about itself so far */
typedef struct
{
    char fullname[96];
    double ptr_until, srv_until, txt_until, next_ask;
    char host[96];
    uint16_t port;
    char name[100];
    int has_name, kids, v_ok;
} seen;

typedef struct
{
    char host[96];
    uint32_t ip;
    uint32_t heard_from;    /* who said so: a cache flush only replaces what the same sender said */
    double until, added;
} address;

/* My drawing, as advertised */
typedef struct
{
    char name[100];
    int kids;
    uint16_t port;
} ad;

struct discovery
{
    os_mutex *m;
    os_thread *th;
    os_sock sock;
    void (*wake)(void);
    int stop;

    nearby *shown;          /* the app's copy, as of the last poll */
    int n_shown;

    /* shared with the thread (under m) */
    int browsing, advertising, announce_left, goodbye, changed;
    ad mine, bye;
    char fullname[96], host[48];
    nearby found[MAX_SEEN];
    int n_found;

    /* the thread's own */
    double next_announce, next_browse, browse_gap, next_ips;
    uint32_t ips[MAX_IPS];
    int n_ips;
    seen seen[MAX_SEEN];
    int n_seen;
    address addrs[MAX_ADDRS];
    int n_addrs;
    uint8_t buf[RECV_CAP];
};

/* ---- names ---- */
static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int name_eq(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
        a++, b++;
    return lower((unsigned char)*a) == lower((unsigned char)*b);
}

/* Whether `name` is an instance of my service type ("<instance>._paintparty._tcp.local.") */
static int is_instance(const char *name)
{
    size_t n = strlen(name), s = strlen(SERVICE);
    return n > s + 1 && name[n - s - 1] == '.' && name_eq(name + n - s, SERVICE);
}

/* ---- writing packets ---- */
typedef struct
{
    uint8_t p[PKT_CAP];
    int len, over;
    int counts[4];          /* questions, answers, authority, additional */
} pkt;

static void put(pkt *k, const void *data, int n)
{
    if (k->len + n > PKT_CAP)
    {
        k->over = 1;
        return;
    }
    memcpy(k->p + k->len, data, (size_t)n);
    k->len += n;
}

static void put16(pkt *k, unsigned v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    put(k, b, 2);
}

static void put32(pkt *k, uint32_t v)
{
    put16(k, v >> 16);
    put16(k, v & 0xFFFF);
}

/* A dotted name ("a.b.local."), uncompressed */
static void put_name(pkt *k, const char *name)
{
    while (*name)
    {
        const char *dot = strchr(name, '.');
        int n = dot ? (int)(dot - name) : (int)strlen(name);
        if (n > 63)
            n = 63;
        uint8_t len = (uint8_t)n;
        put(k, &len, 1);
        put(k, name, n);
        name += n + (dot ? 1 : 0);
        if (!dot)
            break;
    }
    put(k, "", 1);
}

static void start(pkt *k, uint16_t id, uint16_t flags)
{
    memset(k->counts, 0, sizeof k->counts);
    k->len = k->over = 0;
    put16(k, id);
    put16(k, flags);
    put(k, "\0\0\0\0\0\0\0\0", 8);
}

static void question(pkt *k, const char *name, int type)
{
    put_name(k, name);
    put16(k, (unsigned)type);
    put16(k, 1);
    k->counts[0]++;
}

/* Record header; returns where its data length goes */
static int record(pkt *k, int section, const char *name, int type, int flush, uint32_t ttl)
{
    put_name(k, name);
    put16(k, (unsigned)type);
    put16(k, flush ? FLUSH | 1 : 1);
    put32(k, ttl);
    put16(k, 0);
    k->counts[section]++;
    return k->len - 2;
}

static void record_end(pkt *k, int at)
{
    if (k->over)
        return;
    int n = k->len - at - 2;
    k->p[at] = (uint8_t)(n >> 8);
    k->p[at + 1] = (uint8_t)n;
}

static void finish(pkt *k)
{
    for (int i = 0; i < 4; i++)
    {
        k->p[4 + 2 * i] = (uint8_t)(k->counts[i] >> 8);
        k->p[5 + 2 * i] = (uint8_t)k->counts[i];
    }
}

/* How long a record lasts: legacy (one-shot) queriers get at most 10 s, as mdns-sd gives */
static uint32_t ttl_of(uint32_t ttl, int legacy, int bye)
{
    return bye ? 0 : legacy && ttl > 10 ? 10 : ttl;
}

static void put_ptr(discovery *d, pkt *k, int section, int legacy, int bye)
{
    int at = record(k, section, SERVICE, T_PTR, 0, ttl_of(OTHER_TTL, legacy, bye));
    put_name(k, d->fullname);
    record_end(k, at);
}

static void put_srv(discovery *d, pkt *k, int section, const ad *a, int legacy, int bye)
{
    int at = record(k, section, d->fullname, T_SRV, !legacy, ttl_of(HOST_TTL, legacy, bye));
    put16(k, 0);
    put16(k, 0);
    put16(k, a->port);
    put_name(k, d->host);
    record_end(k, at);
}

static void put_txt(discovery *d, pkt *k, int section, const ad *a, int legacy, int bye)
{
    char kv[3][120];
    snprintf(kv[0], sizeof kv[0], "name=%s", a->name);
    snprintf(kv[1], sizeof kv[1], "kids=%d", a->kids);
    snprintf(kv[2], sizeof kv[2], "v=%d", COLLAB_VERSION);
    int at = record(k, section, d->fullname, T_TXT, !legacy, ttl_of(OTHER_TTL, legacy, bye));
    for (int i = 0; i < 3; i++)
    {
        uint8_t n = (uint8_t)strlen(kv[i]);
        put(k, &n, 1);
        put(k, kv[i], n);
    }
    record_end(k, at);
}

static void put_a(discovery *d, pkt *k, int section, uint32_t ip, int legacy, int bye)
{
    int at = record(k, section, d->host, T_A, !legacy, ttl_of(HOST_TTL, legacy, bye));
    put32(k, ip);
    record_end(k, at);
}

static void send_to(discovery *d, pkt *k, os_addr to)
{
    finish(k);
    if (!k->over)
        os_udp_send(d->sock, to, k->p, k->len);
}

static void multicast(discovery *d, pkt *k, uint32_t from_ip)
{
    os_udp_multicast_if(d->sock, from_ip);
    send_to(d, k, (os_addr){ MDNS_GROUP, MDNS_PORT });
}

/* The whole service, as one unsolicited response per interface (TTL 0: goodbye) */
static void announce(discovery *d, const ad *a, int bye)
{
    for (int i = 0; i < d->n_ips; i++)
    {
        pkt k;
        start(&k, 0, 0x8400);
        put_ptr(d, &k, 1, 0, bye);
        put_srv(d, &k, 1, a, 0, bye);
        put_txt(d, &k, 1, a, 0, bye);
        put_a(d, &k, 1, d->ips[i], 0, bye);
        multicast(d, &k, d->ips[i]);
    }
}

/* ---- reading packets ---- */
/* A name at *pos (following compression pointers), dotted with a final "." */
static int get_name(const uint8_t *p, int len, int *pos, char *out, int cap)
{
    int at = *pos, o = 0, hops = 0, end = -1;
    for (;;)
    {
        if (at >= len)
            return -1;
        int n = p[at];
        if ((n & 0xC0) == 0xC0)
        {
            if (at + 1 >= len || ++hops > 16)
                return -1;
            if (end < 0)
                end = at + 2;
            at = (n & 0x3F) << 8 | p[at + 1];
            continue;
        }
        if (n & 0xC0)
            return -1;
        if (!n)
        {
            if (end < 0)
                end = at + 1;
            break;
        }
        if (at + 1 + n > len || o + n + 2 > cap)
            return -1;
        memcpy(out + o, p + at + 1, (size_t)n);
        o += n;
        out[o++] = '.';
        at += 1 + n;
    }
    if (!o)
        out[o++] = '.';
    out[o] = 0;
    *pos = end;
    return 0;
}

static unsigned get16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) << 16 | get16(p + 2);
}

/* ---- answering queries ---- */
/* The address of mine a querier most likely reaches me at: the one sharing most leading bits */
static uint32_t my_ip_for(const discovery *d, uint32_t peer)
{
    uint32_t best = d->n_ips ? d->ips[0] : 0;
    int best_bits = -1;
    for (int i = 0; i < d->n_ips; i++)
    {
        int bits = 0;
        while (bits < 32 && !((d->ips[i] ^ peer) & (0x80000000u >> bits)))
            bits++;
        if (bits > best_bits)
            best_bits = bits, best = d->ips[i];
    }
    return best;
}

static void answer(discovery *d, const uint8_t *p, int len, os_addr from)
{
    int pos = 12, nq = (int)get16(p + 4);
    int ptr = 0, meta = 0, srv = 0, txt = 0, a = 0;
    char names[4][256];
    int types[4], n_names = 0;
    for (int i = 0; i < nq; i++)
    {
        char name[256];
        if (get_name(p, len, &pos, name, sizeof name) || pos + 4 > len)
            return;
        int type = (int)get16(p + pos), any = type == T_ANY;
        pos += 4;
        int mine = 0;
        if ((type == T_PTR || any) && name_eq(name, SERVICE))
            mine = ptr = 1;
        if ((type == T_PTR || any) && name_eq(name, META_QUERY))
            mine = meta = 1;
        if ((type == T_SRV || any) && name_eq(name, d->fullname))
            mine = srv = 1;
        if ((type == T_TXT || any) && name_eq(name, d->fullname))
            mine = txt = 1;
        if ((type == T_A || any) && name_eq(name, d->host))
            mine = a = 1;
        if (mine && n_names < 4)
        {
            snprintf(names[n_names], sizeof names[0], "%s", name);
            types[n_names++] = type;
        }
    }
    if (!d->n_ips || !(ptr | meta | srv | txt | a))
        return;
    int legacy = from.port != MDNS_PORT;
    uint32_t ip = my_ip_for(d, from.ip);
    pkt k;
    start(&k, legacy ? (uint16_t)get16(p) : 0, 0x8400);
    if (legacy)
        for (int i = 0; i < n_names; i++)
            question(&k, names[i], types[i]);
    if (ptr)
        put_ptr(d, &k, 1, legacy, 0);
    if (meta)
    {
        int at = record(&k, 1, META_QUERY, T_PTR, 0, ttl_of(OTHER_TTL, legacy, 0));
        put_name(&k, SERVICE);
        record_end(&k, at);
    }
    if (srv)
        put_srv(d, &k, 1, &d->mine, legacy, 0);
    if (txt)
        put_txt(d, &k, 1, &d->mine, legacy, 0);
    if (a)
        put_a(d, &k, 1, ip, legacy, 0);
    /* What a PTR (or SRV) query will want next */
    if (ptr && !srv)
        put_srv(d, &k, 3, &d->mine, legacy, 0);
    if (ptr && !txt)
        put_txt(d, &k, 3, &d->mine, legacy, 0);
    if ((ptr || srv) && !a)
        put_a(d, &k, 3, ip, legacy, 0);
    if (legacy)
        send_to(d, &k, from);
    else
        multicast(d, &k, ip);
}

/* ---- taking in answers ---- */
static seen *seen_of(discovery *d, const char *fullname, int create)
{
    for (int i = 0; i < d->n_seen; i++)
        if (!strcmp(d->seen[i].fullname, fullname))
            return &d->seen[i];
    if (!create || d->n_seen == MAX_SEEN || strlen(fullname) >= sizeof d->seen[0].fullname)
        return NULL;
    seen *s = &d->seen[d->n_seen++];
    memset(s, 0, sizeof *s);
    strcpy(s->fullname, fullname);
    return s;
}

/* The properties a drawing's TXT record gives (the first of each key counts) */
static void take_txt(seen *s, const uint8_t *p, int n)
{
    int got_name = 0, got_kids = 0, got_v = 0;
    s->has_name = 0;
    s->kids = 1;
    s->v_ok = 0;
    for (int i = 0; i < n;)
    {
        int len = p[i++];
        if (!len || i + len > n)
            break;
        const char *kv = (const char *)p + i;
        const char *eq = memchr(kv, '=', (size_t)len);
        int klen = eq ? (int)(eq - kv) : len, vlen = eq ? len - klen - 1 : -1;
        char val[256] = "";
        if (vlen >= 0)
            memcpy(val, eq + 1, (size_t)vlen), val[vlen] = 0;
        if (klen == 4 && !memcmp(kv, "name", 4) && !got_name++)
        {
            s->has_name = vlen >= 0 && (int)strlen(val) == vlen;
            utf8_prefix(val, 24, s->name, sizeof s->name);
        }
        else if (klen == 4 && !memcmp(kv, "kids", 4) && !got_kids++)
        {
            char *end;
            unsigned long k = strtoul(val, &end, 10);
            if (vlen > 0 && val[0] != '-' && !*end && k < 1000000)
                s->kids = (int)k;
        }
        else if (klen == 1 && kv[0] == 'v' && !got_v++)
        {
            char want[12];
            snprintf(want, sizeof want, "%d", COLLAB_VERSION);
            s->v_ok = !strcmp(val, want) && vlen >= 0;
        }
        i += len;
    }
}

static void take_records(discovery *d, const uint8_t *p, int len, uint32_t sender, double now)
{
    int pos = 12, nq = (int)get16(p + 4);
    int nr = (int)(get16(p + 6) + get16(p + 8) + get16(p + 10));
    for (int i = 0; i < nq; i++)
    {
        char name[256];
        if (get_name(p, len, &pos, name, sizeof name) || (pos += 4) > len)
            return;
    }
    /* Twice: services first, then the addresses of the hosts they named */
    int first = pos;
    for (int pass = 0; pass < 2; pass++)
    {
        pos = first;
        for (int i = 0; i < nr; i++)
        {
            char name[256], target[256];
            if (get_name(p, len, &pos, name, sizeof name) || pos + 10 > len)
                return;
            int type = (int)get16(p + pos), flush = get16(p + pos + 2) & FLUSH;
            uint32_t ttl = get32(p + pos + 4);
            int rdlen = (int)get16(p + pos + 8), rd = pos + 10;
            pos = rd + rdlen;
            if (pos > len)
                return;
            /* A goodbye's record lasts one more second (RFC 6762 10.1) */
            double until = now + (ttl ? ttl : 1);
            if (pass == 0 && type == T_PTR && name_eq(name, SERVICE))
            {
                int at = rd;
                seen *s;
                if (!get_name(p, rd + rdlen, &at, target, sizeof target) && is_instance(target) && (s = seen_of(d, target, 1)))
                    s->ptr_until = until;
            }
            else if (pass == 0 && type == T_SRV && rdlen > 6 && is_instance(name))
            {
                int at = rd + 6;
                seen *s = seen_of(d, name, 1);
                if (s && !get_name(p, rd + rdlen, &at, target, sizeof target) && strlen(target) < sizeof s->host)
                {
                    strcpy(s->host, target);
                    s->port = (uint16_t)get16(p + rd + 4);
                    s->srv_until = until;
                }
            }
            else if (pass == 0 && type == T_TXT && is_instance(name))
            {
                seen *s = seen_of(d, name, 1);
                if (s)
                {
                    take_txt(s, p + rd, rdlen);
                    s->txt_until = until;
                }
            }
            else if (pass == 1 && type == T_A && rdlen == 4)
            {
                int wanted = 0;
                for (int k = 0; k < d->n_seen; k++)
                    wanted |= name_eq(d->seen[k].host, name);
                if (!wanted)
                    continue;
                uint32_t ip = get32(p + rd);
                address *a = NULL;
                for (int k = 0; k < d->n_addrs; k++)
                {
                    address *o = &d->addrs[k];
                    if (!name_eq(o->host, name))
                        continue;
                    if (o->ip == ip)
                        a = o;
                    /* The host says these are all its addresses (on the network it sent
                     * this on, which the sender's address stands for): older others go soon */
                    else if (flush && o->heard_from == sender && o->added < now - 1 && o->until > now + 1)
                        o->until = now + 1;
                }
                if (!a && d->n_addrs < MAX_ADDRS && strlen(name) < sizeof a->host)
                {
                    a = &d->addrs[d->n_addrs++];
                    strcpy(a->host, name);
                    a->ip = ip;
                    a->heard_from = sender;
                    a->added = now;
                }
                if (a)
                    a->until = until;
            }
        }
    }
}

/* ---- the list of drawings ---- */
/* Forget what's expired, and work out what's nearby now */
static void resolve(discovery *d, double now)
{
    for (int i = d->n_addrs - 1; i >= 0; i--)
        if (d->addrs[i].until <= now)
            d->addrs[i] = d->addrs[--d->n_addrs];
    for (int i = d->n_seen - 1; i >= 0; i--)
        if (d->seen[i].ptr_until <= now && d->seen[i].srv_until <= now && d->seen[i].txt_until <= now)
            d->seen[i] = d->seen[--d->n_seen];

    nearby now_found[MAX_SEEN];
    int n = 0;
    for (int i = 0; i < d->n_seen; i++)
    {
        seen *s = &d->seen[i];
        if (s->ptr_until <= now || s->srv_until <= now || !strcmp(s->fullname, d->fullname))
            continue;
        uint32_t ip = 0;
        for (int k = 0; k < d->n_addrs; k++)
            if (name_eq(d->addrs[k].host, s->host) && (!ip || d->addrs[k].ip < ip))
                ip = d->addrs[k].ip;
        if (!ip)
            continue;
        nearby *f = &now_found[n++];
        memset(f, 0, sizeof *f);
        strcpy(f->fullname, s->fullname);
        snprintf(f->name, sizeof f->name, "%s", s->txt_until > now && s->has_name ? s->name : "A friend");
        f->kids = s->txt_until > now ? s->kids : 1;
        f->compatible = s->txt_until > now && s->v_ok;
        f->addr = (os_addr){ ip, s->port };
    }
    /* Keep the order things were found in: update, drop, then add the new ones */
    int changed = 0, kept = 0;
    for (int i = 0; i < d->n_found; i++)
    {
        int k = 0;
        while (k < n && strcmp(now_found[k].fullname, d->found[i].fullname))
            k++;
        if (k == n)
        {
            changed = 1;
            continue;
        }
        changed |= memcmp(&d->found[i], &now_found[k], sizeof(nearby)) != 0;
        d->found[kept++] = now_found[k];
        now_found[k].fullname[0] = 0;
    }
    d->n_found = kept;
    for (int k = 0; k < n; k++)
        if (now_found[k].fullname[0])
        {
            d->found[d->n_found++] = now_found[k];
            changed = 1;
        }
    if (changed)
        d->changed = 1;
}

/* Ask about drawings that aren't fully known yet, at most once a second each */
static void ask_missing(discovery *d, double now)
{
    for (int i = 0; i < d->n_seen; i++)
    {
        seen *s = &d->seen[i];
        if (s->ptr_until <= now || s->next_ask > now || !strcmp(s->fullname, d->fullname))
            continue;
        int has_addr = 0;
        for (int k = 0; k < d->n_addrs; k++)
            has_addr |= name_eq(d->addrs[k].host, s->host);
        if (s->srv_until > now && s->txt_until > now && has_addr)
            continue;
        pkt k;
        start(&k, 0, 0);
        question(&k, s->fullname, T_SRV);
        question(&k, s->fullname, T_TXT);
        if (s->srv_until > now)
            question(&k, s->host, T_A);
        multicast(d, &k, d->n_ips ? d->ips[0] : 0);
        s->next_ask = now + 1;
    }
}

/* ---- the thread ---- */
static void refresh_ips(discovery *d)
{
    d->n_ips = os_local_ips(d->ips, MAX_IPS);
    os_udp_join(d->sock, MDNS_GROUP, 0);
    for (int i = 0; i < d->n_ips; i++)
        os_udp_join(d->sock, MDNS_GROUP, d->ips[i]);    /* fails harmlessly when already joined */
}

static void mdns_main(void *arg)
{
    discovery *d = arg;
    for (;;)
    {
        os_addr from;
        int n = os_udp_recv(d->sock, d->buf, RECV_CAP, &from, 200);
        double now = os_now();
        os_lock(d->m);
        if (now >= d->next_ips)
        {
            refresh_ips(d);
            d->next_ips = now + 30;
        }
        if (n >= 12)
        {
            unsigned flags = get16(d->buf + 2);
            if (!(flags & 0x8000) && d->advertising)
                answer(d, d->buf, n, from);
            else if (flags & 0x8000 && d->browsing)
                take_records(d, d->buf, n, from.ip, now);
        }
        if (d->goodbye)
        {
            announce(d, &d->bye, 1);
            d->goodbye = 0;
        }
        if (d->stop)
        {
            os_unlock(d->m);
            break;
        }
        if (d->advertising && d->announce_left && now >= d->next_announce)
        {
            announce(d, &d->mine, 0);
            d->announce_left--;
            d->next_announce = now + 1;
        }
        if (d->browsing)
        {
            if (now >= d->next_browse)
            {
                pkt k;
                start(&k, 0, 0);
                question(&k, SERVICE, T_PTR);
                for (int i = 0; i < (d->n_ips ? d->n_ips : 1); i++)
                    multicast(d, &k, d->n_ips ? d->ips[i] : 0);
                d->next_browse = now + d->browse_gap;
                d->browse_gap = d->browse_gap * 2 < 60 ? d->browse_gap * 2 : 60;
            }
            ask_missing(d, now);
            resolve(d, now);
        }
        int changed = d->changed;
        os_unlock(d->m);
        if (changed && d->wake)
            d->wake();
    }
}

/* ---- the app's side ---- */
discovery *discovery_new(void (*wake)(void))
{
    os_sock s = os_udp_open(MDNS_PORT, 1);
    if (s == OS_BAD_SOCK)
        return NULL;
    discovery *d = calloc(1, sizeof *d);
    d->sock = s;
    d->wake = wake;
    d->m = os_mutex_new();
    /* A name for this app on the network, unique enough not to clash */
    uint32_t r;
    os_random(&r, sizeof r);
    snprintf(d->host, sizeof d->host, "paint-%08x.local.", (unsigned)(r ^ (uint32_t)os_getpid() << 20));
    snprintf(d->fullname, sizeof d->fullname, "%.14s.%s", d->host, SERVICE);
    refresh_ips(d);
    d->next_ips = os_now() + 30;
    d->th = os_thread_start(mdns_main, d);
    if (!d->th)
    {
        os_sock_close(s);
        os_mutex_free(d->m);
        free(d);
        return NULL;
    }
    return d;
}

void discovery_free(discovery *d)
{
    if (!d)
        return;
    discovery_stop_advertising(d);
    os_lock(d->m);
    d->stop = 1;
    os_unlock(d->m);
    os_thread_join(d->th);
    os_sock_close(d->sock);
    os_mutex_free(d->m);
    free(d->shown);
    free(d);
}

void discovery_browse(discovery *d)
{
    os_lock(d->m);
    if (!d->browsing)
    {
        d->browsing = 1;
        d->next_browse = 0;
        d->browse_gap = 1;
    }
    os_unlock(d->m);
}

int discovery_poll(discovery *d)
{
    os_lock(d->m);
    int changed = d->changed;
    if (changed)
    {
        d->shown = realloc(d->shown, sizeof(nearby) * (size_t)(d->n_found ? d->n_found : 1));
        memcpy(d->shown, d->found, sizeof(nearby) * (size_t)d->n_found);
        d->n_shown = d->n_found;
        d->changed = 0;
    }
    os_unlock(d->m);
    return changed;
}

const nearby *discovery_nearby(const discovery *d, int *n)
{
    *n = d->n_shown;
    return d->shown;
}

void discovery_advertise(discovery *d, const char *name, int kids, uint16_t port)
{
    os_lock(d->m);
    snprintf(d->mine.name, sizeof d->mine.name, "%s", name);
    d->mine.kids = kids;
    d->mine.port = port;
    d->advertising = 1;
    d->announce_left = 2;
    d->next_announce = 0;
    os_unlock(d->m);
}

void discovery_stop_advertising(discovery *d)
{
    os_lock(d->m);
    if (d->advertising)
    {
        d->bye = d->mine;
        d->goodbye = 1;
        d->advertising = 0;
    }
    os_unlock(d->m);
}
