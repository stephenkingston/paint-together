/*
 * Paint (C port) - drawing together, and the app's clock (Rust: app.rs's
 * drawing-together part, and what its update loop did each frame)
 *
 * The app keeps a session (collab/session.h) while drawing together and a
 * link (collab/net.h) to the room. Finished edits go to the session as
 * they are recorded (app_record); friends' edits come back through
 * app_net_receive and are slid under whatever I'm in the middle of
 * (app_rebase). When the host leaves, the kids try each other in a fixed
 * order until one of them hosts and the rest have joined it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/app_internal.h"
#include "collab/names.h"

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(malloc(n), s, n);
}

/* ---------------------------------------------------------------- whole-picture undo marks */

int app_mark_get(const app *a, uint64_t id, uint64_t *foreign)
{
    for (int i = 0; i < a->n_marks; i++)
        if (a->marks[i].id == id)
        {
            *foreign = a->marks[i].foreign;
            return 1;
        }
    return 0;
}

void app_mark_set(app *a, uint64_t id, uint64_t foreign)
{
    for (int i = 0; i < a->n_marks; i++)
        if (a->marks[i].id == id)
        {
            a->marks[i].foreign = foreign;
            return;
        }
    a->marks = realloc(a->marks, (size_t)(a->n_marks + 1) * sizeof *a->marks);
    a->marks[a->n_marks++] = (replace_mark){ id, foreign };
}

static void marks_clear(app *a)
{
    free(a->marks);
    a->marks = NULL;
    a->n_marks = 0;
}

/* ---------------------------------------------------------------- undo while drawing together */

typedef struct
{
    app *a;
    uint64_t id;
    int back, too_late, whole;
    irect region;
} step_ctx;

static int step_one(const change *c, void *user)
{
    step_ctx *st = user;
    app *a = st->a;
    session *s = a->session;
    if (c->kind == CHANGE_REPLACE)
    {
        uint64_t foreign;
        if (!app_mark_get(a, st->id, &foreign) || foreign != s->foreign)
        {
            st->too_late = 1;
            return 0;
        }
        st->whole = 1;
    }
    edit e;
    if (collab_revert(c, s->base, st->back, &e))
    {
        region rg = edit_region(&e, a->img);
        if (!rg.whole)
            st->region = rg.r;
        edit_apply(&e, &a->img);
        session_commit(s, e);
    }
    return 1;
}

/* Undo or redo one of my own edits without touching what friends drew since:
 * a pixel changes back only while it still shows my edit */
void app_step_mine(app *a, int back)
{
    app_settle(a);
    if (!a->session)
        return;
    step_ctx st = { a, back ? history_state_id(a->history) : history_redo_id(a->history), back, 0, 0, IRECT_EMPTY };
    int stepped = history_step_with(a->history, back, step_one, &st);
    if (stepped && st.whole)
        app_mark_set(a, st.id, a->session->foreign);
    if (st.too_late)
        app_set_error(a, "Too late to undo that: your friends have drawn on the picture since.");
    if (st.whole)
        a->dirty_full = 1;
    else
        app_mark(a, st.region);
}

/* ---------------------------------------------------------------- notices */

static void push_toast(app *a, toast t)
{
    if (a->n_toasts == APP_MAX_TOASTS)
        memmove(a->toasts, a->toasts + 1, (APP_MAX_TOASTS - 1) * sizeof *a->toasts);
    else
        a->n_toasts++;
    a->toasts[a->n_toasts - 1] = t;
}

void app_toast(app *a, const char *text)
{
    toast t = { { 0 }, a->now + 3.5, 0, 0 };
    snprintf(t.text, sizeof t.text, "%s", text);
    push_toast(a, t);
}

/* "Blue Fox joined"; or, when several come (or go) close together, one notice that keeps count */
static void announce(app *a, int group, const char *one, int n)
{
    static const char *const verbs[] = { "", "joined", "left" };
    if (!n)
        return;
    for (int i = 0; i < a->n_toasts; i++)
    {
        toast *t = &a->toasts[i];
        if (t->until > a->now && t->group == group)
        {
            t->count += n;
            snprintf(t->text, sizeof t->text, "%d friends %s", t->count, verbs[group]);
            t->until = a->now + 3.5;
            return;
        }
    }
    toast t = { { 0 }, a->now + 3.5, group, n };
    if (n == 1)
        snprintf(t.text, sizeof t.text, "%s %s", one, verbs[group]);
    else
        snprintf(t.text, sizeof t.text, "%d friends %s", n, verbs[group]);
    push_toast(a, t);
}

