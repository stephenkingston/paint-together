/*
 * Paint (C port) - drawing together, app by app: several kids in one room,
 * with every message in flight held in queues so a test decides exactly when
 * each one arrives, plus a real handover over sockets on this computer
 * (Rust: app.rs's mod tests::together)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/app.h"
#include "collab/hub.h"
#include "paint/raster.h"

static int failures;
#define CHECK(cond, ...)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            printf("FAIL %s:%d: %s: ", __func__, __LINE__, #cond);          \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static const rgba WHITE = { 255, 255, 255, 255 };
static const rgba RED = { 237, 28, 36, 255 }, BLUE = { 0, 0, 255, 255 };
#define EQ(c, want) rgba_eq(c, want)

static app *mk(void)
{
    app *a = app_new();
    img_free(a->img);
    a->img = img_new(200, 120, WHITE);
    a->view = (rect){ 0, 0, 800, 600 };
    return a;
}

static rgba px(const app *a, int x, int y)
{
    rgba c;
    memcpy(&c, img_at(a->img, x, y), 4);
    return c;
}

static rgba px_of(const img *im, int x, int y)
{
    rgba c;
    memcpy(&c, img_at(im, x, y), 4);
    return c;
}

static int same(const img *x, const img *y)
{
    return x->w == y->w && x->h == y->h && !memcmp(x->px, y->px, (size_t)x->w * x->h * 4);
}

static void drag(app *a, const pt *pts, int n)
{
    app_begin(a, pts[0], BTN_LEFT, pt_make(0, 0));
    app_update(a, pts + 1, n - 1, 0, 1.0f / 60.0f);
    app_end(a);
}

static void click(app *a, float x, float y)
{
    pt p = pt_make(x, y);
    drag(a, &p, 1);
}

static void line(app *a, app_tool tool, rgba color, float x0, float y0, float x1, float y1)
{
    app_set_tool(a, tool);
    a->primary = color;
    pt pts[2] = { { x0, y0 }, { x1, y1 } };
    drag(a, pts, 2);
}

static void update_to(app *a, float x, float y)
{
    pt p = pt_make(x, y);
    app_update(a, &p, 1, 0, 1.0f / 60.0f);
}

/* ---- a room routed by hand ---- */

#define MAX_KIDS 8
#define QCAP 4096

typedef struct
{
    hub *h;
    int n;
    peer_id id[MAX_KIDS];
    app *kid[MAX_KIDS];
    to_hub *up[MAX_KIDS];       /* each kid's messages on their way to the hub... */
    int up_head[MAX_KIDS], up_n[MAX_KIDS];
    from_hub *down[MAX_KIDS];   /* ...and the hub's on their way to each kid */
    int down_head[MAX_KIDS], down_n[MAX_KIDS];
    /* Friends' edits that arrived mid-stroke, over a live shape, text box or
     * floating selection: the cases where rebasing matters */
    int busy[4];
    double clock;               /* advances with every send, so previews and pointers go out each time */
} room;

static void route(room *r, hub_out *o)
{
    for (int i = 0; i < o->n; i++)
        for (int k = 0; k < r->n; k++)
            if (hub_wants(r->h, o->v[i].to, o->v[i].id, r->id[k]))
            {
                if (r->down_n[k] == QCAP)
                {
                    printf("queue full\n");
                    exit(1);
                }
                r->down[k][(r->down_head[k] + r->down_n[k]++) % QCAP] = from_hub_clone(&o->v[i].msg);
            }
    hub_out_free(o);
}

/* Hand kid i the hub's next message for them */
static int kid_gets(room *r, int i)
{
    if (!r->down_n[i])
        return 0;
    from_hub msg = r->down[i][r->down_head[i]];
    r->down_head[i] = (r->down_head[i] + 1) % QCAP;
    r->down_n[i]--;
    app *a = r->kid[i];
    if (msg.kind == FROM_SEQUENCED && msg.from != r->id[i])
    {
        r->busy[0] += a->op.kind == OP_STROKE;
        r->busy[1] += a->live != NULL;
        r->busy[2] += a->text != NULL;
        r->busy[3] += a->sel && a->sel->fl;
    }
    app_net_receive(a, &msg);
    return 1;
}

