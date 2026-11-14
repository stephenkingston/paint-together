/*
 * Paint (C port) - drawing together: one app's view of the shared picture
 * (Rust: collab/mod.rs)
 */
#include "collab/session.h"

#include <stdlib.h>
#include <string.h>
#include "paint/raster.h"

/* ---- tiles by (tx, ty) ---- */
static uint32_t tile_key(uint16_t tx, uint16_t ty)
{
    return (uint32_t)tx << 16 | ty;
}

static int map_find(const tile_map *m, uint32_t key)
{
    if (!m->cap)
        return -1;
    for (int i = (int)((key * 2654435761u) & (uint32_t)(m->cap - 1));; i = (i + 1) & (m->cap - 1))
    {
        if (!m->v[i].used)
            return -1;
        if (m->v[i].key == key)
            return i;
    }
}

static const patch *map_get(const tile_map *m, uint32_t key)
{
    int i = map_find(m, key);
    return i >= 0 ? &m->v[i].p : NULL;
}

static void map_put(tile_map *m, uint32_t key, patch p);

static void map_grow(tile_map *m)
{
    tile_map old = *m;
    m->cap = old.cap ? old.cap * 2 : 16;
    m->n = 0;
    m->v = calloc((size_t)m->cap, sizeof(tile_slot));
    for (int i = 0; i < old.cap; i++)
        if (old.v[i].used)
            map_put(m, old.v[i].key, old.v[i].p);
    free(old.v);
}

/* Takes ownership of p, replacing (and freeing) what was there */
static void map_put(tile_map *m, uint32_t key, patch p)
{
    int i = map_find(m, key);
    if (i >= 0)
    {
        patch_free(&m->v[i].p);
        m->v[i].p = p;
        return;
    }
    if ((m->n + 1) * 4 > m->cap * 3)
        map_grow(m);
    for (i = (int)((key * 2654435761u) & (uint32_t)(m->cap - 1)); m->v[i].used; i = (i + 1) & (m->cap - 1))
        ;
    m->v[i].used = 1;
    m->v[i].key = key;
    m->v[i].p = p;
    m->n++;
}

static void map_remove(tile_map *m, uint32_t key)
{
    int i = map_find(m, key);
    if (i < 0)
        return;
    patch_free(&m->v[i].p);
    m->v[i].used = 0;
    m->n--;
    /* Move later entries of the run back, so lookups never stop at the gap */
    for (int j = (i + 1) & (m->cap - 1); m->v[j].used; j = (j + 1) & (m->cap - 1))
    {
        tile_slot s = m->v[j];
        m->v[j].used = 0;
        m->n--;
        map_put(m, s.key, s.p);
    }
}

static void map_clear(tile_map *m)
{
    for (int i = 0; i < m->cap; i++)
        if (m->v[i].used)
            patch_free(&m->v[i].p);
    free(m->v);
    memset(m, 0, sizeof *m);
}

/* ---- the session ---- */
static region rebuild(session *s, region g);
static void free_pending(session *s)
{
    for (int i = 0; i < s->n_pending_; i++)
        edit_free(&s->pending_[i].e);
    free(s->pending_);
    s->pending_ = NULL;
    s->n_pending_ = 0;
}

static void free_outbox(session *s)
{
    for (int i = 0; i < s->n_outbox_; i++)
        to_hub_free(&s->outbox_[i]);
    free(s->outbox_);
    s->outbox_ = NULL;
    s->n_outbox_ = 0;
}

static void push_outbox(session *s, to_hub m)
{
    s->outbox_ = realloc(s->outbox_, sizeof(to_hub) * (size_t)(s->n_outbox_ + 1));
    s->outbox_[s->n_outbox_++] = m;
}

static int same_size(const img *a, const img *b)
{
    return a->w == b->w && a->h == b->h;
}

