/*
 * Paint (C port) - what a platform host provides
 *
 * host/x11_linux.c (the X11 wire protocol over the socket, no Xlib; MIT-SHM
 * when the server has it), host/win32_win.c (Win32 + GDI DIB) and
 * host/headless.c (memory only, for tests) implement it.
 *
 * Paint has one window. The app draws the whole client area into the
 * host's surface (a software back buffer) and presents the part that
 * changed. Dialogs, menus and the thumbnail are drawn by the app inside
 * the window; only the Open/Save pickers are the platform's.
 *
 * Everything runs on the UI thread except host_wake. Sizes and positions
 * are device pixels; the app scales its own metrics by host_scale().
 *
 * What every host does the same way (the details the declarations below
 * cannot say):
 *  - Events come only from inside host_run, one at a time: the handler is
 *    never re-entered. Events a host meets while the handler is running (a
 *    host call that makes the system resize the window at once, a picker's
 *    own message loop) are held and delivered after it returns; input met
 *    meanwhile (keys, pointer, text, drops) may be dropped.
 *  - While a picker (host_open_file / host_save_file) is open, the window
 *    keeps repainting from the surface (its last frame) and the clipboard
 *    stays served; the app gets nothing until the picker returns.
 *  - The surface is the one copy of the window's pixels (on X11 the shared
 *    memory the server reads, on Windows the DIB section's bits): call
 *    host_surface before drawing a frame (on X11 it waits until the server
 *    has read the last host_present), and do not keep its px across
 *    EV_SIZE. After EV_SIZE its contents are undefined: redraw it all.
 *  - EV_EXPOSE may never come where the host repaints from the surface by
 *    itself (Windows): keep the surface holding the last frame.
 *  - host_scale() can change while running (Windows: the window moved to a
 *    monitor of another DPI); EV_SIZE follows such a change.
 *  - Pointer: call host_capture(win, 1) on EV_DOWN and (win, 0) on the last
 *    EV_UP to keep getting EV_MOVE outside the window during a drag.
 *    EV_LEAVE is not sent while a button is held, but after the release
 *    when it happened outside.
 *    EV_DOWN's clicks is 2 for the second press of a double click (by the
 *    system's time and distance), 1 for a third.
 *  - Keys: EV_KEY only for keys host.h names (special keys, and printable
 *    keys as their unshifted ASCII; on layouts whose keys are not Latin, the
 *    character a US keyboard has in that place, so shortcuts still work).
 *    EV_TEXT comes after it, and not while Ctrl or Alt is held (AltGr is
 *    neither: its characters do come as text). Control characters (Tab,
 *    Enter, Backspace, Escape, Delete) never come as text. mods is the
 *    state of the modifiers at the time of the event (for a modifier's own
 *    EV_KEY / EV_KEY_UP, before it).
 *  - The clipboard's text ends lines with '\n' (the host converts).
 *  - Pickers, clipboard calls and host_fullscreen may be called from the
 *    handler; the pickers block until the user answers.
 */
#ifndef UI_HOST_H
#define UI_HOST_H

#include "core/img.h"
#include "ui/gfx.h"

typedef struct host_win host_win;

enum
{
    EV_MOVE,        /* x y: the pointer moved (also while a button is held) */
    EV_DOWN,        /* x y button clicks (1 single, 2 double) */
    EV_UP,          /* x y button */
    EV_WHEEL,       /* x y wheel_x wheel_y: notches (fractional for touchpads); + is up / right */
    EV_LEAVE,       /* the pointer left the window */
    EV_KEY,         /* key: went down (or auto-repeats, repeat 1) */
    EV_KEY_UP,      /* key */
    EV_TEXT,        /* text: characters typed (UTF-8), after the EV_KEY that made them */
    EV_SIZE,        /* the window has a new size w h; host_surface() is already resized */
    EV_EXPOSE,      /* x y w h must be shown again (host_present that area) */
    EV_FOCUS,       /* button: 1 gained the keyboard focus, 0 lost it */
    EV_CLOSE,       /* the user asked to close the window (the app decides) */
    EV_DROP,        /* files (paths) dropped at x y */
    EV_TICK,        /* the timeout given to host_run passed, or host_wake was called */
};

enum
{
    BTN_LEFT = 0,
    BTN_RIGHT = 1,
    BTN_MIDDLE = 2,
};

/* Keys: printable keys come as their unshifted ASCII character, letters
 * upper case ('A'..'Z', '0'..'9', '=', '-', ',', '.', '/', ...), whatever
 * the keyboard layout produces for that key without Shift. */