/* Hand the hub kid i's next message */
static int hub_takes(room *r, int i)
{
    r->clock += 0.05;
    int n;
    to_hub *out = app_net_outbox(r->kid[i], r->clock, &n);
    for (int k = 0; k < n; k++)
        r->up[i][(r->up_head[i] + r->up_n[i]++) % QCAP] = out[k];
    free(out);
    if (!r->up_n[i])
        return 0;
    to_hub msg = r->up[i][r->up_head[i]];
    r->up_head[i] = (r->up_head[i] + 1) % QCAP;
    r->up_n[i]--;
    hub_out o = { 0 };
    hub_handle(r->h, r->id[i], &msg, &o);
    route(r, &o);
    return 1;
}

/* A new kid arrives (each from their own computer), and comes in or waits to be let in; returns their index */
static int room_join(room *r)
{
    int i = r->n++;
    r->id[i] = (peer_id)i + 1;
    r->kid[i] = mk();
    r->up[i] = calloc(QCAP, sizeof(to_hub));
    r->down[i] = calloc(QCAP, sizeof(from_hub));
    char name[16], addr[32];
    snprintf(name, sizeof name, "Kid %d", i + 1);
    snprintf(addr, sizeof addr, "10.0.0.%d:47100", i + 1);
    static const uint8_t color[3] = { 200, 50, 50 };
    hub_out o = { 0 };
    hub_join(r->h, r->id[i], name, color, addr, &o);
    route(r, &o);
    kid_gets(r, i);
    return i;
}

static room *room_new(int n)
{
    room *r = calloc(1, sizeof *r);
    r->h = hub_new(img_new(200, 120, WHITE));
    for (int i = 0; i < n; i++)
        room_join(r);
    return r;
}

static void room_free(room *r)
{
    for (int i = 0; i < r->n; i++)
    {
        while (r->up_n[i])
        {
            to_hub_free(&r->up[i][r->up_head[i]]);
            r->up_head[i] = (r->up_head[i] + 1) % QCAP;
            r->up_n[i]--;
        }
        while (r->down_n[i])
        {
            from_hub_free(&r->down[i][r->down_head[i]]);
            r->down_head[i] = (r->down_head[i] + 1) % QCAP;
            r->down_n[i]--;
        }
        free(r->up[i]);
        free(r->down[i]);
        app_free(r->kid[i]);
    }
    hub_free(r->h);
    free(r);
}

/* Deliver everything in flight */
static void quiesce(room *r)
{
    for (int moved = 1; moved;)
    {
        moved = 0;
        for (int i = 0; i < r->n; i++)
        {
            while (hub_takes(r, i))
                moved = 1;
            while (kid_gets(r, i))
                moved = 1;
        }
    }
}

/* Everyone finishes what they're doing; then every screen in the room must match the hub's picture exactly */
static void converged(room *r, const char *where)
{
    for (int i = 0; i < r->n; i++)
        app_settle(r->kid[i]);
    quiesce(r);
    const img *truth = hub_picture(r->h);
    for (int i = 0; i < r->n; i++)
    {
        if (!hub_is_in(r->h, r->id[i]))
            continue;
        const app *a = r->kid[i];
        const session *s = a->session;
        if (!s)
        {
            CHECK(0, "%s: kid %d has no session", where, i);
            continue;
        }
        CHECK(session_unconfirmed(s) == 0, "%s: kid %d still waiting on the hub", where, i);
        CHECK(same(session_confirmed(s), truth), "%s: kid %d: confirmed picture differs from the hub's", where, i);
        CHECK(same(s->base, truth), "%s: kid %d: base differs from the hub's", where, i);
        CHECK(same(a->img, truth), "%s: kid %d: screen differs from the hub's", where, i);
        CHECK(!session_has_previews(s), "%s: kid %d still shows a friend's unfinished drawing", where, i);
    }
}

static void test_joining_gets_the_hosts_picture(void)
{
    room *r = room_new(1);
    line(r->kid[0], TOOL_PENCIL, RED, 10, 10, 100, 10);
    quiesce(r);
    room_join(r);
    CHECK(EQ(px(r->kid[1], 50, 10), RED), "the newcomer sees it");
    converged(r, __func__);
    room_free(r);
}

