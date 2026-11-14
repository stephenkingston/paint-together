/*
 * Paint (C port) - an X11 client over the wire protocol (see x11_wire.h)
 *
 * The socket is non-blocking: requests collect in an output buffer that is
 * written out when it fills or before any wait; input is read into a buffer
 * and cut into packets (32-byte events and errors, replies of any length).
 * Events are queued in arrival order for the host to take. Replies (and
 * errors) to requests sent in a batch are kept until they are asked for; one
 * nobody waits for any more is dropped when a later one is.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "host/x11_wire.h"

xw_display xw = { .fd = -1 };

static uint8_t *out, *in, *reply_buf, *events;
static size_t out_len, out_cap, in_len, in_cap, reply_cap, reply_len;
static int n_events, cap_events, debug;
static uint32_t id_base, id_mask, id_next;
static unsigned want_seq;               /* the reply being waited for */
static int want_state;                  /* 0 waiting, 1 got it, -1 an error came instead */
static int first_error;                 /* since the last xw_sync */

typedef struct stored
{
    struct stored *next;
    unsigned seq;
    size_t len;
    uint8_t data[];
} stored;
static stored *early;                   /* replies and errors that came before they were waited for */

static size_t pad4(size_t n)
{
    return (n + 3) & ~(size_t)3;
}

/* ---- connecting ---- */

static int connect_unix(int number)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int len = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "/tmp/.X11-unix/X%d", number);
    /* Linux's abstract name first (it also works from a container without /tmp), then the file */
    if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + len)) == 0)
        return fd;
    memmove(addr.sun_path, addr.sun_path + 1, (size_t)len + 1);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        return fd;
    if (fd >= 0)
        close(fd);
    return -1;
}

static int connect_tcp(const char *host, int number)
{
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *list, *ai;
    char port[16];
    int fd = -1;
    snprintf(port, sizeof(port), "%d", 6000 + number);
    if (getaddrinfo(host, port, &hints, &list) != 0)
        return -1;
    for (ai = list; ai && fd < 0; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) != 0)
        {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(list);
    return fd;
}

static unsigned get_be16(const uint8_t *at)
{
    return (unsigned)at[0] << 8 | at[1];
}

/* The MIT-MAGIC-COOKIE-1 for this display from $XAUTHORITY or ~/.Xauthority: 16 bytes, or 0 if none */
static int find_cookie(int number, int local, uint8_t cookie[16])
{
    const char *file = getenv("XAUTHORITY"), *home = getenv("HOME");
    char path[1024], host[256] = "", num[16];
    if (!file || !*file)
    {
        snprintf(path, sizeof(path), "%s/.Xauthority", home ? home : "");
        file = path;
    }
    gethostname(host, sizeof(host) - 1);
    snprintf(num, sizeof(num), "%d", number);
    FILE *f = fopen(file, "rb");
    if (!f)
        return 0;
    uint8_t rec[4 * 2 + 4 * 1024];
    int found = 0;
    for (;;)
    {   /* family, then address, number, name, data: each a big-endian length and bytes */
        size_t at = 2;
        const uint8_t *field[4];
        size_t len[4];
        if (fread(rec, 1, 2, f) != 2)
            break;
        int ok = 1;
        for (int idx = 0; idx < 4 && ok; idx++)
        {
            ok = fread(rec + at, 1, 2, f) == 2;
            len[idx] = ok ? get_be16(rec + at) : 0;
            at += 2;
            ok = ok && at + len[idx] <= sizeof(rec) && fread(rec + at, 1, len[idx], f) == len[idx];
            field[idx] = rec + at;
            at += len[idx];
        }
        if (!ok)
            break;
        unsigned family = get_be16(rec);
        int this_host = family == 65535 || (family == 256 && len[0] == strlen(host) && !memcmp(field[0], host, len[0])) ||
                        (!local && (family == 0 || family == 6));
        int this_number = !len[1] || (len[1] == strlen(num) && !memcmp(field[1], num, len[1]));
        if (this_number && len[2] == 18 && !memcmp(field[2], "MIT-MAGIC-COOKIE-1", 18) && len[3] == 16)
        {   /* (a cookie for another host name is kept in case the name has changed since) */
            if (this_host || !found)
                memcpy(cookie, field[3], 16);
            found = this_host ? 2 : found ? found : 1;
            if (found == 2)
                break;
        }
    }
    fclose(f);
    return found != 0;
}

