/*
 * Paint (C port) - the memory-only host, for tests (ui/host.h, host/headless.h)
 *
 * The surface is plain memory of the size the test opens the window at.
 * Nothing makes events; the clipboard holds text or a picture in memory;
 * the pickers answer from a queue the test fills; host_run returns at once.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/headless.h"

struct host_win
{
    surface surf;
    void (*on_event)(void *user, const ui_event *ev);
    void *user;
};

static headless_info info;
static int scale = 100;
static char *clip_text;
static img *clip_image;
static char *picks[16];
static int n_picks;

int host_init(void)
{
    return 0;
}

void host_run(int (*timeout_ms)(void *user), void *user)
{
}

void host_quit(void)
{
    info.quit = 1;
}

void host_wake(void)
{
    __sync_fetch_and_add(&info.wakes, 1);
}

int host_scale(void)
{
    return scale;
}

void headless_set_scale(int percent)
{
    scale = percent;
}

rect host_work_area(void)
{
    return (rect){ 0, 0, 1920, 1040 };
}

void host_icon(host_win *win, const img *icon)
{
}

static void size_surface(host_win *win, int w, int h)
{
    w = w > 0 ? w : 1;
    h = h > 0 ? h : 1;
    free(win->surf.px);
    win->surf.px = calloc((size_t)w * (size_t)h, 4);
    win->surf.w = win->surf.stride = w;
    win->surf.h = h;
    win->surf.clip = (rect){ 0, 0, w, h };
}

host_win *host_open(const char *title, int w, int h, void (*on_event)(void *user, const ui_event *ev), void *user)
{
    host_win *win = calloc(1, sizeof(*win));
    win->on_event = on_event;
    win->user = user;
    size_surface(win, w, h);
    host_title(win, title);
    return win;
}

void host_close(host_win *win)
{
    free(win->surf.px);
    free(win);
}

void headless_resize(host_win *win, int w, int h)
{
    size_surface(win, w, h);
    headless_send(win, &(ui_event){ .type = EV_SIZE, .w = win->surf.w, .h = win->surf.h });
}

void headless_send(host_win *win, const ui_event *ev)
{
    win->on_event(win->user, ev);
}

void host_show(host_win *win, int maximized)
{
    info.shown = 1;
    info.maximized = maximized;
}

void host_title(host_win *win, const char *title)
{
    snprintf(info.title, sizeof(info.title), "%s", title);
}

void host_min_size(host_win *win, int w, int h)
{
    info.min_w = w;
    info.min_h = h;
}

void host_fullscreen(host_win *win, int on)
{
    info.fullscreen = on;
}

surface *host_surface(host_win *win)
{
    return &win->surf;
}

void host_present(host_win *win, rect dirty)
{
    rect *u = &info.presented;
    if (dirty.w <= 0 || dirty.h <= 0)
        return;
    if (u->w <= 0 || u->h <= 0)
    {
        *u = dirty;
        return;
    }
    int x1 = u->x + u->w > dirty.x + dirty.w ? u->x + u->w : dirty.x + dirty.w;
    int y1 = u->y + u->h > dirty.y + dirty.h ? u->y + u->h : dirty.y + dirty.h;
    u->x = u->x < dirty.x ? u->x : dirty.x;
    u->y = u->y < dirty.y ? u->y : dirty.y;
    u->w = x1 - u->x;
    u->h = y1 - u->y;
}

void host_cursor(host_win *win, int shape)
{
    info.cursor = shape;
}

void host_capture(host_win *win, int on)
{
    info.captured = on;
}

void host_ime_spot(host_win *win, int x, int y, int h)
{
}

headless_info headless_state(void)
{
    headless_info now = info;
    info.presented = (rect){ 0 };
    return now;
}

/* ---- the clipboard ---- */

void host_clip_set_text(const char *utf8)
{
    free(clip_text);
    img_free(clip_image);
    clip_text = strdup(utf8 ? utf8 : "");
    clip_image = NULL;
}

char *host_clip_text(void)
{
    return clip_text ? strdup(clip_text) : NULL;
}

void host_clip_set_image(const img *im)
{
    free(clip_text);
    img_free(clip_image);
    clip_text = NULL;
    clip_image = img_dup(im);
}

img *host_clip_image(void)
{
    return clip_image ? img_dup(clip_image) : NULL;
}

int host_clip_has_image(void)
{
    return clip_image != NULL;
}

/* ---- the pickers ---- */

void headless_queue_pick(const char *path)
{
    if (n_picks < 16)
        picks[n_picks++] = path ? strdup(path) : NULL;
}

static char *next_pick(const char *title, int *unavailable)
{
    int dummy;
    unavailable = unavailable ? unavailable : &dummy;
    snprintf(info.picker_title, sizeof(info.picker_title), "%s", title ? title : "");
    *unavailable = n_picks == 0;
    if (!n_picks)
        return NULL;
    char *path = picks[0];
    memmove(picks, picks + 1, sizeof(*picks) * (size_t)--n_picks);
    return path;
}

char *host_open_file(host_win *owner, const char *title, const char *const *filters, int *unavailable)
{
    info.picker_name[0] = 0;
    return next_pick(title, unavailable);
}

char *host_save_file(host_win *owner, const char *title, const char *name, const char *const *filters, int *unavailable)
{
    snprintf(info.picker_name, sizeof(info.picker_name), "%s", name ? name : "");
    return next_pick(title, unavailable);
}

void host_open_url(const char *url)
{
    snprintf(info.url, sizeof(info.url), "%s", url);
}