static void test_a_friends_stroke_goes_under_mine(void)
{
    room *r = room_new(2);
    /* Kid 0 is halfway through a red line when kid 1's blue one arrives */
    app *k0 = r->kid[0];
    app_set_tool(k0, TOOL_PENCIL);
    k0->primary = RED;
    k0->size_pencil = 5.0f;
    app_begin(k0, pt_make(10, 50), BTN_LEFT, pt_make(0, 0));
    update_to(k0, 150, 50);
    r->kid[1]->size_pencil = 5.0f;
    line(r->kid[1], TOOL_PENCIL, BLUE, 80, 10, 80, 110);
    quiesce(r);
    CHECK(EQ(px(k0, 80, 20), BLUE), "the friend's line shows up");
    CHECK(EQ(px(k0, 80, 50), RED), "my unfinished line stays on top");
    update_to(k0, 160, 50);
    app_end(k0);
    converged(r, __func__);
    CHECK(EQ(px(r->kid[1], 80, 50), RED), "mine last");
    CHECK(EQ(px(r->kid[1], 80, 20), BLUE), "theirs under");
    room_free(r);
}

static void test_edits_crossing_end_up_identical(void)
{
    room *r = room_new(3);
    /* Everyone draws before seeing anyone else's work */
    r->kid[0]->size_pencil = 7.0f;
    r->kid[1]->size_pencil = 7.0f;
    line(r->kid[0], TOOL_PENCIL, RED, 10, 60, 190, 60);
    line(r->kid[1], TOOL_PENCIL, BLUE, 100, 5, 100, 115);
    app_set_tool(r->kid[2], TOOL_FILL);
    r->kid[2]->primary = (rgba){ 0, 160, 0, 255 };
    click(r->kid[2], 5, 5);
    /* Kid 2's fill reaches the hub first, then kid 1's line, then kid 0's */
    hub_takes(r, 2);
    hub_takes(r, 1);
    hub_takes(r, 0);
    converged(r, __func__);
    CHECK(EQ(px(r->kid[0], 100, 60), RED), "the last edit in the hub's order wins");
    room_free(r);
}

static void test_undo_takes_back_only_mine(void)
{
    room *r = room_new(2);
    r->kid[0]->size_pencil = 5.0f;
    r->kid[1]->size_pencil = 5.0f;
    line(r->kid[0], TOOL_PENCIL, RED, 10, 50, 150, 50);
    quiesce(r);
    line(r->kid[1], TOOL_PENCIL, BLUE, 80, 10, 80, 110);
    quiesce(r);
    app_undo(r->kid[0]);
    converged(r, __func__);
    for (int i = 0; i < 2; i++)
    {
        CHECK(EQ(px(r->kid[i], 40, 50), WHITE), "my line is gone");
        CHECK(EQ(px(r->kid[i], 80, 50), BLUE), "where a friend drew over it, theirs stays");
    }
    app_redo(r->kid[0]);
    converged(r, __func__);
    CHECK(EQ(px(r->kid[1], 40, 50), RED), "back");
    CHECK(EQ(px(r->kid[1], 80, 50), BLUE), "theirs stays on top");
    room_free(r);
}

static void test_whole_picture_undo_until_friends_draw(void)
{
    room *r = room_new(2);
    line(r->kid[0], TOOL_PENCIL, RED, 10, 10, 60, 10);
    app_rotate(r->kid[0], ROTATE_180);
    quiesce(r);
    app_undo(r->kid[0]);
    converged(r, __func__);
    CHECK(EQ(px(r->kid[1], 30, 10), RED), "undone before anyone drew");
    app_redo(r->kid[0]);
    quiesce(r);
    line(r->kid[1], TOOL_PENCIL, BLUE, 10, 60, 60, 60);
    quiesce(r);
    app_undo(r->kid[0]);
    CHECK(r->kid[0]->error != NULL, "too late");
    app_dismiss_error(r->kid[0]);
    converged(r, __func__);
    CHECK(EQ(px(r->kid[1], 30, 60), BLUE), "theirs stays");
    room_free(r);
}

