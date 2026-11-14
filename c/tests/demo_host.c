/*
 * Paint (C port) - a hand-run check of the platform host (not one of `make test`)
 *
 * Opens the window, draws a few fills, logs every event to stdout and
 * exercises the host on keys:
 *   Escape quit (also on closing the window)      F11 full screen
 *   0..9, -   the cursors, CURSOR_ARROW..CURSOR_BUSY
 *   Ctrl+C    copy text          Ctrl+V    paste text
 *   Ctrl+Shift+C copy a (noisy, big) picture  Ctrl+Shift+V paste a picture (drawn at the left)
 *   Ctrl+O    the Open picker    Ctrl+S    the Save picker
 *   H         is there a picture on the clipboard?
 * A click captures the pointer until its release; a thread wakes the loop
 * once, two seconds in.
 *
 * Build on Linux:  cc -std=c11 -D_GNU_SOURCE -Isrc -Ithird_party/stb tests/demo_host.c src/host/x11*_linux.c
 *                  src/host/portal_linux.c src/os/os_posix.c src/core/img.c src/core/stb_impl.c -lpthread -lm
 * On Windows: the same with src/host/win32_win.c, src/os/os_win.c and the Makefile's libraries.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os/os.h"
#include "ui/host.h"

static host_win *win;
static img *pasted;
static int ticks, full, last_x = -1, last_y = -1;

static const char *const names[] = { "MOVE", "DOWN", "UP", "WHEEL", "LEAVE", "KEY", "KEY_UP", "TEXT", "SIZE",
                                     "EXPOSE", "FOCUS", "CLOSE", "DROP", "TICK" };

static void fill(surface *s, int x, int y, int w, int h, uint32_t c)
{
    for (int row = y < 0 ? 0 : y; row < y + h && row < s->h; row++)
    {
        for (int col = x < 0 ? 0 : x; col < x + w && col < s->w; col++)
            s->px[(size_t)row * (size_t)s->stride + (size_t)col] = c;
    }
}

static img *make_picture(int size)
{
    img *im = img_new(size, size, (rgba){ 0, 0, 0, 0 });
    for (int y = 0; y < size; y++)
    {   /* a round blob, red to blue, with a soft edge */
        for (int x = 0; x < size; x++)
        {
            int dx = 2 * x - size, dy = 2 * y - size, r2 = dx * dx + dy * dy, edge = size * size;
            uint8_t *px = img_at(im, x, y);
            px[0] = (uint8_t)(255 * x / size);
            px[1] = (uint8_t)(64 + (x * 7 + y * 13) % 32 + rand() % 32);     /* noise: a big PNG, for INCR */
            px[2] = (uint8_t)(255 - 255 * x / size);
            px[3] = r2 < edge * 8 / 10 ? 255 : r2 < edge ? (uint8_t)(255 * (edge - r2) / (edge / 5)) : 0;
        }
    }
    return im;
}

static void draw(void)
{
    surface *s = host_surface(win);
    fill(s, 0, 0, s->w, s->h, 0xFFF3F3F3);
    fill(s, 0, 0, s->w, 40, 0xFF2B5797);
    fill(s, 20, 60, 160, 100, 0xFFE81123);
    fill(s, 200, 60, 160, 100, 0xFF00A300);
    fill(s, 380, 60, 160, 100, 0xFF0078D7);
    fill(s, 0, s->h - 8, s->w * (ticks % 10 + 1) / 10, 8, 0xFFFFB900);   /* moves on each tick */
    for (int y = 0; pasted && y < pasted->h && 180 + y < s->h; y++)
    {
        for (int x = 0; x < pasted->w && 20 + x < s->w; x++)
        {   /* blended on the grey */
            const uint8_t *px = img_at(pasted, x, y);
            int a = px[3];
            uint32_t r = (px[0] * a + 0xF3 * (255 - a)) / 255, g = (px[1] * a + 0xF3 * (255 - a)) / 255;
            uint32_t b = (px[2] * a + 0xF3 * (255 - a)) / 255;
            s->px[(size_t)(180 + y) * (size_t)s->stride + 20 + (size_t)x] = 0xFF000000u | r << 16 | g << 8 | b;
        }
    }
    if (last_x >= 0)
        fill(s, last_x - 3, last_y - 3, 7, 7, 0xFF000000);
    host_present(win, (rect){ 0, 0, s->w, s->h });
}

