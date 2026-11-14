/*
 * Paint (C port) - drawing together over real sockets on this computer: host
 * and friend, a quiet friend staying connected, the door (waiting hears
 * nothing until let in; kicked friends hang up), other versions turned away,
 * and a friend taking over as host (Rust: the tests in collab/net.rs)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "collab/net.h"
#include "collab/session.h"

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

#define GIVE_UP_MS 400

static const uint8_t C1[3] = { 1, 2, 3 }, C2[3] = { 4, 5, 6 }, C3[3] = { 7, 8, 9 };

static img *white(int w, int h)
{
    return img_new(w, h, (rgba){ 255, 255, 255, 255 });
}

static char *addr_of(link *host, char *out)
{
    os_addr a;
    link_hosting_at(host, &a);
    a.ip = 0x7F000001;
    os_addr_format(a, out, 32);
    return out;
}

/* The next message on `link` of `kind` (others skipped), within 5 s; closing fails the test */
static int wait_for(link *l, int kind, from_hub *out)
{
    double until = os_now() + 5;
    while (os_now() < until)
    {
        link_event e;
        if (!link_recv(l, &e))
        {
            os_sleep_ms(5);
            continue;
        }
        if (e.kind == LINK_CLOSED)
        {
            fprintf(stderr, "closed: %s\n", e.why);
            link_event_free(&e);
            return 0;
        }
        if (e.msg.kind == kind)
        {
            *out = e.msg;
            return 1;
        }
        link_event_free(&e);
    }
    fprintf(stderr, "message %d never arrived\n", kind);
    return 0;
}

/* Whether `link` closes within 5 s */
static int closes(link *l)
{
    double until = os_now() + 5;
    while (os_now() < until)
    {
        link_event e;
        if (link_recv(l, &e))
        {
            int closed = e.kind == LINK_CLOSED;
            link_event_free(&e);
            if (closed)
                return 1;
        }
        else
            os_sleep_ms(10);
    }
    return 0;
}

static int px(int x, int y, uint8_t out[4], void *user)
{
    memcpy(out, "\x09\x09\x09\xFF", 4);
    return 1;
}

static to_hub edit_msg(uint64_t id, irect r)
{
    to_hub m = { 0 };
    m.kind = TO_EDIT;
    m.id = id;
    m.edit.kind = EDIT_PATCH;
    patch_build(r, px, NULL, &m.edit.p);
    return m;
}

static void host_and_friend(void)
{
    char err[200], addr[32];
    link *host = link_host(white(64, 48), "Host", C1, 0, NULL, err, sizeof err);
    CHECK(host);
    link *friend = link_join(addr_of(host, addr), "Friend", C2, OS_BAD_SOCK, NULL, err, sizeof err);
    from_hub hw, fw, m;
    CHECK(wait_for(host, FROM_WELCOME, &hw));
    CHECK(wait_for(friend, FROM_WELCOME, &fw));
    CHECK(fw.picture.w == 64 && fw.picture.h == 48 && fw.kids.n == 2 && fw.host == hw.you && fw.you != hw.you);
    for (int i = 0; i < fw.kids.n; i++)
        if (fw.kids.v[i].id == fw.you)
            CHECK(!strncmp(fw.kids.v[i].addr, "127.0.0.1:", 10));   /* reachable for a takeover */
    CHECK(wait_for(host, FROM_KIDS, &m) && m.kids.n == 2);
    from_hub_free(&m);

    /* The friend's edit comes back to both, in order */
    to_hub e = edit_msg(1, irect_make(1, 1, 3, 3));
    patch sent = patch_clone(&e.edit.p);
    link_send(friend, e);
    link *both[2] = { host, friend };
    for (int i = 0; i < 2; i++)
    {
        CHECK(wait_for(both[i], FROM_SEQUENCED, &m));
        CHECK(m.seq == 1 && m.from == fw.you && m.id == 1 && m.edit.kind == EDIT_PATCH && patch_eq(&m.edit.p, &sent));
        from_hub_free(&m);
    }
    patch_free(&sent);

    /* The host leaving ends the friend's session */
    link_free(host);
    CHECK(closes(friend));
    link_free(friend);
    from_hub_free(&hw);
    from_hub_free(&fw);
}