static int write_all(const uint8_t *data, size_t len)
{
    while (len && !xw.dead)
    {
        ssize_t put = write(xw.fd, data, len);
        if (put > 0)
        {
            data += put;
            len -= (size_t)put;
        }
        else if (put < 0 && (errno == EAGAIN || errno == EINTR))
        {
            struct pollfd pfd = { xw.fd, POLLOUT, 0 };
            poll(&pfd, 1, 1000);
        }
        else
            xw.dead = 1;
    }
    return xw.dead ? -1 : 0;
}

/* Blocking read of exactly len bytes (only during the setup, before the socket is non-blocking) */
static int read_all(uint8_t *data, size_t len)
{
    while (len)
    {
        ssize_t got = read(xw.fd, data, len);
        if (got <= 0 && !(got < 0 && errno == EINTR))
            return -1;
        data += got > 0 ? got : 0;
        len -= got > 0 ? (size_t)got : 0;
    }
    return 0;
}

/* Pick the screen's 24-bit TrueColor visual (0xRRGGBB, 32 bits a pixel), else a 32-bit one */
static int parse_setup(const uint8_t *s, size_t len, int screen)
{
    size_t vendor = xw_get16(s + 24 - 8), at = 40 - 8 + pad4(vendor);
    int formats = s[29 - 8], screens = s[28 - 8], bpp24 = 0, bpp32 = 0;
    id_base = xw_get32(s + 12 - 8);
    id_mask = xw_get32(s + 16 - 8);
    xw.max_req = 4 * (size_t)xw_get16(s + 26 - 8);
    xw.min_keycode = s[34 - 8];
    xw.max_keycode = s[35 - 8];
    int lsb_server = s[30 - 8] == 0;
    uint16_t probe = 1;
    xw.swap_images = lsb_server != (*(uint8_t *)&probe == 1);
    for (int idx = 0; idx < formats && at + 8 <= len; idx++, at += 8)
    {
        bpp24 |= s[at] == 24 && s[at + 1] == 32;
        bpp32 |= s[at] == 32 && s[at + 1] == 32;
    }
    for (int scr = 0; scr < screens && at + 40 <= len; scr++)
    {
        const uint8_t *sc = s + at;
        int depths = sc[39];
        uint32_t root_visual = xw_get32(sc + 32), best = 0;
        int best_depth = 0, best_rank = 0;
        at += 40;
        for (int d = 0; d < depths && at + 8 <= len; d++)
        {
            int depth = s[at], visuals = (int)xw_get16(s + at + 2);
            at += 8;
            for (int v = 0; v < visuals && at + 24 <= len; v++, at += 24)
            {
                const uint8_t *vis = s + at;
                int fits = vis[4] == 4 && xw_get32(vis + 8) == 0xFF0000 && xw_get32(vis + 12) == 0xFF00 &&
                           xw_get32(vis + 16) == 0xFF && ((depth == 24 && bpp24) || (depth == 32 && bpp32));
                int rank = !fits ? 0 : depth == 24 && xw_get32(vis) == root_visual ? 3 : depth == 24 ? 2 : 1;
                if (rank > best_rank)
                {
                    best_rank = rank;
                    best = xw_get32(vis);
                    best_depth = depth;
                }
            }
        }
        if (scr == screen)
        {
            xw.root = xw_get32(sc);
            xw.colormap = best == root_visual ? xw_get32(sc + 4) : 0;
            xw.screen_w = (int)xw_get16(sc + 20);
            xw.screen_h = (int)xw_get16(sc + 22);
            xw.visual = best;
            xw.depth = best_depth;
            return best ? 0 : -1;
        }
    }
    return -1;
}

