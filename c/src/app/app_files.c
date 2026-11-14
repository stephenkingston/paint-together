/*
 * Paint (C port) - pictures on disk, the recent list, the unsaved-changes
 * prompt and the command line (Rust: app.rs's document and settings parts)
 *
 * When the platform has no file picker, the app asks for a path itself:
 * ask_kind is set and the views show a "type a path" dialog, whose answer
 * comes back through app_ask_path_ok.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/app_internal.h"
#include "collab/names.h"
#include "paint/logo.h"

static const char *const OPEN_FILTERS[] = { "All picture files", "*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.ico", NULL };
static const char *const SAVE_FILTERS[] = { "PNG", "*.png", "JPEG", "*.jpg;*.jpeg", "BMP", "*.bmp", "GIF", "*.gif", "TIFF", "*.tif;*.tiff", NULL };

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(malloc(n), s, n);
}

/* ---------------------------------------------------------------- command line */

int app_cli(int argc, char **argv, const char **path, const char **join)
{
    *path = *join = NULL;
    const char *first = argc > 1 ? argv[1] : NULL;
    /* `--export-icon PATH [SIZE]`: used by the packaging scripts */
    if (first && !strcmp(first, "--export-icon"))
    {
        const char *out = argc > 2 ? argv[2] : "AppIcon.png", *err = "out of memory";
        int size = argc > 3 ? atoi(argv[3]) : 0;
        img *logo = logo_render(size > 0 ? size : 1024);
        if (!logo || img_save(logo, out, &err) != 0)
        {
            fprintf(stderr, "couldn't write %s: %s\n", out, err);
            img_free(logo);
            return 1;
        }
        img_free(logo);
        return 0;
    }
    /* `--join ADDRESS`: join a friend's drawing straight away; otherwise a picture to open */
    if (first && !strcmp(first, "--join"))
        *join = argc > 2 ? argv[2] : NULL;
    else
        *path = first;
    return -1;
}

void app_start(app *a, const char *path, const char *join)
{
    a->lan_discovery = 1;
    if (!app_load_me(a))
    {
        names_random(a->me_name, sizeof a->me_name, a->me_color);
        app_save_me(a);
    }
    if (path)
        app_open_path(a, path);
    if (join)
    {
        a->tab = TAB_TOGETHER;
        app_join_together(a, join);
    }
}

/* ---------------------------------------------------------------- settings */

static char *settings_file(const char *name)
{
    return os_path_join(os_config_dir(), name);
}

void app_load_recent(app *a)
{
    char *file = settings_file("recent.txt");
    char *text = os_read_file(file, NULL);
    free(file);
    if (!text)
        return;
    for (char *line = strtok(text, "\r\n"); line && a->n_recent < 9; line = strtok(NULL, "\r\n"))
        if (*line)
            a->recent[a->n_recent++] = dup_str(line);
    free(text);
}

static void save_recent(const app *a)
{
    size_t len = 0;
    for (int i = 0; i < a->n_recent; i++)
        len += strlen(a->recent[i]) + 1;
    char *text = malloc(len + 1), *p = text;
    for (int i = 0; i < a->n_recent; i++)
        p += sprintf(p, "%s\n", a->recent[i]);
    char *file = settings_file("recent.txt");
    os_write_file(file, text, len);
    free(file);
    free(text);
}

static char *full_path(const char *path)
{
#ifdef _WIN32
    char *abs = _fullpath(NULL, path, 0);
#else
    char *abs = realpath(path, NULL);
#endif
    return abs ? abs : dup_str(path);
}

static void add_recent(app *a, const char *path)
{
    char *p = full_path(path);
    for (int i = 0; i < a->n_recent; i++)
        if (!strcmp(a->recent[i], p))
        {
            free(a->recent[i]);
            memmove(a->recent + i, a->recent + i + 1, (size_t)(a->n_recent - i - 1) * sizeof *a->recent);
            a->n_recent--;
            break;
        }
    if (a->n_recent == 9)
        free(a->recent[--a->n_recent]);
    memmove(a->recent + 1, a->recent, (size_t)a->n_recent * sizeof *a->recent);
    a->recent[0] = p;
    a->n_recent++;
    save_recent(a);
}

/* My name and colour for drawing together, and whether friends ask to join
 * drawings I start, as last chosen */