/* Forget every friend's preview (the picture changed size, say) */
static void clear_previews(session *s)
{
    for (int i = 0; i < s->n_previews_; i++)
        map_clear(&s->previews_[i].tiles);
    free(s->previews_);
    s->previews_ = NULL;
    s->n_previews_ = 0;
    img_free(s->overlay);
    s->overlay = NULL;
    s->ow_ = s->base->w;
    s->oh_ = s->base->h;
    s->overlay_dirty = irect_of(s->base);
}

static int overlay_fits(const session *s)
{
    return s->ow_ == s->base->w && s->oh_ == s->base->h;
}

/* The overlay's no longer needed once no friend is drawing */
static void drop_overlay_if_unused(session *s)
{
    if (s->overlay && !session_has_previews(s))
    {
        s->overlay_dirty = irect_union(s->overlay_dirty, irect_of(s->overlay));
        img_free(s->overlay);
        s->overlay = NULL;
    }
}

/* ---- `confirmed`, whole or by tile ---- */

/* Keep `confirmed` whole from now on */
static void confirmed_whole(session *s)
{
    if (s->confirmed_ || !(s->confirmed_ = img_dup(s->base)))
        return;
    snap_restore(s->under_, s->confirmed_, irect_of(s->base));
    snap_free(s->under_);
    s->under_ = NULL;
}

/* With none of my edits unconfirmed, `confirmed` is `base`: keep no pixels of its own */
static void confirmed_settle(session *s)
{
    if (s->n_pending_ || (!s->confirmed_ && !s->under_->n_kept)
        || (s->confirmed_ && (!same_size(s->confirmed_, s->base) || memcmp(s->confirmed_->px, s->base->px, (size_t)s->base->w * s->base->h * 4))))
        return;
    snap *u = snap_new(s->base);
    if (!u)
        return;
    snap_free(s->under_);
    s->under_ = u;
    img_free(s->confirmed_);
    s->confirmed_ = NULL;
}

/* My edit is about to change `base` in g: keep `confirmed`'s pixels there */
static void confirmed_keep(session *s, region g)
{
    if (s->confirmed_)
        return;
    if (g.whole)
        confirmed_whole(s);
    else
        snap_keep(s->under_, s->base, g.r, NULL);
}

/* Start from `picture` as `base` and `confirmed`, then my unconfirmed edits on top */
static void start_from(session *s, img *picture)
{
    img_free(s->confirmed_);
    img_free(s->base);
    snap_free(s->under_);
    s->confirmed_ = NULL;
    s->base = picture;
    s->under_ = snap_new(picture);
    if (!s->under_)
        confirmed_whole(s);
}

session *session_new(peer_id me, peer_id host, uint64_t seq, img *picture, kid_list kids)
{
    session *s = calloc(1, sizeof *s);
    s->me = me;
    s->host = host;
    s->kids = kids;
    start_from(s, picture);
    s->seq_ = seq;
    s->next_id_ = 1;
    clear_previews(s);
    return s;
}

void session_free(session *s)
{
    if (!s)
        return;
    clear_previews(s);
    kid_list_free(&s->kids);
    kid_list_free(&s->waiting);
    img_free(s->base);
    img_free(s->confirmed_);
    snap_free(s->under_);
    free_pending(s);
    free_outbox(s);
    map_clear(&s->sent_);
    free(s->pointers);
    free(s);
}

void session_rejoin(session *s, peer_id me, peer_id host, uint64_t seq, img *picture, kid_list kids)
{
    s->me = me;
    s->host = host;
    s->seq_ = seq;
    kid_list_free(&s->kids);
    s->kids = kids;
    kid_list_free(&s->waiting);
    start_from(s, picture);
    free_outbox(s);
    for (int i = 0; i < s->n_pending_; i++)
    {
        confirmed_keep(s, edit_region(&s->pending_[i].e, s->base));
        edit_apply(&s->pending_[i].e, &s->base);
        to_hub m = { 0 };
        m.kind = TO_EDIT;
        m.id = s->pending_[i].id;
        m.edit = edit_clone(&s->pending_[i].e);
        push_outbox(s, m);
    }
    map_clear(&s->sent_);
    s->resend_all_ = 1;
    s->has_last_presence_ = 0;
    free(s->pointers);
    s->pointers = NULL;
    s->n_pointers = 0;
    clear_previews(s);
    s->version++;
}