static void on_event(void *user, const ui_event *ev)
{
    static const char *filters[] = { "PNG (*.png)", "*.png", "Pictures", "*.png;*.jpg;*.jpeg;*.bmp", "All files", "*", NULL };
    printf("%-7s x=%d y=%d w=%d h=%d button=%d clicks=%d wheel=%.1f,%.1f key=%d repeat=%d mods=%d", names[ev->type], ev->x,
           ev->y, ev->w, ev->h, ev->button, ev->clicks, ev->wheel_x, ev->wheel_y, ev->key, ev->repeat, ev->mods);
    if (ev->type == EV_TEXT)
        printf(" text=\"%s\"", ev->text);
    for (int idx = 0; ev->type == EV_DROP && idx < ev->n_files; idx++)
        printf(" file=\"%s\"", ev->files[idx]);
    printf("\n");
    fflush(stdout);
    int ctrl = ev->mods & KMOD_CTRL, shift = ev->mods & KMOD_SHIFT;
    switch (ev->type)
    {
    case EV_CLOSE:
        host_quit();
        break;
    case EV_TICK:
        ticks++;
        draw();
        break;
    case EV_SIZE:
        draw();
        break;
    case EV_EXPOSE:
        host_present(win, (rect){ ev->x, ev->y, ev->w, ev->h });
        break;
    case EV_DOWN:
        host_capture(win, 1);
        last_x = ev->x;
        last_y = ev->y;
        draw();
        break;
    case EV_UP:
        host_capture(win, 0);
        break;
    case EV_KEY:
        if (ev->key == KEY_ESCAPE)
            host_quit();
        else if (ev->key == KEY_F11)
            host_fullscreen(win, full = !full);
        else if (!ctrl && ev->key >= '0' && ev->key <= '9')
            host_cursor(win, ev->key - '0');
        else if (!ctrl && ev->key == '-')
            host_cursor(win, CURSOR_BUSY);
        else if (!ctrl && ev->key == 'H')
            printf("clipboard has a picture: %d\n", host_clip_has_image());
        else if (ctrl && shift && ev->key == 'C')
        {
            img *im = make_picture(400);
            host_clip_set_image(im);
            img_free(im);
            printf("copied a 400x400 picture\n");
        }
        else if (ctrl && ev->key == 'C')
        {
            host_clip_set_text("Hello from Paint \xE2\x9C\x93 (ünïcödé)");
            printf("copied text\n");
        }
        else if (ctrl && shift && ev->key == 'V')
        {
            img_free(pasted);
            pasted = host_clip_image();
            printf("pasted picture: %dx%d\n", pasted ? pasted->w : 0, pasted ? pasted->h : 0);
            draw();
        }
        else if (ctrl && ev->key == 'V')
        {
            char *text = host_clip_text();
            printf("pasted text: \"%s\"\n", text ? text : "(none)");
            free(text);
        }
        else if (ctrl && (ev->key == 'O' || ev->key == 'S'))
        {
            int unavailable = 0;
            char *path = ev->key == 'O' ? host_open_file(win, "Open a picture", filters, &unavailable) :
                         host_save_file(win, "Save the picture", "untitled.png", filters, &unavailable);
            printf("picker: %s unavailable=%d\n", path ? path : "(cancelled)", unavailable);
            free(path);
        }
        fflush(stdout);
        break;
    default:
        break;
    }
}

static int timeout(void *user)
{
    return 1000;
}

static void waker(void *arg)
{
    os_sleep_ms(2000);
    printf("(waking from a thread)\n");
    fflush(stdout);
    host_wake();
}

int main(int argc, char **argv)
{
    os_init();
    int maximized = 0;
    for (int idx = 1; idx < argc; idx++)
    {   /* --log FILE (a Windows GUI program has no stdout), --maximized */
        if (!strcmp(argv[idx], "--log") && idx + 1 < argc && !freopen(argv[++idx], "w", stdout))
            return 1;
        maximized |= !strcmp(argv[idx], "--maximized");
    }
    if (host_init())
    {
        fprintf(stderr, "no display\n");
        return 1;
    }
    rect work = host_work_area();
    printf("scale %d%%, work area %d,%d %dx%d\n", host_scale(), work.x, work.y, work.w, work.h);
    win = host_open("Paint host demo \xE2\x80\x94 ünïcödé", 800, 500, on_event, NULL);
    img *icon = make_picture(64);
    host_icon(win, icon);
    img_free(icon);
    host_min_size(win, 400, 300);
    draw();
    host_show(win, maximized);
    os_thread_detach(os_thread_start(waker, NULL));
    host_run(timeout, NULL);
    host_close(win);
    img_free(pasted);
    printf("bye\n");
    return 0;
}