static const kid *find_kid(const kid_list *l, peer_id id)
{
    for (int i = 0; i < l->n; i++)
        if (l->v[i].id == id)
            return &l->v[i];
    return NULL;
}

/* "Blue Fox joined" / "Blue Fox left", comparing with who was here before */
static void announce_kids(app *a, const kid_list *before)
{
    const session *s = a->session;
    const char *one = NULL;
    int n = 0;
    for (int i = 0; i < s->kids.n; i++)
        if (s->kids.v[i].id != s->me && !find_kid(before, s->kids.v[i].id))
            one = one ? one : s->kids.v[i].name, n++;
    announce(a, 1, one, n);
    one = NULL;
    n = 0;
    for (int i = 0; i < before->n; i++)
        if (before->v[i].id != s->me && !find_kid(&s->kids, before->v[i].id))
            one = one ? one : before->v[i].name, n++;
    announce(a, 2, one, n);
}

/* ---------------------------------------------------------------- the room */

int app_hosting(const app *a)
{
    return !a->handover.active && a->session && a->session->host == a->session->me;
}

const kid *app_find_kid(const app *a, peer_id id)
{
    return a->session ? find_kid(&a->session->kids, id) : NULL;
}

int app_together_status(const app *a, char *buf, size_t cap)
{
    if (!a->session)
        return 0;
    int n = a->session->kids.n - 1;
    if (n <= 0)
        snprintf(buf, cap, "Waiting for friends");
    else if (n == 1)
        snprintf(buf, cap, "Drawing with 1 friend");
    else
        snprintf(buf, cap, "Drawing with %d friends", n);
    return 1;
}

/* When hosting, tell the network about the drawing (and how many are in it) */
void app_advertise(app *a)
{
    if (!a->has_invite)
        return;
    int kids = a->session && a->session->kids.n > 1 ? a->session->kids.n : 1;
    if (!a->discovery && a->lan_discovery)
        a->discovery = discovery_new(host_wake);
    if (a->discovery)
        discovery_advertise(a->discovery, a->me_name, kids, a->invite.port);
}

void app_discover(app *a)
{
    if (!a->discovery && a->lan_discovery)
        a->discovery = discovery_new(host_wake);
    if (a->discovery)
        discovery_browse(a->discovery);
}

const nearby *app_nearby(const app *a, int *n)
{
    *n = 0;
    return a->discovery ? discovery_nearby(a->discovery, n) : NULL;
}

void app_set_ask_first(app *a, int on)
{
    a->ask_first = on;
    app_save_me(a);
    if (app_hosting(a))
    {
        a->session->ask_first = on;
        to_hub m = { 0 };
        m.kind = TO_ASK_FIRST;
        m.yes = on;
        session_send(a->session, m);
    }
}

void app_kick(app *a, peer_id kid)
{
    if (!app_hosting(a))
        return;
    to_hub m = { 0 };
    m.kind = TO_KICK;
    m.kid = kid;
    session_send(a->session, m);
}

void app_let_in(app *a, const peer_id *kids, int n, int yes)
{
    session *s = a->session;
    if (!s)
        return;
    for (int i = 0; i < n; i++)
    {
        to_hub m = { 0 };
        m.kind = TO_LET_IN;
        m.kid = kids[i];
        m.yes = yes;
        session_send(s, m);
    }
    int k = 0;
    for (int i = 0; i < s->waiting.n; i++)
    {
        int answered = 0;
        for (int j = 0; j < n; j++)
            answered |= s->waiting.v[i].id == kids[j];
        if (answered)
            kid_free(&s->waiting.v[i]);
        else
            s->waiting.v[k++] = s->waiting.v[i];
    }
    s->waiting.n = k;
}

void app_set_me_name(app *a, const char *name)
{
    names_tidy(name, a->me_name, sizeof a->me_name);
    app_save_me(a);
}

void app_set_me_color(app *a, const uint8_t c[3])
{
    memcpy(a->me_color, c, 3);
    app_save_me(a);
}

void app_open_join_dialog(app *a)
{
    free(a->join_dialog);
    a->join_dialog = dup_str("");
    app_discover(a);
}