int session_has_previews(const session *s)
{
    for (int i = 0; i < s->n_previews_; i++)
        if (s->previews_[i].tiles.n)
            return 1;
    return 0;
}

/* Redraw one tile of the overlay from everyone's previews */
static void recompose(session *s, uint32_t key)
{
    if (!s->overlay)
    {
        s->overlay = img_new(s->ow_, s->oh_, (rgba){ 0, 0, 0, 0 });
        if (!s->overlay)
            return;
    }
    irect r = irect_intersect(tile_rect((uint16_t)(key >> 16), (uint16_t)key), irect_of(s->overlay));
    if (irect_empty(r))
        return;
    raster_fill_rect(s->overlay, r, (rgba){ 0, 0, 0, 0 });
    for (int i = 0; i < s->n_previews_; i++)
    {
        const patch *p = map_get(&s->previews_[i].tiles, key);
        if (p)
            patch_apply_within(p, s->overlay, r);
    }
    s->overlay_dirty = irect_union(s->overlay_dirty, r);
}

static int find_preview(const session *s, peer_id id)
{
    for (int i = 0; i < s->n_previews_; i++)
        if (s->previews_[i].id == id)
            return i;
    return -1;
}

/* A friend finished (or left): their preview goes away */
static void drop_preview(session *s, peer_id kid)
{
    int i = find_preview(s, kid);
    if (i < 0)
        return;
    tile_map gone = s->previews_[i].tiles;
    memmove(&s->previews_[i], &s->previews_[i + 1], sizeof(friend_preview) * (size_t)(s->n_previews_ - i - 1));
    s->n_previews_--;
    for (int k = 0; k < gone.cap; k++)
        if (gone.v[k].used)
            recompose(s, gone.v[k].key);
    map_clear(&gone);
    drop_overlay_if_unused(s);
}

int session_preview_tiles(session *s, const img *screen, irect changed, tile_list *out)
{
    memset(out, 0, sizeof *out);
    if (!same_size(screen, s->base))
        return 0;
    irect bounds = irect_of(screen);
    irect area = s->resend_all_ ? bounds : irect_intersect(changed, bounds);
    s->resend_all_ = 0;
    if (irect_empty(area))
        return 0;
    for (int ty = area.y0 / TILE; ty <= (area.y1 - 1) / TILE; ty++)
        for (int tx = area.x0 / TILE; tx <= (area.x1 - 1) / TILE; tx++)
        {
            uint32_t key = tile_key((uint16_t)tx, (uint16_t)ty);
            patch now;
            int has = patch_diff(s->base, screen, tile_rect((uint16_t)tx, (uint16_t)ty), &now);
            const patch *was = map_get(&s->sent_, key);
            if (has ? was && patch_eq(&now, was) : !was)
            {
                patch_free(&now);
                continue;
            }
            tile t = { (uint16_t)tx, (uint16_t)ty, has, { 0 } };
            if (has)
            {
                t.patch = now;
                map_put(&s->sent_, key, patch_clone(&now));
            }
            else
                map_remove(&s->sent_, key);
            out->v = realloc(out->v, sizeof(tile) * (size_t)(out->n + 1));
            out->v[out->n++] = t;
        }
    return out->n > 0;
}

static int presence_eq(const presence *a, const presence *b)
{
    return a->has_at == b->has_at && (!a->has_at || (a->x == b->x && a->y == b->y)) && a->tool == b->tool
           && !memcmp(a->color, b->color, 4) && a->size == b->size;
}

int session_presence(session *s, presence p, to_hub *out)
{
    memset(out, 0, sizeof *out);
    if (s->has_last_presence_ && presence_eq(&s->last_presence_, &p))
        return 0;
    s->has_last_presence_ = 1;
    s->last_presence_ = p;
    out->kind = TO_PRESENCE;
    out->presence = p;
    return 1;
}