static void test_a_friend_rotating_finishes_my_stroke(void)
{
    room *r = room_new(2);
    app_set_tool(r->kid[0], TOOL_PENCIL);
    app_begin(r->kid[0], pt_make(10, 10), BTN_LEFT, pt_make(0, 0));
    update_to(r->kid[0], 50, 10);
    app_rotate(r->kid[1], ROTATE_RIGHT);
    quiesce(r);
    CHECK(r->kid[0]->op.kind == OP_IDLE, "my stroke was finished");
    CHECK(r->kid[0]->img->w == 120 && r->kid[0]->img->h == 200, "turned");
    converged(r, __func__);
    room_free(r);
}

static void test_a_friend_drawing_under_my_text(void)
{
    room *r = room_new(2);
    app *k0 = r->kid[0];
    app_set_tool(k0, TOOL_TEXT);
    k0->text_style.size_pt = 24.0f;
    click(k0, 20, 30);
    app_text(k0, "Hello");
    frect b = k0->text->bounds;
    float y = (b.y0 + b.y1) / 2;
    r->kid[1]->size_pencil = 3.0f;
    line(r->kid[1], TOOL_PENCIL, BLUE, 0, y, 199, y);
    quiesce(r);
    CHECK(k0->text && k0->text->editing, "still typing");
    CHECK(EQ(px(k0, 5, (int)y), BLUE), "the friend's line runs through the text box");
    converged(r, __func__);
    room_free(r);
}

static void test_moving_a_selection_while_a_friend_draws_under_it(void)
{
    room *r = room_new(2);
    app *k0 = r->kid[0];
    k0->size_pencil = 8.0f;
    line(k0, TOOL_PENCIL, RED, 10, 20, 60, 20);
    quiesce(r);
    /* Pick the red bar up and start dragging it down */
    app_set_tool(k0, TOOL_SELECT);
    pt box[2] = { { 5, 10 }, { 66, 31 } };
    drag(k0, box, 2);
    app_begin(k0, pt_make(30, 20), BTN_LEFT, pt_make(0, 0));
    update_to(k0, 30, 80);
    /* A friend's diagonal line crosses where the bar was */
    r->kid[1]->size_pencil = 3.0f;
    line(r->kid[1], TOOL_PENCIL, BLUE, 20, 0, 60, 119);
    quiesce(r);
    int crossing = (int)(20.0f + 40.0f * 20.0f / 119.0f);
    CHECK(EQ(px(k0, 50, 20), WHITE), "the hole stays empty, with no ghost of the bar");
    CHECK(EQ(px(k0, crossing, 20), BLUE), "the friend's line through the hole stays");
    app_end(k0);
    converged(r, __func__);
    for (int i = 0; i < 2; i++)
    {
        CHECK(EQ(px(r->kid[i], 50, 20), WHITE), "kid %d: hole", i);
        CHECK(EQ(px(r->kid[i], crossing, 20), BLUE), "kid %d: crossing", i);
        CHECK(EQ(px(r->kid[i], 45, 80), RED), "kid %d: the bar moved", i);
    }
    room_free(r);
}

static void test_friends_see_what_im_drawing(void)
{
    room *r = room_new(2);
    app *k0 = r->kid[0];
    app_set_tool(k0, TOOL_PENCIL);
    k0->primary = RED;
    k0->size_pencil = 5.0f;
    app_begin(k0, pt_make(10, 50), BTN_LEFT, pt_make(0, 0));
    update_to(k0, 150, 50);
    quiesce(r);
    const session *s = r->kid[1]->session;
    CHECK(session_has_previews(s) && s->overlay && EQ(px_of(s->overlay, 100, 50), RED), "shown over the picture");
    CHECK(app_friends_overlay(r->kid[1]) == s->overlay, "for the view");
    CHECK(EQ(px(r->kid[1], 100, 50), WHITE), "but not part of it yet");
    update_to(k0, 150, 100);
    quiesce(r);
    s = r->kid[1]->session;
    CHECK(s->overlay && EQ(px_of(s->overlay, 150, 90), RED), "follows along");
    app_end(k0);
    converged(r, __func__);
    CHECK(EQ(px(r->kid[1], 100, 50), RED), "finished");
    s = r->kid[1]->session;
    CHECK(!s->overlay || EQ(px_of(s->overlay, 100, 50), ((rgba){ 0, 0, 0, 0 })), "the preview is gone");
    room_free(r);
}