void app_close_join_dialog(app *a)
{
    free(a->join_dialog);
    a->join_dialog = NULL;
}

static void set_invite(app *a)
{
    a->has_invite = link_hosting_at(a->link, &a->invite) == 0;
}

void app_start_together(app *a)
{
    app_settle(a);
    char err[256] = "";
    link *l = link_host(img_dup(a->img), a->me_name, a->me_color, a->ask_first, host_wake, err, sizeof err);
    if (!l)
    {
        app_set_error(a, "Paint couldn't start drawing together.\n\n%s", err);
        return;
    }
    a->link = l;
    set_invite(a);
    app_advertise(a);
}

void app_join_together(app *a, const char *addr)
{
    app_settle(a);
    char err[256] = "";
    link *l = link_join(addr, a->me_name, a->me_color, OS_BAD_SOCK, host_wake, err, sizeof err);
    if (!l)
    {
        app_set_error(a, "Paint couldn't join that drawing.\n\n%s", err);
        return;
    }
    link_free(a->link);
    a->link = l;
    a->has_invite = 0;
}

void app_handover_clear(app *a)
{
    kid_list_free(&a->handover.candidates);
    if (a->handover.spare != OS_BAD_SOCK)
        os_sock_close(a->handover.spare);
    a->handover.spare = OS_BAD_SOCK;
    a->handover.active = 0;
}

/* Stop drawing together; the picture stays, as an ordinary unsaved one */
void app_leave_together(app *a)
{
    app_settle(a);
    app_flush_network(a);
    int dirty = app_is_dirty(a);
    link_free(a->link);
    a->link = NULL;
    a->has_invite = 0;
    app_handover_clear(a);
    free(a->asking);
    a->asking = NULL;
    if (a->discovery)
        discovery_stop_advertising(a->discovery);
    if (a->session)
    {
        session_free(a->session);
        a->session = NULL;
        history_clear(a->history);
        marks_clear(a);
        a->saved_state = dirty ? UINT64_MAX : history_state_id(a->history);
    }
}

/* Drop the first candidate */
static void next_candidate(handover *h)
{
    if (!h->candidates.n)
        return;
    kid_free(&h->candidates.v[0]);
    memmove(h->candidates.v, h->candidates.v + 1, (size_t)(--h->candidates.n) * sizeof *h->candidates.v);
}

static int by_takeover_order(const void *x, const void *y, peer_id host)
{
    const kid *p = x, *q = y;
    int ph = p->id != host, qh = q->id != host;
    return ph != qh ? ph - qh : (p->id > q->id) - (p->id < q->id);
}

/* The connection to the room dropped (or a takeover attempt failed) */
static void lost_host(app *a)
{
    os_sock spare = OS_BAD_SOCK;
    if (a->link)
    {
        spare = link_take_spare(a->link);
        link_free(a->link);
        a->link = NULL;
    }
    handover *h = &a->handover;
    if (h->active)
    {
        next_candidate(h);
        if (spare != OS_BAD_SOCK)
        {
            if (h->spare != OS_BAD_SOCK)
                os_sock_close(h->spare);
            h->spare = spare;
        }
        return;
    }
    if (!a->session)
    {
        if (spare != OS_BAD_SOCK)
            os_sock_close(spare);
        return;
    }
    /* The old host first (in case only my connection dropped), then everyone else, earliest to join first */
    const session *s = a->session;
    h->candidates = kid_list_clone(&s->kids);
    for (int i = 1; i < h->candidates.n; i++)
        for (int j = i; j > 0 && by_takeover_order(&h->candidates.v[j - 1], &h->candidates.v[j], s->host) > 0; j--)
        {
            kid t = h->candidates.v[j];
            h->candidates.v[j] = h->candidates.v[j - 1];
            h->candidates.v[j - 1] = t;
        }
    h->spare = spare;
    h->active = 1;
    app_toast(a, "The host left: carrying on\xE2\x80\xA6");
}