enum
{
    KEY_BACKSPACE = 8,
    KEY_TAB = 9,
    KEY_ENTER = 13,
    KEY_ESCAPE = 27,
    KEY_SPACE = 32,
    KEY_DELETE = 127,
    KEY_LEFT = 0x100,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_INSERT,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
    KEY_KP_ADD,         /* keypad + and - (zoom shortcuts) */
    KEY_KP_SUBTRACT,
    KEY_SHIFT,          /* the modifier keys themselves (for EV_KEY_UP of Shift while dragging) */
    KEY_CTRL,
    KEY_ALT,
};

enum
{
    KMOD_SHIFT = 1,
    KMOD_CTRL = 2,      /* Command on a Mac */
    KMOD_ALT = 4,
};

typedef struct
{
    int type;
    int x, y, w, h;     /* pointer position; EV_SIZE / EV_EXPOSE: area */
    int button;         /* BTN_*; EV_FOCUS: 1 or 0 */
    int clicks;
    float wheel_x, wheel_y;
    int key, repeat;
    int mods;           /* KMOD_* held during this event (all event types) */
    const char *text;
    const char *const *files;
    int n_files;
    double time;        /* os_now() when it happened */
} ui_event;

enum
{
    CURSOR_ARROW,
    CURSOR_HAND,
    CURSOR_TEXT,        /* I-beam */
    CURSOR_CROSS,       /* crosshair: shapes, selections */
    CURSOR_MOVE,        /* four arrows */
    CURSOR_SIZE_NS,
    CURSOR_SIZE_EW,
    CURSOR_SIZE_NWSE,
    CURSOR_SIZE_NESW,
    CURSOR_NONE,        /* hidden: the app draws the tool as the pointer */
    CURSOR_BUSY,
};

/* ---- the event loop ---- */
int host_init(void);                    /* 0, or -1: no display */
/* Pump events into the window's handler until host_quit. When `timeout_ms`
 * (called before each wait; -1 = none) passes, or host_wake was called, the
 * handler gets EV_TICK. */
void host_run(int (*timeout_ms)(void *user), void *user);
void host_quit(void);
void host_wake(void);                   /* any thread: an EV_TICK soon */
int host_scale(void);                   /* percent: 100, 125, 150, 200 (Xft.dpi / the monitor DPI) */
rect host_work_area(void);              /* of the primary monitor, without the taskbar / panels */
void host_icon(host_win *win, const img *icon);     /* the window's icon (e.g. 64x64 and smaller) */

/* ---- the window ---- */
host_win *host_open(const char *title, int w, int h, void (*on_event)(void *user, const ui_event *ev), void *user);
void host_close(host_win *win);
void host_show(host_win *win, int maximized);
void host_title(host_win *win, const char *title);
void host_min_size(host_win *win, int w, int h);
void host_fullscreen(host_win *win, int on);
surface *host_surface(host_win *win);   /* the back buffer, the window's size; kept between frames */
void host_present(host_win *win, rect dirty);   /* copy that part of the back buffer to the screen */
void host_cursor(host_win *win, int shape);
void host_capture(host_win *win, int on);       /* keep getting the pointer while a button is held outside */
/* Where an input method should show its candidates (the text caret), window pixels.
 * (X11 has no input method here: dead keys and compose type nothing; a no-op.) */
void host_ime_spot(host_win *win, int x, int y, int h);

/* ---- the desktop ---- */
void host_clip_set_text(const char *utf8);
char *host_clip_text(void);             /* malloc'd, or NULL */
void host_clip_set_image(const img *im);
img *host_clip_image(void);             /* or NULL (also when the clipboard holds no picture) */
int host_clip_has_image(void);          /* cheap check for enabling Paste */

/* The platform's file pickers. `filters` is pairs of label and patterns, ending
 * with NULL: { "PNG (*.png)", "*.png", "All Pictures", "*.png;*.jpg;*.bmp", NULL }.
 * Returns a malloc'd path, or NULL if cancelled. *unavailable is set to 1 (and NULL
 * returned) when there is no picker here (no portal, zenity or kdialog): the app
 * then asks for a path itself. */
char *host_open_file(host_win *owner, const char *title, const char *const *filters, int *unavailable);
char *host_save_file(host_win *owner, const char *title, const char *name, const char *const *filters, int *unavailable);
/* Show a picture's folder / open a URL in the desktop's browser */
void host_open_url(const char *url);

#endif