static void quiet_friend_stays(void)
{
    char err[200], addr[32];
    link *host = link_host(white(32, 32), "Host", C1, 0, NULL, err, sizeof err);
    link *chatty = link_join(addr_of(host, addr), "Chatty", C2, OS_BAD_SOCK, NULL, err, sizeof err);
    link *quiet = link_join(addr, "Quiet", C3, OS_BAD_SOCK, NULL, err, sizeof err);
    /* For several times the give-up time, one friend keeps the hub busy while the
     * other sends nothing of its own and has nothing new to hear */
    double until = os_now() + GIVE_UP_MS * 5 / 1000.0;
    int dropped = 0;
    while (os_now() < until)
    {
        to_hub p = { 0 };
        p.kind = TO_PRESENCE;
        p.presence = (presence){ 1, 1, 1, HELD_OTHER, { 0 }, 0 };
        link_send(chatty, p);
        os_sleep_ms(20);
        link *all[3] = { host, chatty, quiet };
        for (int i = 0; i < 3; i++)
        {
            link_event e;
            while (link_recv(all[i], &e))
            {
                if (e.kind == LINK_CLOSED)
                {
                    fprintf(stderr, "a connection dropped: %s\n", e.why);
                    dropped = 1;
                }
                link_event_free(&e);
            }
        }
    }
    CHECK(!dropped);
    link_free(quiet);
    link_free(chatty);
    link_free(host);
}

static void door(void)
{
    char err[200], addr[32];
    link *host = link_host(white(32, 32), "Host", C1, 1, NULL, err, sizeof err);
    link *friend = link_join(addr_of(host, addr), "Friend", C2, OS_BAD_SOCK, NULL, err, sizeof err);
    from_hub hw, m;
    CHECK(wait_for(host, FROM_WELCOME, &hw));
    CHECK(wait_for(friend, FROM_ASKING, &m) && !strcmp(m.text, "Host"));
    from_hub_free(&m);
    CHECK(wait_for(host, FROM_WAITING, &m) && m.kids.n == 1);
    peer_id friend_id = m.kids.n ? m.kids.v[0].id : 0;
    from_hub_free(&m);

    /* Waiting for longer than a quiet connection lasts, hearing nothing of the drawing */
    link_send(host, edit_msg(1, irect_make(1, 1, 3, 3)));
    double until = os_now() + GIVE_UP_MS * 2 / 1000.0;
    while (os_now() < until)
    {
        link_event e;
        if (link_recv(friend, &e))
        {
            fprintf(stderr, "heard %d (%s) before being let in\n", e.kind == LINK_MESSAGE ? e.msg.kind : -1, e.why ? e.why : "");
            CHECK(0);
            link_event_free(&e);
        }
        os_sleep_ms(10);
    }

    to_hub yes = { 0 };
    yes.kind = TO_LET_IN;
    yes.kid = friend_id;
    yes.yes = 1;
    link_send(host, yes);
    CHECK(wait_for(friend, FROM_WELCOME, &m) && m.seq == 1);    /* the picture so far comes with the welcome */
    img *pic = picture_to_image(&m.picture);
    CHECK(pic && !memcmp(img_at(pic, 2, 2), "\x09\x09\x09\xFF", 4));
    img_free(pic);
    from_hub_free(&m);

    to_hub kick = { 0 };
    kick.kind = TO_KICK;
    kick.kid = friend_id;
    link_send(host, kick);
    CHECK(wait_for(friend, FROM_KICKED_OUT, &m) && m.by == hw.you);
    from_hub_free(&m);
    CHECK(closes(friend));
    link_free(friend);
    link_free(host);
    from_hub_free(&hw);
}

/* Another version of Paint (or a stray connection) is turned away */
static void other_versions(void)
{
    char err[200];
    link *host = link_host(white(8, 8), "Host", C1, 0, NULL, err, sizeof err);
    os_addr a;
    link_hosting_at(host, &a);
    a.ip = 0x7F000001;
    os_sock s = os_tcp_connect(a, 1000);
    hello h = { { 'P', 'N', 'T', '!' }, 1, "Old", { 0 }, 47100 };
    bytes raw = { 0 };
    hello_encode(&h, &raw);
    size_t len;
    uint8_t *frame = net_frame(&raw, &len);
    os_send_all(s, frame, (int)len);
    free(frame);
    os_sock_timeout(s, 3000, 0);
    uint8_t *body;
    size_t blen;
    from_hub m;
    int read_sock(void *ctx, void *buf, int n);
    CHECK(net_read_frame(read_sock, &s, &body, &blen) == 0 && from_hub_decode(body, blen, &m) == 0 && m.kind == FROM_REJECTED);
    CHECK(m.text && strstr(m.text, "different version"));
    free(body);
    from_hub_free(&m);
    char c;
    CHECK(os_recv(s, &c, 1) <= 0);      /* and hung up */
    os_sock_close(s);

    s = os_tcp_connect(a, 1000);
    os_send_all(s, "GET / HTTP/1.0\r\n\r\n", 18);
    CHECK(os_recv(s, &c, 1) <= 0);
    os_sock_close(s);
    link_free(host);
}