/* Try the next kid in line for host, or become it */
static void step_handover(app *a)
{
    handover *h = &a->handover;
    while (!a->link && h->active && a->session)
    {
        const session *s = a->session;
        if (!h->candidates.n)
        {
            app_leave_together(a);
            app_set_error(a, "The drawing ended: the host left.");
            return;
        }
        const kid *next = &h->candidates.v[0];
        if (next->id == s->me)
        {
            os_sock listener = h->spare != OS_BAD_SOCK ? h->spare : net_listen();
            h->spare = OS_BAD_SOCK;
            if (listener == OS_BAD_SOCK)
            {
                app_leave_together(a);
                app_set_error(a, "Paint couldn't carry on the drawing.\n\nNo network port was free for friends to connect to.");
                return;
            }
            /* The room's rules carry on, and everyone who was in it comes straight back */
            hub *hb = hub_new(img_dup(session_confirmed(s)));
            hub_set_door(hb, s->ask_first, kid_list_clone(&s->kids));
            a->link = link_host_with(listener, hb, a->me_name, a->me_color, host_wake);
            set_invite(a);
        }
        else if (next->id == s->host && !*next->addr)
            next_candidate(h);
        else
        {
            char err[256];
            os_sock spare = h->spare;
            h->spare = OS_BAD_SOCK;
            a->link = link_join(next->addr, a->me_name, a->me_color, spare, host_wake, err, sizeof err);
            if (!a->link)
                next_candidate(h);
        }
    }
}

typedef struct
{
    const img *base;
    img *under;
    irect r;
    rgba color;
} refill_ctx;

static void refill(int x, int y, const uint8_t px[4], void *user)
{
    refill_ctx *c = user;
    if (irect_contains(c->r, (float)x, (float)y) && !memcmp(img_at(c->base, x, y), px, 4))
        memcpy(img_at(c->under, x, y), &c->color, 4);
}

/* A friend's edit changed `base` in the region: put it under whatever I'm in
 * the middle of, then draw that on top again */
void app_rebase(app *a, region rg)
{
    session *s = a->session;
    if (!s)
        return;
    const img *base = s->base;
    if (rg.whole || a->img->w != base->w || a->img->h != base->h)
    {
        /* Only reached with nothing in progress (see app_net_receive) */
        img *copy = img_dup(base);
        if (copy)
        {
            img_free(a->img);
            a->img = copy;
        }
        a->dirty_full = 1;
        return;
    }
    irect r = rg.r;
    if (a->op.kind == OP_STROKE)
        raster_copy_rect(a->op.snapshot, base, r);
    if (a->live)
        live_shape_rebase(a->live, base, r);
    if (a->text)
    {
        raster_copy_rect(a->text->snapshot, base, r);
        a->text->rendered = 0;
    }
    floating *f = a->sel ? a->sel->fl : NULL;
    if (f)
    {
        if (f->before)
            raster_copy_rect(f->before, base, r);
        else if (f->lifted)
        {
            /* `before` is `base` outside the lifted area, so only that part needs it */
            irect in = irect_intersect(r, f->origin);
            for (int y = in.y0; y < in.y1; y++)
                if (in.x1 > in.x0)
                    memcpy(img_at(f->lifted, in.x0 - f->origin.x0, y - f->origin.y0), img_at(base, in.x0, y), (size_t)(in.x1 - in.x0) * 4);
        }
        raster_copy_rect(f->base, base, r);
        /* The hole stays Color 2, except where a friend has drawn into it since */
        if (f->has_hole)
        {
            refill_ctx ctx = { base, f->base, r, f->hole_color };
            patch_each(&f->hole, refill, &ctx);
        }
        raster_copy_rect(a->img, f->base, r);
    }
    else
        raster_copy_rect(a->img, base, r);
    if (a->op.kind == OP_STROKE)
        raster_blend_mask(a->img, a->op.mask, a->op.color, a->op.opacity, irect_intersect(r, a->op.dirty));
    app_mark(a, r);
    app_render_live_shape(a);
    app_update_text_preview(a);
    if (f)
        app_refresh_float(a);
}

/* ---------------------------------------------------------------- messages from the room */

