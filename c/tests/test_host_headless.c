/*
 * Paint (C port) - the headless host does what tests rely on (host/headless.c)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/headless.h"

static int failures, sizes, last_w, last_h;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(cond))                                                         \
        {                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void on_event(void *user, const ui_event *ev)
{
    if (ev->type == EV_SIZE)
    {
        sizes++;
        last_w = ev->w;
        last_h = ev->h;
    }
}

int main(void)
{
    static const char *const filters[] = { "PNG", "*.png", NULL };
    CHECK(host_init() == 0);
    host_win *win = host_open("Untitled - Paint", 320, 200, on_event, NULL);
    surface *s = host_surface(win);
    CHECK(s->w == 320 && s->h == 200 && s->stride >= 320 && s->clip.w == 320);
    s->px[199 * s->stride + 319] = 0xFF123456;  /* the whole buffer is writable */

    headless_resize(win, 640, 480);
    CHECK(sizes == 1 && last_w == 640 && last_h == 480);
    CHECK(host_surface(win)->w == 640 && host_surface(win)->h == 480);

    host_present(win, (rect){ 10, 10, 5, 5 });
    host_present(win, (rect){ 100, 50, 10, 10 });
    headless_info info = headless_state();
    CHECK(info.presented.x == 10 && info.presented.y == 10 && info.presented.w == 100 && info.presented.h == 50);
    CHECK(headless_state().presented.w == 0);
    CHECK(!strcmp(info.title, "Untitled - Paint"));

    /* the clipboard holds text or a picture, not both */
    CHECK(!host_clip_has_image() && host_clip_text() == NULL);
    host_clip_set_text("hello");
    char *text = host_clip_text();
    CHECK(text && !strcmp(text, "hello"));
    free(text);
    img *im = img_new(3, 2, (rgba){ 1, 2, 3, 4 });
    host_clip_set_image(im);
    img_free(im);
    CHECK(host_clip_has_image() && host_clip_text() == NULL);
    im = host_clip_image();
    CHECK(im && im->w == 3 && im->h == 2 && im->px[3] == 4);
    img_free(im);

    /* the pickers answer from the queue, then are unavailable */
    int unavailable = -1;
    headless_queue_pick("/tmp/a.png");
    headless_queue_pick(NULL);
    char *path = host_save_file(win, "Save as", "untitled.png", filters, &unavailable);
    CHECK(path && !strcmp(path, "/tmp/a.png") && unavailable == 0);
    CHECK(!strcmp(headless_state().picker_name, "untitled.png"));
    free(path);
    CHECK(host_open_file(win, "Open", filters, &unavailable) == NULL && unavailable == 0);
    CHECK(host_open_file(win, "Open", filters, &unavailable) == NULL && unavailable == 1);

    host_cursor(win, CURSOR_CROSS);
    host_fullscreen(win, 1);
    host_wake();
    host_quit();
    info = headless_state();
    CHECK(info.cursor == CURSOR_CROSS && info.fullscreen && info.wakes == 1 && info.quit);
    host_close(win);
    printf("%s\n", failures ? "host_headless: FAILED" : "host_headless: ok");
    return failures != 0;
}
