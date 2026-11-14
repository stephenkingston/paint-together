/*
 * Paint (C port) - the program: the command line, the window and its loop (Rust: main.rs and
 * eframe's part of app.rs's update)
 *
 * Host events are queued in the widget kit; each time host_run is about to
 * wait, the app takes in the network (app_tick) and, if anything is due, one
 * frame is built from all the events that arrived (so a burst of pointer
 * moves makes one frame), drawn into the host's surface, and only the rows
 * that changed are presented: a hash per row of pixels finds them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include "app/view.h"
#include "os/os.h"
#include "paint/logo.h"
#include "ui/font.h"

#ifdef _WIN32
/* The command line in UTF-16, so paths with any letters arrive (main's argv is the ANSI code page) */
__declspec(dllimport) wchar_t *__stdcall GetCommandLineW(void);
__declspec(dllimport) wchar_t **__stdcall CommandLineToArgvW(const wchar_t *line, int *argc);
__declspec(dllimport) int __stdcall WideCharToMultiByte(unsigned cp, unsigned long flags, const wchar_t *w, int wn, char *s, int sn,
                                                        const char *def, int *used);

static char **utf8_argv(int *argc)
{
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), argc);
    char **v = calloc((size_t)*argc + 1, sizeof *v);
    for (int i = 0; w && i < *argc; i++)
    {
        int n = WideCharToMultiByte(65001, 0, w[i], -1, NULL, 0, NULL, NULL);
        v[i] = malloc((size_t)n);
        WideCharToMultiByte(65001, 0, w[i], -1, v[i], n, NULL, NULL);
    }
    return v;
}
#endif

static app *A;
static host_win *W;
static uint32_t *row_hash;      /* each row of the last frame presented */
static int hash_h, held, cursor = -1;
static char title[300] = "Untitled - Paint";

/* Present the runs of rows whose pixels changed since the last frame (all of them after a resize) */
static void present(surface *s)
{
    int all = hash_h != s->h;
    if (all)
    {
        free(row_hash);
        row_hash = malloc(((size_t)s->h + 1) * sizeof *row_hash);
        hash_h = s->h;
    }
    int start = -1;
    for (int y = 0; y <= s->h; y++)
    {
        int changed = 0;
        if (y < s->h)
        {
            const uint32_t *p = s->px + (size_t)y * s->stride;
            uint32_t h = 2166136261u;
            for (int x = 0; x < s->w; x++)
                h = (h ^ p[x]) * 16777619u;
            changed = all || h != row_hash[y];
            row_hash[y] = h;
        }
        if (changed && start < 0)
            start = y;
        if (!changed && start >= 0)
        {
            host_present(W, rect_make(0, start, s->w, y - start));
            start = -1;
        }
    }
}

static void frame(void)
{
    if (!W)
        return;
    surface *s = host_surface(W);
    for (int pass = 0; pass < 3; pass++)
    {
        kit_begin(s, os_now());
        view_build(A);
        if (!pass)
        {
            view_keys(A);
            /* A click or key may have changed what was drawn before it was handled: draw once more */
            int n;
            const ui_event *ev = kit_events(&n);
            for (int i = 0; i < n; i++)
                if (ev[i].type != EV_MOVE)
                    kit_repaint();
        }
        if (!kit_end())
            break;
    }
    present(s);
    if (kit_cursor() != cursor)
        host_cursor(W, cursor = kit_cursor());
    const char *t = app_title(A);
    if (strcmp(t, title))
    {
        snprintf(title, sizeof title, "%s", t);
        host_title(W, title);
    }
    /* Something the frame did changed the picture: show it */
    if (A->dirty_full || !irect_empty(A->dirty))
        kit_repaint();
}

static void on_event(void *user, const ui_event *ev)
{
    switch (ev->type)
    {
    case EV_EXPOSE:
        host_present(W, rect_make(ev->x, ev->y, ev->w, ev->h));
        return;
    case EV_SIZE:
        theme_set_scale(host_scale());
        app_set_scale(A, host_scale() / 100.0f);
        hash_h = -1;
        /* Draw at once: during a live resize the system's own loop runs, not host_run's */
        kit_repaint();
        frame();
        return;
    case EV_CLOSE:
        if (app_close_requested(A))
            host_quit();
        kit_repaint();
        return;
    case EV_DROP:
        if (ev->n_files > 0)
            app_request(A, PENDING_OPEN, ev->files[0]);
        kit_repaint();
        return;
    case EV_TICK:
        return;
    case EV_DOWN:
        if (!held++)
            host_capture(W, 1);
        break;
    case EV_UP:
        if (held && !--held)
            host_capture(W, 0);
        break;
    case EV_FOCUS:
        if (!ev->button && held)
            held = 0, host_capture(W, 0);
        break;
    }
    kit_event(ev);
}

/* Before host_run waits: take in the network, build a frame if one is due, and say when to come back */
static int on_wait(void *user)
{
    if (app_tick(A, os_now()))
        kit_repaint();
    if (kit_wants_frame())
        frame();
    if (A->quit)
        host_quit();
    int k = kit_timeout_ms(), w = app_wake_ms(A);
    return k < 0 ? w : w < 0 || k < w ? k : w;
}

int main(int argc, char **argv)
{
#if defined(__GLIBC__)
    /* One malloc arena: the network threads otherwise get their own, each holding memory */
    mallopt(M_ARENA_MAX, 1);
    /* Big blocks (pictures, undo steps) from the system and back to it when freed: glibc's
     * sliding threshold would otherwise keep tens of MB in the heap after a big picture goes */
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
#endif
#ifdef _WIN32
    argv = utf8_argv(&argc);
#endif
    /* Older macOS gives apps the Finder starts a -psn_ argument: not ours */
    int kept = 1;
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], "-psn_", 5))
            argv[kept++] = argv[i];
    argc = kept;
    os_init();
    const char *path, *join;
    int status = app_cli(argc, argv, &path, &join);
    if (status >= 0)
        return status;
    if (host_init() != 0)
    {
        fprintf(stderr, "paint: can't open a window: no display (is DISPLAY set?)\n");
        return 1;
    }
    theme_set_scale(host_scale());
    if (font_init() != 0)
    {
        fprintf(stderr, "paint: no font found for the window's text\n");
        return 1;
    }
    kit_init();
    A = app_new();
    app_set_scale(A, host_scale() / 100.0f);

    /* Rust's default size, 1460 x 900 points, within the screen; at least 760 x 480 */
    rect work = host_work_area();
    int w = S(1460), h = S(900);
    if (work.w > 0 && w > work.w)
        w = work.w;
    if (work.h > 0 && h > work.h)
        h = work.h;
    W = host_open("Untitled - Paint", w, h, on_event, NULL);
    if (!W)
    {
        fprintf(stderr, "paint: can't open a window\n");
        return 1;
    }
    host_min_size(W, S(760), S(480));
    img *icon = logo_render(
#ifdef __APPLE__
        256     /* the Dock's */
#else
        64      /* the hosts make the smaller ones from it */
#endif
    );
    if (icon)
        host_icon(W, icon);
    img_free(icon);
    A->win = W;
    app_start(A, path, join);
    host_show(W, 0);
    host_run(on_wait, NULL);

    host_close(W);
    view_free();
    app_free(A);
    free(row_hash);
    return 0;
}