static void welcome(app *a, from_hub *msg)
{
    img *im = picture_to_image(&msg->picture);
    if (!im)
        return;
    free(a->asking);
    a->asking = NULL;
    kid_list kids = msg->kids;
    msg->kids = (kid_list){ 0 };
    int was_handover = a->handover.active;
    app_handover_clear(a);
    if (was_handover && a->session)
    {
        session *s = a->session;
        session_rejoin(s, msg->you, msg->host, msg->seq, im, kids);
        s->ask_first = msg->yes;
        img *copy = img_dup(s->base);
        if (copy)
        {
            img_free(a->img);
            a->img = copy;
        }
        a->dirty_full = 1;
        char news[160];
        const kid *k = find_kid(&s->kids, msg->host);
        if (msg->host == msg->you)
            snprintf(news, sizeof news, "You're the host now");
        else if (k)
            snprintf(news, sizeof news, "%s is the host now", k->name);
        else
            snprintf(news, sizeof news, "Drawing together again");
        app_toast(a, news);
        app_advertise(a);
        return;
    }
    app_settle(a);
    app_selection_free(a->sel);
    a->sel = NULL;
    history_clear(a->history);
    marks_clear(a);
    session_free(a->session);
    a->session = session_new(msg->you, msg->host, msg->seq, img_dup(im), kids);
    a->session->ask_first = msg->yes;
    a->saved_version = 0;
    img_free(a->img);
    a->img = im;
    a->dirty_full = 1;
    a->fit_pending = 1;
    a->scroll_x = a->scroll_y = 0.0f;
}

static const char *name_in(const kid_list *l, peer_id id)
{
    const kid *k = find_kid(l, id);
    return k ? k->name : "A friend";
}

void app_net_receive(app *a, from_hub *msg)
{
    switch (msg->kind)
    {
    case FROM_WELCOME:
        welcome(a, msg);
        from_hub_free(msg);
        return;
    case FROM_ASKING:
        free(a->asking);
        a->asking = dup_str(msg->text ? msg->text : "");
        from_hub_free(msg);
        return;
    case FROM_REJECTED:
        /* Not let in (or taking over as host didn't work out: try the next kid) */
        if (a->handover.active)
            lost_host(a);
        else
        {
            app_leave_together(a);
            app_set_error(a, "%s", msg->text ? msg->text : "");
        }
        from_hub_free(msg);
        return;
    }
    session *s = a->session;
    if (!s)
    {
        from_hub_free(msg);
        return;
    }
    kid_list before = kid_list_clone(&s->kids);
    if (msg->kind == FROM_KICKED_OUT)
    {
        if (msg->kid == s->me)
        {
            char host[100];
            snprintf(host, sizeof host, "%s", name_in(&before, msg->by));
            kid_list_free(&before);
            from_hub_free(msg);
            app_leave_together(a);
            app_set_error(a, "You have been kicked out by %s.\n\nThe picture stays with you.", host);
            return;
        }
        char text[160];
        snprintf(text, sizeof text, "%s has been kicked out by %s", name_in(&before, msg->kid), msg->by == s->me ? "you" : name_in(&before, msg->by));
        app_toast(a, text);
        /* Not "... left" as well */
        int k = 0;
        for (int i = 0; i < before.n; i++)
            if (before.v[i].id == msg->kid)
                kid_free(&before.v[i]);
            else
                before.v[k++] = before.v[i];
        before.n = k;
    }
    /* Whole-picture changes (and anything arriving while my canvas is a
     * different size, mid-resize) land after I finish what I'm doing */
    int foreign = msg->kind == FROM_SEQUENCED && msg->from != s->me;
    if (session_replaces_picture(s, msg) || (foreign && (a->img->w != s->base->w || a->img->h != s->base->h)))
        app_settle(a);
    received got = session_receive(a->session, msg);
    if (got.kind == RECEIVED_PICTURE)
        app_rebase(a, got.region);
    else if (got.kind == RECEIVED_KIDS)
    {
        announce_kids(a, &before);
        app_advertise(a);
    }
    kid_list_free(&before);
}

void app_pump_network(app *a)
{
    if (a->discovery)
        discovery_poll(a->discovery);
    if (a->session)
        a->session->now = a->now;
    link_event ev;
    while (a->link && link_recv(a->link, &ev))
    {
        if (ev.kind == LINK_MESSAGE)
            app_net_receive(a, &ev.msg);
        else if (!a->session)
        {
            /* Couldn't join in the first place */
            app_leave_together(a);
            if (ev.why && *ev.why)
                app_set_error(a, "%s", ev.why);
        }
        else
            lost_host(a);
        link_event_free(&ev);
    }
    step_handover(a);
}