int xw_open(void)
{
    const char *display = getenv("DISPLAY"), *colon = display ? strrchr(display, ':') : NULL;
    char host[256] = "";
    uint8_t cookie[16];
    debug = getenv("PAINT_X11DEBUG") && atoi(getenv("PAINT_X11DEBUG"));
    if (!colon)
    {
        fprintf(stderr, "x11: DISPLAY is not set\n");
        return -1;
    }
    int number = atoi(colon + 1), screen = strchr(colon, '.') ? atoi(strchr(colon, '.') + 1) : 0;
    size_t host_len = (size_t)(colon - display) < sizeof(host) ? (size_t)(colon - display) : sizeof(host) - 1;
    memcpy(host, display, host_len);
    host[host_len] = 0;
    xw.local = !*host || !strcmp(host, "unix");
    xw.fd = xw.local ? connect_unix(number) : connect_tcp(host, number);
    if (xw.fd < 0)
    {
        fprintf(stderr, "x11: cannot connect to %s\n", display);
        return -1;
    }
    int auth = find_cookie(number, xw.local, cookie);
    uint8_t setup[12 + 20 + 16] = { 0 };
    uint16_t probe = 1;
    setup[0] = *(uint8_t *)&probe == 1 ? 'l' : 'B';
    xw_put16(setup + 2, 11);
    if (auth)
    {
        xw_put16(setup + 6, 18);
        xw_put16(setup + 8, 16);
        memcpy(setup + 12, "MIT-MAGIC-COOKIE-1", 18);
        memcpy(setup + 32, cookie, 16);
    }
    uint8_t head[8];
    if (write_all(setup, auth ? sizeof(setup) : 12) || read_all(head, 8))
    {
        fprintf(stderr, "x11: the display hung up\n");
        close(xw.fd);
        return -1;
    }
    size_t len = 4 * (size_t)xw_get16(head + 6);
    uint8_t *rest = malloc(len + 1);
    if (read_all(rest, len) || head[0] != 1 || parse_setup(rest, len, screen))
    {
        if (head[0] != 1)
            fprintf(stderr, "x11: the display refused us: %.*s\n", head[0] == 0 ? head[1] : (int)len, (char *)rest);
        else
            fprintf(stderr, "x11: no 24-bit TrueColor visual on %s\n", display);
        free(rest);
        close(xw.fd);
        return -1;
    }
    free(rest);
    fcntl(xw.fd, F_SETFL, fcntl(xw.fd, F_GETFL) | O_NONBLOCK);
    id_next = 1;
    if (!xw.colormap)
    {   /* a visual other than the root's needs its own colormap */
        xw.colormap = xw_new_id();
        uint8_t *req = xw_req(78, 0, 16);
        xw_put32(req + 4, xw.colormap);
        xw_put32(req + 8, xw.root);
        xw_put32(req + 12, xw.visual);
    }
    return 0;
}

uint32_t xw_new_id(void)
{
    uint32_t step = id_mask & (~id_mask + 1);   /* the mask's lowest bit */
    return id_base | ((id_next++ * step) & id_mask);
}

/* ---- requests ---- */

uint8_t *xw_req(int opcode, int data, size_t len)
{
    len = pad4(len);
    if (out_len + len > out_cap)
    {
        xw_flush();
        if (len > out_cap)
        {
            out_cap = len > 65536 ? len : 65536;
            out = realloc(out, out_cap);
        }
    }
    uint8_t *req = out + out_len;
    memset(req, 0, len);
    req[0] = (uint8_t)opcode;
    req[1] = (uint8_t)data;
    xw_put16(req + 2, (unsigned)(len / 4));
    out_len += len;
    xw.seq++;
    return req;
}

void xw_req32(int opcode, uint32_t value)
{
    xw_put32(xw_req(opcode, 0, 8) + 4, value);
}

void xw_flush(void)
{
    if (out_len && !xw.dead)
        write_all(out, out_len);
    out_len = 0;
}

/* ---- input ---- */

static unsigned full_seq(unsigned low16)
{
    unsigned seq = (xw.seq & ~0xFFFFu) | low16;
    return seq > xw.seq ? seq - 0x10000 : seq;
}

static void keep_reply(const uint8_t *p, size_t size, unsigned seq)
{
    stored *s = malloc(sizeof(stored) + size), **at = &early;
    s->next = NULL;
    s->seq = seq;
    s->len = size;
    memcpy(s->data, p, size);
    while (*at)
        at = &(*at)->next;
    *at = s;
}

static void take_reply(const uint8_t *p, size_t size)
{
    if (size > reply_cap)
    {
        reply_cap = size;
        reply_buf = realloc(reply_buf, reply_cap);
    }
    memcpy(reply_buf, p, size);
    reply_len = size;
    want_state = p[0] == XE_REPLY ? 1 : -1;
}

static void queue_event(const uint8_t *ev)
{
    if (n_events == cap_events)
    {
        cap_events = cap_events ? cap_events * 2 : 64;
        events = realloc(events, 32 * (size_t)cap_events);
    }
    memcpy(events + 32 * (size_t)n_events++, ev, 32);
}