const img *session_confirmed(const session *s)
{
    /* Rarely asked for with edits of mine on their way (a new host starting from it) */
    if (!s->confirmed_ && s->n_pending_)
        confirmed_whole((session *)s);
    return s->confirmed_ ? s->confirmed_ : s->base;
}

int session_unconfirmed(const session *s)
{
    return s->n_pending_;
}

void session_commit(session *s, edit e)
{
    confirmed_keep(s, edit_region(&e, s->base));
    edit_apply(&e, &s->base);
    to_hub m = { 0 };
    m.kind = TO_EDIT;
    m.id = s->next_id_++;
    m.edit = edit_clone(&e);
    push_outbox(s, m);
    s->pending_ = realloc(s->pending_, sizeof(pending_edit) * (size_t)(s->n_pending_ + 1));
    s->pending_[s->n_pending_].e = e;
    s->pending_[s->n_pending_].id = m.id;
    s->n_pending_++;
    s->version++;
    /* Friends drop my preview when this edit reaches them */
    map_clear(&s->sent_);
    s->resend_all_ = 1;
    if (!overlay_fits(s))
        clear_previews(s);
}

to_hub *session_take_outbox(session *s, int *n)
{
    to_hub *out = s->outbox_;
    *n = s->n_outbox_;
    s->outbox_ = NULL;
    s->n_outbox_ = 0;
    return out;
}

void session_send(session *s, to_hub msg)
{
    push_outbox(s, msg);
}

int session_replaces_picture(const session *s, const from_hub *msg)
{
    return msg->kind == FROM_SEQUENCED && msg->edit.kind == EDIT_REPLACE && msg->from != s->me;
}

static int has_kid(const kid_list *l, peer_id id)
{
    for (int i = 0; i < l->n; i++)
        if (l->v[i].id == id)
            return 1;
    return 0;
}

/* An edit the hub put in order: `confirmed` takes it, and `base` is `confirmed` with my
 * unconfirmed edits on top again where it changed (all of it, for a whole-picture edit) */
static region take_in(session *s, const edit *e)
{
    if (e->kind != EDIT_PATCH)
        confirmed_whole(s);
    region g = edit_region(e, s->confirmed_ ? s->confirmed_ : s->base);
    if (s->confirmed_)
    {
        edit_apply(e, &s->confirmed_);
        return rebuild(s, g);
    }
    /* By tile: `base` there becomes `confirmed`, takes the edit, and that is kept as `confirmed`;
     * outside the kept tiles `base` was `confirmed` all along */
    snap_restore(s->under_, s->base, g.r);
    edit_apply(e, &s->base);
    snap_rebase(s->under_, s->base, g.r);
    for (int i = 0; i < s->n_pending_; i++)
        patch_apply_within(&s->pending_[i].e.p, s->base, g.r);
    return g;
}

/* Recompute `base` where `confirmed` changed, keeping my unconfirmed edits on top */
static region rebuild(session *s, region g)
{
    int only_patches = 1;
    for (int i = 0; i < s->n_pending_; i++)
        only_patches &= s->pending_[i].e.kind == EDIT_PATCH;
    if (!g.whole && same_size(s->base, s->confirmed_) && only_patches)
    {
        raster_copy_rect(s->base, s->confirmed_, g.r);
        for (int i = 0; i < s->n_pending_; i++)
            patch_apply_within(&s->pending_[i].e.p, s->base, g.r);
        return g;
    }
    img_free(s->base);
    s->base = img_dup(s->confirmed_);
    for (int i = 0; i < s->n_pending_; i++)
        edit_apply(&s->pending_[i].e, &s->base);
    g.whole = 1;
    g.r = IRECT_EMPTY;
    return g;
}