int app_load_me(app *a)
{
    char *file = settings_file("me.txt");
    char *text = os_read_file(file, NULL);
    free(file);
    if (!text)
        return 0;
    char *lines[3] = { 0 };
    char *p = text;
    for (int i = 0; i < 3 && p; i++)
    {
        lines[i] = p;
        p = strchr(p, '\n');
        if (p)
            *p++ = 0;
        size_t n = strlen(lines[i]);
        if (n && lines[i][n - 1] == '\r')
            lines[i][n - 1] = 0;
    }
    int ok = 0;
    if (lines[0] && lines[1])
    {
        /* Three numbers 0..255, comma-separated (anything else in the list is skipped) */
        int c[3], n = 0;
        for (char *v = strtok(lines[1], ","); v; v = strtok(NULL, ","))
        {
            char *end;
            while (*v == ' ' || *v == '\t')
                v++;
            long x = strtol(v, &end, 10);
            while (*end == ' ' || *end == '\t')
                end++;
            if (end != v && !*end && x >= 0 && x <= 255 && *v != '-' && *v != '+')
            {
                if (n < 3)
                    c[n] = (int)x;
                n++;
            }
        }
        if (n == 3)
        {
            names_tidy(lines[0], a->me_name, sizeof a->me_name);
            for (int i = 0; i < 3; i++)
                a->me_color[i] = (uint8_t)c[i];
            a->ask_first = lines[2] && !strcmp(lines[2], "ask first");
            ok = 1;
        }
    }
    free(text);
    return ok;
}

void app_save_me(const app *a)
{
    char text[200];
    int n = snprintf(text, sizeof text, "%s\n%d,%d,%d\n%s\n", a->me_name, a->me_color[0], a->me_color[1], a->me_color[2],
                     a->ask_first ? "ask first" : "let in");
    char *file = settings_file("me.txt");
    os_write_file(file, text, (size_t)n);
    free(file);
}

/* ---------------------------------------------------------------- the prompt */

void app_request(app *a, int what, const char *arg)
{
    pending p = { 0 };
    pending_set(&p, what, arg);
    if (a->session && (what == PENDING_NEW || what == PENDING_OPEN))
    {
        pending_clear(&a->confirm_everyone);
        a->confirm_everyone = p;
        return;
    }
    if (app_is_dirty(a))
    {
        pending_clear(&a->confirm);
        a->confirm = p;
    }
    else
        app_perform(a, &p);
}

static void open_dialog(app *a)
{
    int unavailable = 0;
    char *path = host_open_file(a->win, "Open", OPEN_FILTERS, &unavailable);
    if (unavailable)
    {
        a->ask_kind = ASK_OPEN;
        a->ask_name[0] = 0;
        return;
    }
    if (path)
        app_open_path(a, path);
    free(path);
}

void app_perform(app *a, pending *p)
{
    switch (p->kind)
    {
    case PENDING_NEW:
        app_load_image(a, img_new(APP_DEFAULT_W, APP_DEFAULT_H, (rgba){ 255, 255, 255, 255 }));
        free(a->path);
        a->path = NULL;
        a->has_meta = 0;
        break;
    case PENDING_OPEN:
        if (p->arg)
            app_open_path(a, p->arg);
        else
            open_dialog(a);
        break;
    case PENDING_JOIN:
        app_join_together(a, p->arg ? p->arg : "");
        break;
    case PENDING_QUIT:
        a->allow_close = a->quit = 1;
        host_quit();
        break;
    }
    pending_clear(p);
}

void app_confirm_answer(app *a, int choice)
{
    pending p = a->confirm;
    a->confirm = (pending){ 0 };
    if (choice == 0)
    {
        /* Save first; if it asks for a path, the action waits for that */
        int saved = app_save(a);
        if (saved)
            app_perform(a, &p);
        else if (a->ask_kind == ASK_SAVE)
        {
            pending_clear(&a->ask_then);
            a->ask_then = p;
        }
        else
            pending_clear(&p);
    }
    else if (choice == 1)
        app_perform(a, &p);
    else
        pending_clear(&p);
}

void app_confirm_everyone_answer(app *a, int go)
{
    pending p = a->confirm_everyone;
    a->confirm_everyone = (pending){ 0 };
    if (go)
        app_perform(a, &p);
    else
        pending_clear(&p);
}

int app_close_requested(app *a)
{
    if (a->allow_close || !app_is_dirty(a))
        return 1;
    pending_clear(&a->confirm);
    a->confirm.kind = PENDING_QUIT;
    return 0;
}

void app_dismiss_error(app *a)
{
    free(a->error);
    a->error = NULL;
}

/* ---------------------------------------------------------------- open and save */

static void read_meta(app *a, const char *path)
{
    a->has_meta = os_file_size(path, &a->file_size, &a->file_mtime) == 0;
}

int app_open_path(app *a, const char *path)
{
    const char *err = "";
    img *im = img_load(path, &err);
    if (!im)
    {
        app_set_error(a, "Paint cannot read %s.\n\n%s", path, err);
        return 0;
    }
    char *keep = dup_str(path);
    app_load_image(a, im);
    read_meta(a, keep);
    add_recent(a, keep);
    free(a->path);
    a->path = keep;
    return 1;
}

static int save_to(app *a, const char *path)
{
    app_commit_all(a);
    const char *err = "";
    if (img_save(a->img, path, &err) != 0)
    {
        app_set_error(a, "Paint cannot save %s.\n\n%s", path, err);
        return 0;
    }
    char *keep = dup_str(path);
    read_meta(a, keep);
    add_recent(a, keep);
    free(a->path);
    a->path = keep;
    a->saved_state = history_state_id(a->history);
    a->saved_version = a->session ? a->session->version : 0;
    return 1;
}

