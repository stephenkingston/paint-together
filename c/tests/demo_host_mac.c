/*
 * Paint (C port) - a hand-run check of the macOS host (not one of `make test`)
 *
 * Opens the window, draws a few fills, logs every event to stdout and
 * exercises the host on keys (Command is KMOD_CTRL):
 *   Escape quit (also on closing the window or Command-Q)   F11 full screen
 *   0..9, -   the cursors, CURSOR_ARROW..CURSOR_BUSY
 *   Cmd+C     copy text          Cmd+V    paste text
 *   Cmd+Shift+C copy a picture   Cmd+Shift+V paste a picture (drawn at the left)
 *   Cmd+O     the Open picker    Cmd+S    the Save picker
 *   H         is there a picture on the clipboard?
 * A thread wakes the loop once, two seconds in.
 *
 * --auto runs all of that by itself, one step a tick, with no input: the
 * pickers close themselves, and with SHOTS=<dir> it takes screenshots of the
 * window (screencapture -l) along the way. --idle opens the window and quits
 * after IDLE seconds (default 10), for measuring memory. --feed only hands the
 * view made-up keys, clicks and scrolls (see feed_input) and quits.
 *
 * Build: cc -std=c11 -Isrc -Ithird_party/stb tests/demo_host_mac.c src/host/cocoa_mac.c src/os/os_posix.c
 *        src/core/img.c src/core/stb_impl.c -framework Cocoa -framework Carbon -framework IOSurface -lobjc
 */
#include <ApplicationServices/ApplicationServices.h>
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os/os.h"
#include "ui/host.h"

#define SEND(o, s) ((id (*)(id, SEL))objc_msgSend)((id)(o), sel_registerName(s))
#define CLASS(name) ((id)objc_getClass(name))

static host_win *win;
static img *pasted;
static int ticks, full, last_x = -1, last_y = -1, mode;    /* mode 1 auto, 2 idle */
static long window_number;
static const char *shots;

static const char *const names[] = { "MOVE", "DOWN", "UP", "WHEEL", "LEAVE", "KEY", "KEY_UP", "TEXT", "SIZE",
                                     "EXPOSE", "FOCUS", "CLOSE", "DROP", "TICK" };
static const char *filters[] = { "PNG (*.png)", "*.png", "Pictures", "*.png;*.jpg;*.jpeg;*.bmp", "All files", "*", NULL };

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
            px[1] = (uint8_t)(64 + (x * 7 + y * 13) % 32);
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
    fill(s, s->w - 21, s->h - 41, 20, 20, 0xFF000000);     /* marks the bottom right corner */
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

static void shot(const char *name, long number)
{
    char cmd[1024];
    if (!shots)
        return;
    snprintf(cmd, sizeof(cmd), "screencapture -x -o -l%ld '%s/%s.png'", number, shots, name);
    printf("shot %s: %d\n", name, system(cmd));
}

/* While a picker is open: a screenshot of it, then Cancel */
static void close_picker(void *name)
{
    id app = SEND(objc_getClass("NSApplication"), "sharedApplication"), panel = SEND(app, "modalWindow");
    printf("picker up: %s\n", panel ? "yes" : "no");
    if (panel)
    {
        shot(name, (long)SEND(panel, "windowNumber"));
        ((void (*)(id, SEL, id))objc_msgSend)(panel, sel_registerName("cancel:"), nil);
    }
    else
        SEND(app, "abortModal");
}

static void picker(int open)
{
    int unavailable = 0;
    if (mode == 1)
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 2000000000), dispatch_get_main_queue(), open ? "open" : "save",
                         close_picker);
    char *path = open ? host_open_file(win, "Open a picture", filters, &unavailable) :
                 host_save_file(win, "Save the picture", "/tmp/untitled.png", filters, &unavailable);
    printf("picker: %s unavailable=%d\n", path ? path : "(cancelled)", unavailable);
    free(path);
}

/* Events made here and handed to the view's methods (never posted to the system, so
 * nothing reaches other programs): keys, text through the input context, Command
 * chords, a dead key, clicks and a touchpad scroll */
static id make_key(int type, int code, long flags, const char *chars)
{
    id text = ((id (*)(id, SEL, const char *))objc_msgSend)(CLASS("NSString"), sel_registerName("stringWithUTF8String:"), chars);
    SEL make = sel_registerName("keyEventWithType:location:modifierFlags:timestamp:windowNumber:context:characters:"
                                "charactersIgnoringModifiers:isARepeat:keyCode:");
    return ((id (*)(id, SEL, unsigned long, CGPoint, unsigned long, double, long, id, id, id, BOOL, unsigned short))objc_msgSend)(
        CLASS("NSEvent"), make, (unsigned long)type, CGPointZero, (unsigned long)flags, 0.0, window_number, nil, text, text, NO,
        (unsigned short)code);
}