static void test_friends_drawing_merged_on_screen(void)
{
    img *im = img_new(4, 2, WHITE), *over = img_new(4, 2, (rgba){ 0, 0, 0, 0 });
    memcpy(img_at(over, 2, 1), &RED, 4);
    uint8_t shown[3 * 2 * 4];
    app_with_friends(im, over, irect_make(1, 0, 4, 2), shown);
    rgba c;
    memcpy(&c, shown + (1 * 3 + 1) * 4, 4);
    CHECK(EQ(c, RED), "the friend's pixel replaces mine");
    memcpy(&c, shown + (1 * 3 + 0) * 4, 4);
    CHECK(EQ(c, WHITE), "elsewhere the picture shows through");
    memcpy(&c, shown + (0 * 3 + 2) * 4, 4);
    CHECK(EQ(c, WHITE), "elsewhere the picture shows through");
    img_free(im);
    img_free(over);
}

static void test_a_text_box_thrown_away_disappears(void)
{
    room *r = room_new(2);
    app_set_tool(r->kid[0], TOOL_TEXT);
    click(r->kid[0], 20, 30);
    app_text(r->kid[0], "Hi");
    quiesce(r);
    CHECK(session_has_previews(r->kid[1]->session), "friends see it");
    app_discard_text(r->kid[0]);
    quiesce(r);
    CHECK(!session_has_previews(r->kid[1]->session), "gone for them too");
    converged(r, __func__);
    room_free(r);
}

static int has_toast(const app *a, const char *text)
{
    for (int i = 0; i < a->n_toasts; i++)
        if (!strcmp(a->toasts[i].text, text))
            return 1;
    return 0;
}

static void test_kids_coming_and_going(void)
{
    room *r = room_new(1);
    room_join(r);
    quiesce(r);
    CHECK(has_toast(r->kid[0], "Kid 2 joined"), "joined");
    hub_out o = { 0 };
    hub_leave(r->h, 2, &o);
    route(r, &o);
    quiesce(r);
    CHECK(has_toast(r->kid[0], "Kid 2 left"), "left");
    room_free(r);
}

static void test_a_crowd_is_one_notice(void)
{
    room *r = room_new(1);
    for (int i = 0; i < 5; i++)
        room_join(r);
    quiesce(r);
    CHECK(r->kid[0]->n_toasts == 1 && !strcmp(r->kid[0]->toasts[0].text, "5 friends joined"), "%d: %s", r->kid[0]->n_toasts,
          r->kid[0]->toasts[0].text);
    room_free(r);
}

static void test_kick(void)
{
    room *r = room_new(3);
    quiesce(r);
    for (int i = 0; i < 3; i++)
        r->kid[i]->n_toasts = 0;
    /* Only the host can */
    app_kick(r->kid[1], 3);
    quiesce(r);
    CHECK(r->kid[2]->session && r->kid[2]->session->kids.n == 3, "still in");

    line(r->kid[2], TOOL_PENCIL, RED, 10, 10, 60, 10);
    quiesce(r);
    /* Still on its way when the kick lands: it stays theirs alone */
    line(r->kid[2], TOOL_PENCIL, BLUE, 10, 20, 60, 20);
    app_kick(r->kid[0], 3);
    quiesce(r);
    app *out = r->kid[2];
    CHECK(out->session == NULL, "out of the room");
    CHECK(out->error && !strcmp(out->error, "You have been kicked out by Kid 1.\n\nThe picture stays with you."), "%s", out->error);
    CHECK(EQ(px(out, 30, 10), RED) && EQ(px(out, 30, 20), BLUE), "they keep the picture");
    CHECK(r->kid[1]->n_toasts == 1 && !strcmp(r->kid[1]->toasts[0].text, "Kid 3 has been kicked out by Kid 1"), "%s", r->kid[1]->toasts[0].text);
    CHECK(r->kid[0]->n_toasts == 1 && !strcmp(r->kid[0]->toasts[0].text, "Kid 3 has been kicked out by you"), "%s", r->kid[0]->toasts[0].text);
    CHECK(r->kid[1]->session->kids.n == 2, "two left");
    converged(r, __func__);
    CHECK(EQ(px(r->kid[1], 30, 10), RED) && EQ(px(r->kid[1], 30, 20), WHITE), "only the edit that arrived in time");
    room_free(r);
}