static int has_extension(const char *path)
{
    const char *base = os_path_base(path), *dot = strrchr(base, '.');
    return dot && dot != base && dot[1];
}

/* Save to what the picker (or the "type a path" dialog) gave, adding ext if the name has none */
static int save_picked(app *a, const char *path, const char *ext)
{
    if (has_extension(path))
        return save_to(a, path);
    size_t n = strlen(path) + strlen(ext) + 2;
    char *full = malloc(n);
    snprintf(full, n, "%s.%s", path, ext);
    int ok = save_to(a, full);
    free(full);
    return ok;
}

static int save_with(app *a, const char *name, const char *const *filters, const char *ext)
{
    int unavailable = 0;
    char *path = host_save_file(a->win, "Save As", name, filters, &unavailable);
    if (unavailable)
    {
        a->ask_kind = ASK_SAVE;
        snprintf(a->ask_ext, sizeof a->ask_ext, "%s", ext);
        snprintf(a->ask_name, sizeof a->ask_name, "%s", name);
        return 0;
    }
    int ok = path && save_picked(a, path, ext);
    free(path);
    return ok;
}

int app_save(app *a)
{
    if (a->path)
    {
        char *path = dup_str(a->path);
        int ok = save_to(a, path);
        free(path);
        return ok;
    }
    return app_save_as(a);
}

int app_save_as(app *a)
{
    return save_with(a, a->path ? os_path_base(a->path) : "Untitled.png", SAVE_FILTERS, "png");
}

int app_save_as_format(app *a, const char *ext)
{
    if (!ext)
        return app_save_as(a);
    char name[256];
    const char *base = a->path ? os_path_base(a->path) : "Untitled", *dot = strrchr(base, '.');
    int stem = dot && dot != base ? (int)(dot - base) : (int)strlen(base);
    snprintf(name, sizeof name, "%.*s.%s", stem, base, ext);
    const char *const png[] = { "PNG", "*.png", NULL }, *const jpg[] = { "JPEG", "*.jpg;*.jpeg", NULL };
    const char *const bmp[] = { "BMP", "*.bmp", NULL }, *const gif[] = { "GIF", "*.gif", NULL };
    const char *const *filters = !strcmp(ext, "png") ? png : !strcmp(ext, "jpg") ? jpg : !strcmp(ext, "bmp") ? bmp : gif;
    return save_with(a, name, filters, ext);
}

void app_paste_from(app *a)
{
    int unavailable = 0;
    char *path = host_open_file(a->win, "Paste From", OPEN_FILTERS, &unavailable);
    if (unavailable)
    {
        a->ask_kind = ASK_PASTE_FROM;
        a->ask_name[0] = 0;
        return;
    }
    if (!path)
        return;
    const char *err = "";
    img *im = img_load(path, &err);
    if (im)
        app_paste_image(a, im);
    else
        app_set_error(a, "Paint cannot read %s.\n\n%s", path, err);
    free(path);
}

void app_ask_path_ok(app *a, const char *path)
{
    /* ~ is the home folder, as the field's hint and a shell have it */
    char home[1100];
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\\'))
    {
        snprintf(home, sizeof home, "%s%s", os_home_dir(), path + 1);
        path = home;
    }
    int kind = a->ask_kind;
    a->ask_kind = ASK_NONE;
    pending then = a->ask_then;
    a->ask_then = (pending){ 0 };
    if (kind == ASK_OPEN)
        app_open_path(a, path);
    else if (kind == ASK_PASTE_FROM)
    {
        const char *err = "";
        img *im = img_load(path, &err);
        if (im)
            app_paste_image(a, im);
        else
            app_set_error(a, "Paint cannot read %s.\n\n%s", path, err);
    }
    else if (kind == ASK_SAVE && save_picked(a, path, a->ask_ext) && then.kind)
    {
        app_perform(a, &then);
        return;
    }
    pending_clear(&then);
}

void app_ask_path_cancel(app *a)
{
    a->ask_kind = ASK_NONE;
    pending_clear(&a->ask_then);
}

/* ---------------------------------------------------------------- Properties */

void app_format_bytes(int64_t b, char *buf, size_t cap)
{
    if (b < 1024)
        snprintf(buf, cap, "%d bytes", (int)b);   /* (not %lld: XP's msvcrt.dll can't print long long) */
    else if (b < 1048576)
        snprintf(buf, cap, "%.1fKB", (double)b / 1024.0);
    else
        snprintf(buf, cap, "%.1fMB", (double)b / 1048576.0);
}

void app_format_time(int64_t secs, char *buf, size_t cap)
{
    os_tm t;
    os_localtime(secs, &t);
    snprintf(buf, cap, "%02d/%02d/%d %02d:%02d", t.day, t.month, t.year, t.hour, t.min);
}