static id make_click(int type, double x, double y, int clicks, long flags)
{
    SEL make = sel_registerName("mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:"
                                "clickCount:pressure:");
    return ((id (*)(id, SEL, unsigned long, CGPoint, unsigned long, double, long, id, long, long, float))objc_msgSend)(
        CLASS("NSEvent"), make, (unsigned long)type, CGPointMake(x, y), (unsigned long)flags, 0.0, window_number, nil, 0, clicks, 1.0f);
}

static id first_window(void)
{
    id windows = SEND(SEND(CLASS("NSApplication"), "sharedApplication"), "windows");
    return ((id (*)(id, SEL, long))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), 0);
}

static id from_cg(CGEventRef event)
{
    return ((id (*)(id, SEL, CGEventRef))objc_msgSend)(CLASS("NSEvent"), sel_registerName("eventWithCGEvent:"), event);
}

static void feed(const char *method, id event)
{
    id view = SEND(first_window(), "contentView");
    printf("-- %s\n", method);
    ((void (*)(id, SEL, id))objc_msgSend)(view, sel_registerName(method), event);
}

static void feed_input(void *unused)
{
    enum { SHIFT = 1 << 17, OPTION = 1 << 19, COMMAND = 1 << 20, CONTROL = 1 << 18 };
    feed("keyDown:", make_key(10, 0, 0, "a"));                 /* A: EV_KEY 'A', EV_TEXT "a" */
    feed("keyUp:", make_key(11, 0, 0, "a"));
    feed("keyDown:", make_key(10, 0x12, SHIFT, "!"));          /* Shift+1: EV_KEY '1', EV_TEXT "!" */
    feed("flagsChanged:", make_key(12, 0x37, COMMAND, ""));    /* Command down: EV_KEY KEY_CTRL */
    feed("keyDown:", make_key(10, 6, COMMAND, "z"));           /* Command+Z: EV_KEY 'Z' mods 2, no text */
    feed("keyUp:", make_key(11, 6, COMMAND, "z"));
    feed("flagsChanged:", make_key(12, 0x37, 0, ""));          /* Command up */
    feed("keyDown:", make_key(10, 0x7B, 0, "\xEF\x9C\x82"));   /* left arrow: KEY_LEFT, no text */
    feed("keyDown:", make_key(10, 0x24, 0, "\r"));             /* Enter */
    feed("keyDown:", make_key(10, 0x0E, OPTION, ""));  /* Option+E: a dead key (marked text) */
    feed("keyDown:", make_key(10, 0x0E, 0, "e"));               /* then E: EV_TEXT "é" */
    feed("mouseDown:", make_click(1, 100.5, 400.5, 1, 0));     /* x 100, y 99 (500 - 401) */
    feed("mouseDragged:", make_click(6, 110.5, 390.5, 1, 0));
    feed("mouseUp:", make_click(2, 110.5, 390.5, 1, 0));
    feed("mouseDown:", make_click(1, 100.5, 400.5, 2, 0));     /* a double click */
    feed("mouseUp:", make_click(2, 100.5, 400.5, 2, 0));
    feed("mouseDown:", make_click(1, 50.5, 50.5, 1, CONTROL)); /* Control-click: the right button */
    feed("mouseUp:", make_click(2, 50.5, 50.5, 1, 0));
    feed("rightMouseDown:", make_click(3, 60.5, 60.5, 1, SHIFT));
    feed("rightMouseUp:", make_click(4, 60.5, 60.5, 1, SHIFT));
    CGEventRef scroll = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitPixel, 2, -25, 10);   /* a touchpad: points */
    feed("scrollWheel:", from_cg(scroll));
    CFRelease(scroll);
    scroll = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, 1);                    /* a wheel: a line up */
    feed("scrollWheel:", from_cg(scroll));
    CFRelease(scroll);
}