to_hub *app_net_outbox(app *a, double now, int *n)
{
    *n = 0;
    session *s = a->session;
    if (!s)
        return NULL;
    to_hub *out = session_take_outbox(s, n);
    if (now - a->last_share < 1.0 / 30.0)
        return out;
    a->last_share = now;
    irect changed = a->dirty_full ? irect_of(a->img) : irect_union(a->preview_dirty, a->dirty);
    a->preview_dirty = IRECT_EMPTY;
    out = realloc(out, (size_t)(*n + 2) * sizeof *out);
    tile_list tiles;
    if (session_preview_tiles(s, a->img, changed, &tiles))
    {
        memset(&out[*n], 0, sizeof *out);
        out[*n].kind = TO_PREVIEW;
        out[*n].tiles = tiles;
        (*n)++;
    }
    int held = HELD_OTHER;
    switch (a->tool)
    {
    case TOOL_PENCIL:
        held = HELD_PENCIL;
        break;
    case TOOL_FILL:
        held = HELD_FILL;
        break;
    case TOOL_BRUSH:
        held = HELD_BRUSH;
        break;
    case TOOL_ERASER:
        held = HELD_ERASER;
        break;
    case TOOL_PICKER:
        held = HELD_PICKER;
        break;
    default:
        break;
    }
    rgba c = held == HELD_ERASER ? a->secondary : a->primary;
    presence p = { a->has_pointer, a->has_pointer ? a->pointer.x : 0.0f, a->has_pointer ? a->pointer.y : 0.0f, held,
                   { c.r, c.g, c.b, c.a }, held == HELD_ERASER ? a->size_eraser : 0.0f };
    if (session_presence(s, p, &out[*n]))
        (*n)++;
    return out;
}

void app_flush_network(app *a)
{
    if (a->handover.active)
        return;
    int n;
    to_hub *out = app_net_outbox(a, a->now, &n);
    for (int i = 0; i < n; i++)
        if (a->link)
            link_send(a->link, out[i]);
        else
            to_hub_free(&out[i]);
    free(out);
}

/* ---------------------------------------------------------------- the clock */

int app_tick(app *a, double now)
{
    a->now = now;
    int changed = 0;
    app_pump_network(a);
    if (a->op.kind == OP_STROKE && a->op.pen == PEN_BRUSH && a->op.brush == BRUSH_AIRBRUSH)
    {
        float dt = (float)(now - a->last_spray_);
        a->last_spray_ = now;
        app_spray_frame(a, dt < 0.0f ? 0.0f : dt > 0.1f ? 0.1f : dt);
        changed = 1;
    }
    app_render_live_shape(a);
    app_update_text_preview(a);
    int k = 0;
    for (int i = 0; i < a->n_toasts; i++)
        if (a->toasts[i].until > now)
            a->toasts[k++] = a->toasts[i];
        else
            changed = 1;
    a->n_toasts = k;
    app_flush_network(a);
    return changed || app_needs_repaint(a);
}

int app_needs_repaint(const app *a)
{
    return a->dirty_full || !irect_empty(a->dirty) || a->op.kind != OP_IDLE
        || (a->session && !irect_empty(a->session->overlay_dirty));
}

static int sooner(int ms, double secs)
{
    int t = secs <= 0.0 ? 0 : (int)ceil(secs * 1000.0);
    return ms < 0 || t < ms ? t : ms;
}

int app_wake_ms(const app *a)
{
    int ms = -1;
    if (a->op.kind == OP_STROKE && a->op.pen == PEN_BRUSH && a->op.brush == BRUSH_AIRBRUSH)
        ms = 16;
    if (a->text && a->text->editing)
    {
        /* The caret blinks: on for 0.53 s of every 1.06 */
        double phase = fmod(a->now - a->text->blink, 1.06);
        ms = sooner(ms, (phase < 0.53 ? 0.53 : 1.06) - phase);
    }
    if (a->n_toasts)
        ms = sooner(ms, 0.05);
    if (a->session)
    {
        /* My drawing in progress waits for the next share (30 a second) */
        if (!irect_empty(a->preview_dirty) || !irect_empty(a->dirty) || a->dirty_full)
            ms = sooner(ms, a->last_share + 1.0 / 30.0 - a->now);
        for (int i = 0; i < a->session->n_pointers; i++)
            if (a->now - a->session->pointers[i].at <= 10.0)
                ms = sooner(ms, 1.0);
    }
    else if (a->discovery)
        ms = sooner(ms, 0.7);
    if (a->handover.active)
        ms = sooner(ms, 0.1);
    return ms;
}