static void test_the_door(void)
{
    room *r = room_new(2);
    app_set_ask_first(r->kid[0], 1);
    line(r->kid[0], TOOL_PENCIL, RED, 10, 10, 60, 10);
    quiesce(r);
    CHECK(r->kid[1]->session->ask_first, "everyone knows, for a new host to carry on");

    int maya = room_join(r);
    quiesce(r);
    CHECK(r->kid[maya]->session == NULL, "not in yet");
    CHECK(r->kid[maya]->asking && !strcmp(r->kid[maya]->asking, "Kid 1"), "waiting for Kid 1");
    const session *host = r->kid[0]->session;
    CHECK(host->waiting.n == 1 && host->waiting.v[0].id == 3, "the host is asked");
    CHECK(r->kid[1]->session->waiting.n == 0, "only the host is asked");
    /* Waiting, they see nothing of the drawing */
    line(r->kid[1], TOOL_PENCIL, BLUE, 10, 20, 60, 20);
    quiesce(r);
    CHECK(r->down_n[maya] == 0, "nothing sent to the door");

    peer_id three = 3, four = 4;
    app_let_in(r->kid[0], &three, 1, 1);
    quiesce(r);
    CHECK(!r->kid[maya]->asking, "let in");
    CHECK(EQ(px(r->kid[maya], 30, 20), BLUE), "and sees the drawing");
    CHECK(r->kid[0]->session->waiting.n == 0, "nobody waiting");
    converged(r, __func__);

    int sam = room_join(r);
    app_let_in(r->kid[0], &four, 1, 0);
    quiesce(r);
    CHECK(!r->kid[sam]->session && !r->kid[sam]->asking, "not let in");
    CHECK(r->kid[sam]->error && !strcmp(r->kid[sam]->error, "Kid 1 didn't let you in this time."), "%s", r->kid[sam]->error);
    CHECK(r->kid[1]->session->kids.n == 3, "three in the room");
    room_free(r);
}

static const pointer *pointer_of(const app *a, peer_id id)
{
    for (int i = 0; i < a->session->n_pointers; i++)
        if (a->session->pointers[i].id == id)
            return &a->session->pointers[i];
    return NULL;
}

static void test_friends_see_my_pointer(void)
{
    room *r = room_new(2);
    app_set_tool(r->kid[0], TOOL_FILL);
    r->kid[0]->has_pointer = 1;
    r->kid[0]->pointer = pt_make(42, 17);
    quiesce(r);
    const pointer *p = pointer_of(r->kid[1], r->id[0]);
    CHECK(p && p->p.has_at && p->p.x == 42 && p->p.y == 17 && p->p.tool == HELD_FILL, "where and what");
    /* The eraser goes with its size and Color 2; the colour picker by name */
    app_set_tool(r->kid[0], TOOL_ERASER);
    r->kid[0]->size_eraser = 8.0f;
    quiesce(r);
    p = pointer_of(r->kid[1], r->id[0]);
    CHECK(p && p->p.tool == HELD_ERASER && p->p.size == 8.0f && !memcmp(p->p.color, "\xFF\xFF\xFF\xFF", 4), "the eraser");
    app_set_tool(r->kid[0], TOOL_PICKER);
    quiesce(r);
    p = pointer_of(r->kid[1], r->id[0]);
    CHECK(p && p->p.tool == HELD_PICKER, "the picker");
    room_free(r);
}

/* ---- real sockets ---- */

typedef int (*until_fn)(app **k);

static int run_until(app **k, int n, const char *what, until_fn done)
{
    for (int round = 0; round < 1500; round++)
    {
        for (int i = 0; i < n; i++)
        {
            k[i]->now = os_now();
            app_pump_network(k[i]);
            app_flush_network(k[i]);
        }
        if (done(k))
            return 1;
        os_sleep_ms(10);
    }
    CHECK(0, "timed out waiting until %s", what);
    return 0;
}