/* Cut the input into packets: events are queued, the awaited reply kept, errors noted */
static void cut_packets(void)
{
    size_t at = 0;
    while (in_len - at >= 32)
    {
        const uint8_t *p = in + at;
        size_t size = p[0] == XE_REPLY || (p[0] & 0x7F) == 35 ? 32 + 4 * (size_t)xw_get32(p + 4) : 32;
        if (in_len - at < size)
            break;
        unsigned seq = full_seq(xw_get16(p + 2));
        if (p[0] == XE_ERROR)
        {
            if (debug)
                fprintf(stderr, "x11: error %d for request %d.%d (seq %u, value 0x%x)\n", p[1], p[10],
                        xw_get16(p + 8), seq, xw_get32(p + 4));
            first_error = first_error ? first_error : p[1];
        }
        if (p[0] > XE_REPLY)
            queue_event(p);
        else if (seq == want_seq && !want_state)
            take_reply(p, size);
        else if ((int)(seq - want_seq) > 0 || !want_seq)
            keep_reply(p, size, seq);
        at += size;
    }
    memmove(in, in + at, in_len - at);
    in_len -= at;
    if (!in_len && in_cap > 1 << 20)
    {   /* (a big reply came: do not keep its room) */
        free(in);
        in = NULL;
        in_cap = 0;
    }
}