/* --auto: one step a tick */
static void auto_step(void)
{
    int step = ticks;
    if (mode == 3 && step > 1)  /* --feed: only that */
        step = step == 2 ? 25 : 27;
    if (step == 1)
    {
        window_number = (long)SEND(first_window(), "windowNumber");
        printf("window %ld\n", window_number);
        shot("window", window_number);
    }
    else if (step >= 2 && step <= 12)
    {
        printf("cursor %d\n", step - 2);
        host_cursor(win, step - 2);
    }
    else if (step == 13)
    {
        const char *text = "Hello from Paint \xE2\x9C\x93 (\xC3\xBCn\xC3\xAF""c\xC3\xB6""d\xC3\xA9)";
        host_clip_set_text(text);
        char *back = host_clip_text();
        printf("text round trip: %s \"%s\" has picture %d\n", back && !strcmp(back, text) ? "OK" : "FAILED", back ? back : "",
               host_clip_has_image());
        free(back);
    }
    else if (step == 14)
    {
        img *im = make_picture(200);
        host_clip_set_image(im);
        img_free(pasted);
        pasted = host_clip_image();
        int same = pasted && pasted->w == im->w && pasted->h == im->h && !memcmp(pasted->px, im->px, (size_t)im->w * im->h * 4);
        printf("picture round trip: %s %dx%d, has picture %d\n", same ? "OK" : "FAILED", pasted ? pasted->w : 0,
               pasted ? pasted->h : 0, host_clip_has_image());
        img_free(im);
        host_cursor(win, CURSOR_ARROW);
        draw();
    }
    else if (step == 15)
        shot("pasted", window_number);
    else if (step == 16)
        picker(1);
    else if (step == 17)
        picker(0);
    else if (step == 18)
    {
        host_title(win, "Paint host demo \xE2\x80\x94 full screen");
        host_fullscreen(win, full = 1);
    }
    else if (step == 22)
        shot("fullscreen", window_number);
    else if (step == 23)
        host_fullscreen(win, full = 0);
    else if (step == 25)    /* (after this tick: input met while the app handles an event is dropped) */
        dispatch_async_f(dispatch_get_main_queue(), NULL, feed_input);
    else if (step == 27)
    {
        shot("back", window_number);
        host_quit();
    }
}

static void on_event(void *user, const ui_event *ev)
{
    printf("%-7s x=%d y=%d w=%d h=%d button=%d clicks=%d wheel=%.2f,%.2f key=%d repeat=%d mods=%d", names[ev->type], ev->x,
           ev->y, ev->w, ev->h, ev->button, ev->clicks, ev->wheel_x, ev->wheel_y, ev->key, ev->repeat, ev->mods);
    if (ev->type == EV_TEXT)
        printf(" text=\"%s\"", ev->text);
    for (int idx = 0; ev->type == EV_DROP && idx < ev->n_files; idx++)
        printf(" file=\"%s\"", ev->files[idx]);
    printf("\n");
    int ctrl = ev->mods & KMOD_CTRL, shift = ev->mods & KMOD_SHIFT;
    switch (ev->type)
    {
    case EV_CLOSE:
        host_quit();
        break;
    case EV_TICK:
        ticks++;
        draw();
        if (mode == 1 || mode == 3)
            auto_step();
        else if (mode == 2 && ticks * 0.5 >= (getenv("IDLE") ? atof(getenv("IDLE")) : 10))
            host_quit();
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
            host_clip_set_text("Hello from Paint \xE2\x9C\x93 (\xC3\xBCn\xC3\xAF""c\xC3\xB6""d\xC3\xA9)");
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
            picker(ev->key == 'O');
        break;
    default:
        break;
    }
    fflush(stdout);
}

static int timeout(void *user)
{
    return mode ? 500 : 1000;
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
    mode = argc < 2 ? 0 : !strcmp(argv[1], "--auto") ? 1 : !strcmp(argv[1], "--idle") ? 2 : !strcmp(argv[1], "--feed") ? 3 : 0;
    shots = getenv("SHOTS");
    if (host_init())
    {
        fprintf(stderr, "no display\n");
        return 1;
    }
    rect work = host_work_area();
    printf("pid %d, scale %d%%, work area %d,%d %dx%d\n", os_getpid(), host_scale(), work.x, work.y, work.w, work.h);
    win = host_open("Paint host demo \xE2\x80\x94 \xC3\xBCn\xC3\xAF""c\xC3\xB6""d\xC3\xA9", 800 * host_scale() / 100,
                    500 * host_scale() / 100, on_event, NULL);
    surface *s = host_surface(win);
    printf("surface %dx%d\n", s->w, s->h);
    if (mode != 2)
    {
        img *icon = make_picture(64);
        host_icon(win, icon);
        img_free(icon);
    }
    host_min_size(win, 400 * host_scale() / 100, 300 * host_scale() / 100);
    draw();
    host_show(win, argc > 1 && !strcmp(argv[1], "--maximized"));
    os_thread_detach(os_thread_start(waker, NULL));
    fflush(stdout);
    host_run(timeout, NULL);
    host_close(win);
    img_free(pasted);
    printf("bye\n");
    return 0;
}