static int host_in(app **k)
{
    return k[0]->session != NULL;
}
static int kid1_in(app **k)
{
    return k[1]->session != NULL;
}
static int kid2_in(app **k)
{
    return k[2]->session != NULL;
}
static int three_kids(app **k)
{
    for (int i = 0; i < 3; i++)
        if (!k[i]->session || k[i]->session->kids.n != 3)
            return 0;
    return 1;
}
static int red_everywhere(app **k)
{
    for (int i = 0; i < 3; i++)
        if (!EQ(px(k[i], 50, 20), RED))
            return 0;
    return 1;
}
static int taken_over(app **k)
{
    const session *a = k[1]->session, *b = k[2]->session;
    return a && b && a->host == a->me && b->host == a->me && b->kids.n == 2 && !k[2]->handover.active;
}
static int both_lines(app **k)
{
    for (int i = 1; i < 3; i++)
        if (!EQ(px(k[i], 50, 60), BLUE) || !EQ(px(k[i], 50, 90), RED))
            return 0;
    return 1;
}

static void test_handover(void)
{
    /* Real sockets on this computer: kid 0 hosts, kids 1 and 2 join */
    app *k[3] = { mk(), mk(), mk() };
    app_start_together(k[0]);
    CHECK(k[0]->link && k[0]->has_invite, "hosting");
    if (!k[0]->has_invite)
        goto done;
    char addr[32];
    snprintf(addr, sizeof addr, "127.0.0.1:%d", k[0]->invite.port);
    if (!run_until(k, 3, "the host is in", host_in))
        goto done;
    app_join_together(k[1], addr);
    if (!run_until(k, 3, "kid 1 is in", kid1_in))
        goto done;
    app_join_together(k[2], addr);
    if (!run_until(k, 3, "kid 2 is in", kid2_in) || !run_until(k, 3, "everyone sees three kids", three_kids))
        goto done;
    line(k[1], TOOL_PENCIL, RED, 10, 20, 100, 20);
    if (!run_until(k, 3, "kid 1's line reaches everyone", red_everywhere))
        goto done;

    /* The host leaves; kid 1 (the first to join) takes over and kid 2 follows */
    app_leave_together(k[0]);
    if (!run_until(k, 3, "kid 1 is the host and kid 2 is with them", taken_over))
        goto done;
    CHECK(k[1]->n_toasts && (!strcmp(k[1]->toasts[k[1]->n_toasts - 1].text, "You're the host now")
                             || !strcmp(k[1]->toasts[0].text, "You're the host now")), "announced");
    CHECK(EQ(px(k[2], 50, 20), RED), "the picture carried over");

    /* Drawing carries on, both ways */
    line(k[2], TOOL_PENCIL, BLUE, 10, 60, 100, 60);
    line(k[1], TOOL_PENCIL, RED, 10, 90, 100, 90);
    if (run_until(k, 3, "both lines reach both", both_lines))
        CHECK(same(k[1]->img, k[2]->img), "identical");
done:
    for (int i = 0; i < 3; i++)
        app_free(k[i]);
}

/* ---- random sessions ---- */

/* A tiny random generator, so each seed replays the same session */
static uint64_t roll(uint64_t *d, uint64_t n)
{
    *d ^= *d << 13;
    *d ^= *d >> 7;
    *d ^= *d << 17;
    return *d % n;
}

static pt dice_point(uint64_t *d, const app *a)
{
    float x = (float)roll(d, (uint64_t)a->img->w) + 0.5f;
    float y = (float)roll(d, (uint64_t)a->img->h) + 0.5f;
    return pt_make(x, y);
}