int xw_read(void)
{
    for (;;)
    {
        if (in_cap - in_len < 4096)
        {
            in_cap = in_cap ? in_cap * 2 : 65536;
            in = realloc(in, in_cap);
        }
        ssize_t got = xw.dead ? -1 : read(xw.fd, in + in_len, in_cap - in_len);
        if (got > 0)
        {
            in_len += (size_t)got;
            cut_packets();
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            xw.dead = 1;
        return xw.dead ? -1 : 0;
    }
}

int xw_wait(int timeout_ms, int fd)
{
    struct pollfd pfd[2] = { { xw.fd, POLLIN, 0 }, { fd, POLLIN, 0 } };
    xw_flush();
    if (xw.dead)
        return -1;
    int ready = poll(pfd, fd >= 0 ? 2 : 1, timeout_ms);
    if (ready <= 0)
        return 0;
    if (pfd[0].revents && xw_read() < 0)
        return -1;
    return (pfd[0].revents ? 1 : 0) | (fd >= 0 && pfd[1].revents ? 2 : 0);
}

const uint8_t *xw_reply(unsigned seq, size_t *len)
{
    want_seq = seq;
    want_state = 0;
    while (early && (int)(early->seq - seq) <= 0)
    {   /* the stale ones go */
        stored *s = early;
        early = s->next;
        if (s->seq == seq)
            take_reply(s->data, s->len);
        free(s);
    }
    for (int waited = 0; !want_state && waited < 10000 && !xw.dead;)
    {
        if (xw_wait(1000, -1) == 0)
            waited += 1000;
    }
    want_seq = 0;
    if (len)
        *len = reply_len;
    return want_state == 1 ? reply_buf : NULL;
}

int xw_sync(void)
{
    xw_req(43, 0, 4);       /* GetInputFocus: its reply comes after every earlier error */
    xw_reply(xw.seq, NULL);
    int error = first_error;
    first_error = 0;
    return error;
}

int xw_take(uint8_t ev[32], int (*match)(const uint8_t *ev, void *arg), void *arg)
{
    for (int idx = 0; idx < n_events; idx++)
    {
        uint8_t *at = events + 32 * (size_t)idx;
        if (!match || match(at, arg))
        {
            memcpy(ev, at, 32);
            memmove(at, at + 32, 32 * (size_t)(n_events - idx - 1));
            n_events--;
            return 1;
        }
    }
    return 0;
}

const uint8_t *xw_peek(void)
{
    return n_events ? events : NULL;
}

int xw_ext(const char *name, int *first_event)
{
    size_t len = strlen(name);
    uint8_t *req = xw_req(98, 0, 8 + len);
    xw_put16(req + 4, (unsigned)len);
    memcpy(req + 8, name, len);
    const uint8_t *reply = xw_reply(xw.seq, NULL);
    if (first_event)
        *first_event = reply ? reply[10] : 0;
    return reply && reply[8] ? reply[9] : 0;
}

/* ---- atoms, properties, events ---- */

static unsigned intern(const char *name)
{
    size_t len = strlen(name);
    uint8_t *req = xw_req(16, 0, 8 + len);
    xw_put16(req + 4, (unsigned)len);
    memcpy(req + 8, name, len);
    return xw.seq;
}

uint32_t xw_atom(const char *name)
{
    const uint8_t *reply = xw_reply(intern(name), NULL);
    return reply ? xw_get32(reply + 8) : 0;
}

void xw_atoms(const char *const *names, int count, uint32_t *atoms)
{
    unsigned first = xw.seq + 1;
    for (int idx = 0; idx < count; idx++)
        intern(names[idx]);
    for (int idx = 0; idx < count; idx++)
    {
        const uint8_t *reply = xw_reply(first + (unsigned)idx, NULL);
        atoms[idx] = reply ? xw_get32(reply + 8) : 0;
    }
}

void xw_set_prop(uint32_t win, uint32_t prop, uint32_t type, int format, const void *data, size_t count)
{
    size_t unit = (size_t)format / 8, total = count * unit, piece = (xw.max_req - 24) & ~(size_t)3, done = 0;
    do
    {   /* Replace with the first piece, Append the rest */
        size_t len = total - done < piece ? total - done : piece;
        uint8_t *req = xw_req(18, done ? 2 : 0, 24 + len);
        xw_put32(req + 4, win);
        xw_put32(req + 8, prop);
        xw_put32(req + 12, type);
        req[16] = (uint8_t)format;
        xw_put32(req + 20, (uint32_t)(len / unit));
        if (len)
            memcpy(req + 24, (const uint8_t *)data + done, len);
        done += len;
    } while (done < total);
}

uint8_t *xw_get_prop(uint32_t win, uint32_t prop, uint32_t type, int del, size_t *bytes, uint32_t *type_out)
{
    uint8_t *req = xw_req(20, del, 24);
    xw_put32(req + 4, win);
    xw_put32(req + 8, prop);
    xw_put32(req + 12, type);
    xw_put32(req + 20, 0x4000000);  /* up to 256 MB in one reply */
    const uint8_t *reply = xw_reply(xw.seq, NULL);
    uint32_t got_type = reply ? xw_get32(reply + 8) : 0;
    if (type_out)
        *type_out = got_type;
    if (!got_type || (type && got_type != type))
        return NULL;
    size_t len = (size_t)xw_get32(reply + 16) * (reply[1] / 8);
    uint8_t *data = malloc(len + 1);
    if (data)
    {
        memcpy(data, reply + 32, len);
        data[len] = 0;
    }
    if (reply_cap > 1 << 20)
    {   /* a picture's worth: give the room back */
        free(reply_buf);
        reply_buf = NULL;
        reply_cap = 0;
    }
    if (bytes)
        *bytes = len;
    return data;
}

uint32_t xw_get_prop32(uint32_t win, uint32_t prop, uint32_t type, size_t index, uint32_t fallback)
{
    size_t len;
    uint8_t *data = xw_get_prop(win, prop, type, 0, &len, NULL);
    uint32_t value = data && len >= 4 * (index + 1) ? xw_get32(data + 4 * index) : fallback;
    free(data);
    return value;
}

void xw_send_event(uint32_t dest, uint32_t mask, const uint8_t ev[32])
{
    uint8_t *req = xw_req(25, 0, 44);
    xw_put32(req + 4, dest);
    xw_put32(req + 8, mask);
    memcpy(req + 12, ev, 32);
}

void xw_client_message(uint32_t dest, uint32_t mask, uint32_t win, uint32_t type, const uint32_t data[5])
{
    uint8_t ev[32] = { XE_CLIENT_MESSAGE, 32 };
    xw_put32(ev + 4, win);
    xw_put32(ev + 8, type);
    memcpy(ev + 12, data, 20);
    xw_send_event(dest, mask, ev);
}

void xw_put_image(uint32_t drawable, uint32_t gc, const uint32_t *px, int stride, int x, int y, int w, int h)
{
    int rows = (int)((xw.max_req - 24) / (4 * (size_t)w));
    rows = rows < 1 ? 1 : rows;
    for (int top = 0; top < h; top += rows)
    {
        int count = h - top < rows ? h - top : rows;
        uint8_t *req = xw_req(72, 2, 24 + 4 * (size_t)w * (size_t)count);    /* ZPixmap */
        xw_put32(req + 4, drawable);
        xw_put32(req + 8, gc);
        xw_put16(req + 12, (unsigned)w);
        xw_put16(req + 14, (unsigned)count);
        xw_put16(req + 16, (unsigned)x);
        xw_put16(req + 18, (unsigned)(y + top));
        req[21] = (uint8_t)xw.depth;
        uint32_t *dst = (uint32_t *)(req + 24);
        for (int row = 0; row < count; row++, dst += w)
        {
            memcpy(dst, px + (size_t)(top + row) * (size_t)stride, 4 * (size_t)w);
            for (int col = 0; xw.swap_images && col < w; col++)
                dst[col] = __builtin_bswap32(dst[col]);
        }
    }
}