int read_sock(void *ctx, void *buf, int n)
{
    return os_recv_all(*(os_sock *)ctx, buf, n);
}

/* The host leaves: the earliest friend to join takes over with their spare
 * listener, and the other friend comes straight back in there */
static void handover(void)
{
    char err[200], addr[32];
    link *host = link_host(white(16, 16), "Host", C1, 1, NULL, err, sizeof err);
    link *a = link_join(addr_of(host, addr), "Ann", C2, OS_BAD_SOCK, NULL, err, sizeof err);
    from_hub hw, m;
    CHECK(wait_for(host, FROM_WELCOME, &hw));
    CHECK(wait_for(host, FROM_WAITING, &m) && m.kids.n == 1);
    to_hub yes = { 0 };
    yes.kind = TO_LET_IN;
    yes.kid = m.kids.v[0].id;
    yes.yes = 1;
    from_hub_free(&m);
    link_send(host, yes);
    from_hub aw;
    CHECK(wait_for(a, FROM_WELCOME, &aw));
    link *b = link_join(addr, "Bob", C3, OS_BAD_SOCK, NULL, err, sizeof err);
    while (wait_for(host, FROM_WAITING, &m) && m.kids.n == 0)
        from_hub_free(&m);      /* the door was empty once Ann came in */
    CHECK(m.kids.n == 1);
    yes.kid = m.kids.v[0].id;
    from_hub_free(&m);
    link_send(host, yes);
    from_hub bw;
    CHECK(wait_for(b, FROM_WELCOME, &bw));
    session *sa = session_new(aw.you, aw.host, aw.seq, picture_to_image(&aw.picture), aw.kids);
    session *sb = session_new(bw.you, bw.host, bw.seq, picture_to_image(&bw.picture), bw.kids);
    aw.kids = bw.kids = (kid_list){ 0 };
    CHECK(wait_for(a, FROM_KIDS, &m));
    session_receive(sa, &m);
    CHECK(sa->kids.n == 3 && sb->kids.n == 3);

    /* Bob draws a square that Ann hasn't heard about when the host goes */
    link_send(b, edit_msg(1, irect_make(0, 0, 4, 4)));
    CHECK(wait_for(a, FROM_SEQUENCED, &m));
    session_receive(sa, &m);
    link_free(host);
    CHECK(closes(a) && closes(b));

    /* Ann joined first, so she hosts now: everyone who was in comes straight back */
    os_sock spare_a = link_take_spare(a), spare_b = link_take_spare(b);
    link_free(a);
    link_free(b);
    hub *h = hub_new(img_dup(session_confirmed(sa)));
    hub_set_door(h, 1, kid_list_clone(&sa->kids));
    link *na = link_host_with(spare_a, h, "Ann", C2, NULL);
    char ann_addr[32] = "";
    for (int i = 0; i < sb->kids.n; i++)
        if (!strcmp(sb->kids.v[i].name, "Ann"))
            snprintf(ann_addr, sizeof ann_addr, "%s", sb->kids.v[i].addr);
    link *nb = link_join(ann_addr, "Bob", C3, spare_b, NULL, err, sizeof err);
    CHECK(wait_for(na, FROM_WELCOME, &m) && m.you == m.host);
    from_hub_free(&m);
    CHECK(wait_for(nb, FROM_WELCOME, &m) && m.seq == 0 && m.kids.n == 2);
    img *pic = picture_to_image(&m.picture);
    CHECK(pic && !memcmp(img_at(pic, 1, 1), "\x09\x09\x09\xFF", 4));   /* Bob's square came along */
    img_free(pic);
    from_hub_free(&m);
    link_free(nb);
    link_free(na);
    session_free(sa);
    session_free(sb);
    from_hub_free(&hw);
    from_hub_free(&aw);
    from_hub_free(&bw);
}

int main(void)
{
    os_init();
    net_set_timing(100, GIVE_UP_MS);
    host_and_friend();
    quiet_friend_stays();
    door();
    other_versions();
    handover();
    os_sleep_ms(300);       /* let the threads finish hanging up */
    if (failures)
    {
        fprintf(stderr, "test_collab_net: %d failed\n", failures);
        return 1;
    }
    printf("test_collab_net: ok\n");
    return 0;
}