received session_receive(session *s, from_hub *msg)
{
    received got = { RECEIVED_NOTHING, { 0, IRECT_EMPTY } };
    switch (msg->kind)
    {
    case FROM_KIDS:
    case FROM_KICKED_OUT:
    {
        kid_list_free(&s->kids);
        s->kids = msg->kids;
        memset(&msg->kids, 0, sizeof msg->kids);
        for (int i = s->n_previews_ - 1; i >= 0; i--)
            if (!has_kid(&s->kids, s->previews_[i].id))
                drop_preview(s, s->previews_[i].id);
        int n = 0;
        for (int i = 0; i < s->n_pointers; i++)
            if (has_kid(&s->kids, s->pointers[i].id))
                s->pointers[n++] = s->pointers[i];
        s->n_pointers = n;
        got.kind = RECEIVED_KIDS;
        break;
    }
    case FROM_WAITING:
        kid_list_free(&s->waiting);
        s->waiting = msg->kids;
        memset(&msg->kids, 0, sizeof msg->kids);
        break;
    case FROM_ASK_FIRST:
        s->ask_first = msg->yes;
        break;
    case FROM_PREVIEW:
    {
        int i = find_preview(s, msg->from);
        if (i < 0)
        {
            for (i = 0; i < s->n_previews_ && s->previews_[i].id < msg->from; i++)
                ;
            s->previews_ = realloc(s->previews_, sizeof(friend_preview) * (size_t)(s->n_previews_ + 1));
            memmove(&s->previews_[i + 1], &s->previews_[i], sizeof(friend_preview) * (size_t)(s->n_previews_ - i));
            memset(&s->previews_[i], 0, sizeof s->previews_[i]);
            s->previews_[i].id = msg->from;
            s->n_previews_++;
        }
        tile_map *theirs = &s->previews_[i].tiles;
        for (int k = 0; k < msg->tiles.n; k++)
        {
            tile *t = &msg->tiles.v[k];
            if (t->has_patch)
            {
                map_put(theirs, tile_key(t->tx, t->ty), t->patch);
                memset(&t->patch, 0, sizeof t->patch);
            }
            else
                map_remove(theirs, tile_key(t->tx, t->ty));
        }
        for (int k = 0; k < msg->tiles.n; k++)
            recompose(s, tile_key(msg->tiles.v[k].tx, msg->tiles.v[k].ty));
        drop_overlay_if_unused(s);
        break;
    }
    case FROM_PRESENCE:
    {
        int i = 0;
        while (i < s->n_pointers && s->pointers[i].id < msg->from)
            i++;
        if (i == s->n_pointers || s->pointers[i].id != msg->from)
        {
            s->pointers = realloc(s->pointers, sizeof(pointer) * (size_t)(s->n_pointers + 1));
            memmove(&s->pointers[i + 1], &s->pointers[i], sizeof(pointer) * (size_t)(s->n_pointers - i));
            s->n_pointers++;
        }
        s->pointers[i].id = msg->from;
        s->pointers[i].p = msg->presence;
        s->pointers[i].at = s->now;
        break;
    }
    case FROM_SEQUENCED:
    {
        s->seq_ = msg->seq;
        int mine = msg->from == s->me && s->n_pending_ && s->pending_[0].id == msg->id;
        if (mine)
        {
            /* Already part of `base` and the screen */
            edit_free(&s->pending_[0].e);
            memmove(&s->pending_[0], &s->pending_[1], sizeof(pending_edit) * (size_t)(s->n_pending_ - 1));
            s->n_pending_--;
            if (s->confirmed_)
                edit_apply(&msg->edit, &s->confirmed_);
            else if (s->n_pending_)
                take_in(s, &msg->edit);
            confirmed_settle(s);
            break;
        }
        s->foreign++;
        s->version++;
        drop_preview(s, msg->from);
        region g = take_in(s, &msg->edit);
        confirmed_settle(s);
        if (g.whole && !overlay_fits(s))
        {
            clear_previews(s);
            map_clear(&s->sent_);
            s->resend_all_ = 1;
        }
        got.kind = RECEIVED_PICTURE;
        got.region = g;
        break;
    }
    default:    /* welcome, asking, rejected, ping: the app's business */
        break;
    }
    from_hub_free(msg);
    return got;
}