/* One random thing a kid might do */
static void act(app *a, uint64_t *d)
{
    if (a->op.kind != OP_IDLE)
    {
        if (roll(d, 3) == 0)
            app_end(a);
        else
        {
            pt p = dice_point(d, a);
            app_update(a, &p, 1, 0, 1.0f / 60.0f);
        }
        return;
    }
    const uint8_t *c = APP_PALETTE[roll(d, 20)];
    a->primary = (rgba){ c[0], c[1], c[2], 255 };
    pt p = dice_point(d, a);
    switch (roll(d, 14))
    {
    case 0:
    case 1:
    case 2:
        app_set_tool(a, TOOL_PENCIL);
        a->size_pencil = 1.0f + (float)roll(d, 8);
        app_begin(a, p, BTN_LEFT, pt_make(0, 0));
        break;
    case 3:
    case 4:
        app_set_tool(a, TOOL_BRUSH);
        a->brush = (brush_kind)roll(d, 9);
        app_begin(a, p, BTN_LEFT, pt_make(0, 0));
        break;
    case 5:
        app_set_tool(a, TOOL_FILL);
        drag(a, &p, 1);
        break;
    case 6:
        app_set_tool(a, TOOL_SHAPE);
        a->shape = (shape_kind)roll(d, 21);
        app_begin(a, p, BTN_LEFT, pt_make(0, 0));
        break;
    case 7:
        app_set_tool(a, TOOL_TEXT);
        drag(a, &p, 1);
        app_text(a, "Hi");
        break;
    case 8:
    {
        /* Select a box, then start dragging it somewhere else */
        app_set_tool(a, TOOL_SELECT);
        pt box[2] = { p, dice_point(d, a) };
        drag(a, box, 2);
        if (a->sel)
        {
            irect r = a->sel->rect;
            app_begin(a, pt_make((float)(r.x0 + r.x1) / 2.0f, (float)(r.y0 + r.y1) / 2.0f), BTN_LEFT, pt_make(0, 0));
        }
        break;
    }
    case 9:
        app_commit_all(a);
        break;
    case 10:
        app_undo(a);
        break;
    case 11:
        app_redo(a);
        break;
    case 12:
        switch (roll(d, 6))
        {
        case 0:
            app_rotate(a, ROTATE_RIGHT);
            break;
        case 1:
            app_rotate(a, ROTATE_180);
            break;
        case 2:
            app_clear_image(a);
            break;
        default:
            app_invert(a);
            break;
        }
        break;
    default:
        break;
    }
    app_dismiss_error(a);
}

static void test_random_sessions(void)
{
    int busy[4] = { 0 };
    for (uint64_t seed = 1; seed <= 24; seed++)
    {
        uint64_t d = (seed * 0x9E3779B97F4A7C15ull) | 1;
        room *r = room_new(3);
        for (int step = 0; step < 120; step++)
        {
            int i = (int)roll(&d, 3);
            switch (roll(&d, 5))
            {
            case 0:
            case 1:
                act(r->kid[i], &d);
                break;
            case 2:
                hub_takes(r, i);
                break;
            default:
                kid_gets(r, i);
                break;
            }
        }
        char where[32];
        snprintf(where, sizeof where, "seed %d", (int)seed);
        converged(r, where);
        for (int k = 0; k < 4; k++)
            busy[k] += r->busy[k];
        room_free(r);
    }
    /* Make sure the hard cases really happened */
    printf("   friends' edits arriving mid-stroke / over a live shape / text / floating selection: %d %d %d %d\n", busy[0], busy[1],
           busy[2], busy[3]);
    for (int k = 0; k < 4; k++)
        CHECK(busy[k] >= 10, "case %d happened %d times", k, busy[k]);
}

int main(void)
{
#ifndef _WIN32
    setenv("XDG_CONFIG_HOME", "/tmp/paint-c-test-together", 1);
#endif
    os_init();
    test_joining_gets_the_hosts_picture();
    test_a_friends_stroke_goes_under_mine();
    test_edits_crossing_end_up_identical();
    test_undo_takes_back_only_mine();
    test_whole_picture_undo_until_friends_draw();
    test_a_friend_rotating_finishes_my_stroke();
    test_a_friend_drawing_under_my_text();
    test_moving_a_selection_while_a_friend_draws_under_it();
    test_friends_see_what_im_drawing();
    test_friends_drawing_merged_on_screen();
    test_a_text_box_thrown_away_disappears();
    test_kids_coming_and_going();
    test_a_crowd_is_one_notice();
    test_kick();
    test_the_door();
    test_friends_see_my_pointer();
    test_random_sessions();
    test_handover();
    if (failures)
    {
        printf("test_together: %d failed\n", failures);
        return 1;
    }
    printf("test_together: ok\n");
    return 0;
}
