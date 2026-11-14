/*
 * Paint (C port) - the platform host on macOS (ui/host.h): AppKit from plain C
 *
 * There is no Objective-C source: AppKit is called through objc_msgSend cast
 * to each method's real type (arm64 needs the exact prototype; on x86_64 a
 * method returning a CGRect goes through objc_msgSend_stret), and the one
 * class Paint needs is made at run time: a view that is also the window's and
 * the application's delegate, a drop target and an NSTextInputClient (so input
 * methods and dead keys work).
 *
 * The surface is an IOSurface set as the view's layer's contents, so the
 * window server reads the app's pixels where they are. Sizes are device
 * pixels (points times the backing scale) with y down from the top. Command
 * is KMOD_CTRL, as in Paint's Rust build on the Mac; Control-click is the
 * right button.
 *
 * AppKit calls back into the view from inside calls the app makes (making the
 * window key, a file picker's modal loop), so events met while the app is
 * still handling one are held and delivered after it returns; input met then
 * is dropped.
 */
#include <Carbon/Carbon.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurface.h>
#include <limits.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os/os.h"
#include "ui/host.h"

/* What clang emits for @autoreleasepool; not in the SDK's headers */
void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *pool);

typedef intptr_t ip;
typedef struct
{
    unsigned long location, length;
} text_range;   /* NSRange */

#define S(name) sel_registerName(name)
#define CLS(name) ((id)objc_getClass(name))
#if defined(__x86_64__)
#define STRET objc_msgSend_stret
#define B_ "c"  /* BOOL's type encoding */
#else
#define STRET objc_msgSend
#define B_ "B"
#endif
/* Typed sends: MSG(return type, argument types)(object, selector, arguments...) */
#define MSG(ret, ...) ((ret (*)(id, SEL, __VA_ARGS__))objc_msgSend)
#define MSG0(ret) ((ret (*)(id, SEL))objc_msgSend)
/* The common case: integer and object arguments, an object (or an integer) back */
#define msg(o, s) MSG0(id)((id)(o), S(s))
#define msg1(o, s, a) MSG(id, ip)((id)(o), S(s), (ip)(a))
#define msg2(o, s, a, b) MSG(id, ip, ip)((id)(o), S(s), (ip)(a), (ip)(b))
#define msg3(o, s, a, b, c) MSG(id, ip, ip, ip)((id)(o), S(s), (ip)(a), (ip)(b), (ip)(c))
#define msg4(o, s, a, b, c, d) MSG(id, ip, ip, ip, ip)((id)(o), S(s), (ip)(a), (ip)(b), (ip)(c), (ip)(d))
#define msg_is(o, s) MSG0(BOOL)((id)(o), S(s))
#define msg1_is(o, s, a) MSG(BOOL, ip)((id)(o), S(s), (ip)(a))
#define msg_f(o, s) MSG0(double)((id)(o), S(s))
#define msg_pt(o, s) MSG0(CGPoint)((id)(o), S(s))
#define msg_rect(o, s) ((CGRect (*)(id, SEL))STRET)((id)(o), S(s))
#define STR(literal) ((id)CFSTR(literal))

enum
{   /* NSEventModifierFlag* */
    MOD_SHIFT = 1 << 17,
    MOD_CONTROL = 1 << 18,
    MOD_OPTION = 1 << 19,
    MOD_COMMAND = 1 << 20,
};

struct host_win
{
    id window, view;
    surface surf;
    /* The window server shows a surface where it is, while it is shown: so the app draws each
     * frame into another one (`back`, locked, surf.px), handed to the layer whole once drawn.
     * Drawing over the shown one let the screen catch half-drawn frames (a flickering canvas). */
    IOSurfaceRef buf[3];
    int nbuf, back, shown;      /* indexes into buf; back -1: none picked yet; shown -1: none */
    int cap_w, cap_h;           /* the buffers' size: kept while the window fits in it */
    double factor;              /* device pixels per point */
    int buttons, ctrl_click, cursor, hidden, marked;
    int ime_x, ime_y, ime_h;
    unsigned long typed;        /* UTF-16 units typed: where input methods think the caret is */
    double caret_at;            /* when the app last showed a caret (host_ime_spot) */
    void (*on_event)(void *user, const ui_event *ev);
    void *user;
};

static host_win *W;             /* Paint has one window */
static id app;
static Class view_class;
static CGColorSpaceRef srgb;
static volatile int wake_pending;
static int quitting, scale = 100, dispatching, tick_pending;
static int waiting;              /* inside nextEventMatchingMask (Apple events: opening files, Quit from the Dock) */
static int (*run_timeout)(void *user);     /* host_run's, for frames during a live resize */
static void *run_user;
static id input_event;          /* the mouse or key event being handled: its modifiers go with what it makes */
static ui_event held[16];       /* events met while the app was busy (no pointers in them) */
static int n_held;

/* ---- small helpers ---- */

static id nsstr(const char *utf8)
{
    id s = utf8 ? msg1(CLS("NSString"), "stringWithUTF8String:", utf8) : nil;
    return s ? s : STR("");
}

static char *dup_str(id s)
{
    const char *utf8 = s ? (const char *)msg(s, "UTF8String") : NULL;
    return utf8 ? strdup(utf8) : NULL;
}

static id array1(id obj)
{
    return msg1(CLS("NSArray"), "arrayWithObject:", obj);
}

static void release(id obj)
{
    if (obj)
        msg(obj, "release");
}

static int mods_of(ip flags)
{
    return (flags & MOD_SHIFT ? KMOD_SHIFT : 0) | (flags & MOD_COMMAND ? KMOD_CTRL : 0) | (flags & MOD_OPTION ? KMOD_ALT : 0);
}

/* ---- events ---- */

static void put_event(ui_event ev)
{
    if (!W)
        return;
    ev.mods |= mods_of((ip)msg(input_event ? input_event : CLS("NSEvent"), "modifierFlags"));
    ev.time = os_now();
    if (dispatching)
    {   /* the app is busy: keep what can wait, drop input */
        int input = ev.type <= EV_TEXT || ev.type == EV_DROP;
        if (ev.type == EV_TICK)
            tick_pending = 1;
        else if (!input && n_held < 16)
            held[n_held++] = ev;
        return;
    }
    dispatching = 1;
    W->on_event(W->user, &ev);
    while (W && (n_held || tick_pending))
    {
        ui_event next = n_held ? held[0] : (ui_event){ .type = EV_TICK, .time = os_now() };
        if (n_held)
            memmove(held, held + 1, sizeof(*held) * (size_t)--n_held);
        else
            tick_pending = 0;
        W->on_event(W->user, &next);
    }
    n_held = tick_pending = 0;
    dispatching = 0;
    if (waiting)
        host_wake();    /* the wait goes on after this: end it, so host_run draws the frame */
}

/* A point in the window (points, y up) in device pixels of the view, y down */
static void view_point(host_win *win, CGPoint in_window, int *x, int *y)
{
    CGPoint p = MSG(CGPoint, CGPoint, id)(win->view, S("convertPoint:fromView:"), in_window, nil);
    CGRect bounds = msg_rect(win->view, "bounds");
    *x = (int)floor(p.x * win->factor);
    *y = (int)floor((bounds.size.height - p.y) * win->factor);
}

/* Is the pointer over the view now (and not over a window in front of it)? */
static int pointer_inside(host_win *win)
{
    CGPoint at = msg_pt(CLS("NSEvent"), "mouseLocation");
    ip number = MSG(ip, CGPoint, ip)(CLS("NSWindow"), S("windowNumberAtPoint:belowWindowWithWindowNumber:"), at, 0);
    if (number != (ip)msg(win->window, "windowNumber"))
        return 0;
    int x, y;
    view_point(win, MSG(CGPoint, CGPoint)(win->window, S("convertPointFromScreen:"), at), &x, &y);
    return x >= 0 && y >= 0 && x < win->surf.w && y < win->surf.h;
}

/* ---- the back buffer ---- */

/* A BGRA IOSurface (the bytes of 0xAARRGGBB pixels), zero-filled */
static IOSurfaceRef new_buffer(int w, int h)
{
    int four = 4, format = 0x42475241;  /* 'BGRA' */
    const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat };
    const void *values[] = { CFNumberCreate(NULL, kCFNumberIntType, &w), CFNumberCreate(NULL, kCFNumberIntType, &h),
                             CFNumberCreate(NULL, kCFNumberIntType, &four), CFNumberCreate(NULL, kCFNumberIntType, &format) };
    CFDictionaryRef props = CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef buffer = IOSurfaceCreate(props);
    CFRelease(props);
    for (int idx = 0; idx < 4; idx++)
        CFRelease(values[idx]);
    return buffer;
}

/* Let go of every buffer (the layer keeps the one it shows until it gets another) */
static void drop_buffers(host_win *win)
{
    if (win->back >= 0)
        IOSurfaceUnlock(win->buf[win->back], 0, NULL);
    for (int i = 0; i < win->nbuf; i++)
        CFRelease(win->buf[i]);
    win->nbuf = 0;
    win->back = win->shown = -1;
}

/* The buffer the next frame is drawn into: one the screen isn't showing or still reading,
 * made when there's none (two usually do; a third if the window server holds on to one) */
static void pick_back(host_win *win)
{
    if (win->back >= 0)
        return;
    int pick = -1;
    for (int i = 0; i < win->nbuf && pick < 0; i++)
        if (i != win->shown && !IOSurfaceIsInUse(win->buf[i]))
            pick = i;
    if (pick < 0 && win->nbuf < 3)
    {
        IOSurfaceRef b = new_buffer(win->cap_w, win->cap_h);
        if (b)
            win->buf[pick = win->nbuf++] = b;
    }
    for (int i = 0; i < win->nbuf && pick < 0; i++)
        if (i != win->shown)
            pick = i;   /* all busy: draw anyway rather than stall */
    if (pick < 0)
        pick = 0;
    win->back = pick;
    IOSurfaceLock(win->buf[pick], 0, NULL);
    win->surf.px = IOSurfaceGetBaseAddress(win->buf[pick]);
    win->surf.stride = (int)(IOSurfaceGetBytesPerRow(win->buf[pick]) / 4);
}

/* Fit the buffers to the window: new ones when it grows past them, or is a quarter of them */
static void size_buffer(host_win *win, int w, int h)
{
    w = w > 0 ? w : 1;
    h = h > 0 ? h : 1;
    if (w > win->cap_w || h > win->cap_h || (size_t)w * (size_t)h * 4 < (size_t)win->cap_w * (size_t)win->cap_h)
    {
        drop_buffers(win);
        win->cap_w = (w + 31) & ~31;
        win->cap_h = (h + 31) & ~31;
        IOSurfaceRef b = new_buffer(win->cap_w, win->cap_h);
        if (!b)
        {   /* out of memory: a tiny window is better than a crash */
            win->cap_w = win->cap_h = w = h = 1;
            b = new_buffer(1, 1);
        }
        win->buf[win->nbuf++] = b;
        pick_back(win);
    }
    win->surf.w = w;
    win->surf.h = h;
    win->surf.clip = (rect){ 0, 0, w, h };
}

/* The view's new size or backing scale: a new surface and EV_SIZE */
static void resized(id self, SEL cmd, id note)
{
    host_win *win = W;
    if (!win)
        return;
    CGRect bounds = msg_rect(win->view, "bounds");
    double factor = msg_f(win->window, "backingScaleFactor");
    int w = (int)lround(bounds.size.width * factor), h = (int)lround(bounds.size.height * factor);
    scale = (int)lround(factor * 100);
    if (factor == win->factor && w == win->surf.w && h == win->surf.h)
        return;
    win->factor = factor;
    size_buffer(win, w, h);
    put_event((ui_event){ .type = EV_SIZE, .w = win->surf.w, .h = win->surf.h });
    if (W == win && !dispatching && run_timeout && msg_is(win->view, "inLiveResize"))
        run_timeout(run_user);  /* AppKit's resize loop runs instead of host_run's: let the app draw its frame */
}

static void backing_changed(id self, SEL cmd)
{
    resized(self, cmd, nil);
}

/* The view's layer shows the last finished frame's surface itself, with no copy. (A CGImage over
 * plain memory as the contents costs Core Animation a copy of the window's size; drawing that
 * in drawRect: costs a backing store and Metal buffers besides.) */
static void update_layer(id self, SEL cmd)
{
    host_win *win = W;
    if (!win || win->view != self || win->back < 0)
        return;
    id layer = msg(self, "layer");
    MSG(void, double)(layer, S("setContentsScale:"), win->factor);
    IOSurfaceUnlock(win->buf[win->back], 0, NULL);  /* drawn: the compositor may read it now */
    msg1(layer, "setContents:", win->buf[win->back]);
    win->shown = win->back;
    win->back = -1;     /* the next frame picks a buffer once it starts */
}

surface *host_surface(host_win *win)
{
    pick_back(win);
    return &win->surf;
}

/* The whole surface goes to the layer at the next display (one a frame however often this is called) */
void host_present(host_win *win, rect dirty)
{
    if (dirty.w > 0 && dirty.h > 0)
        msg1(win->view, "setNeedsDisplay:", YES);
}

/* ---- the pointer ---- */

/* Four arrows, as winit (WebKit's cursor; AppKit has no public one), or an open hand if it has gone */
static id move_cursor(void)
{
    static id cursor;
    if (cursor)
        return cursor;
    id image = msg1(msg(CLS("NSImage"), "alloc"), "initByReferencingFile:",
                    STR("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/HIServices.framework/"
                        "Versions/A/Resources/cursors/move/cursor.pdf"));
    if (image && msg_is(image, "isValid"))
        cursor = MSG(id, id, CGPoint)(msg(CLS("NSCursor"), "alloc"), S("initWithImage:hotSpot:"), image, CGPointMake(12, 12));
    release(image);
    return cursor ? cursor : (cursor = msg(msg(CLS("NSCursor"), "openHandCursor"), "retain"));
}

static void apply_cursor(host_win *win)
{
    static const char *const names[CURSOR_BUSY + 1][2] = {
        { "arrowCursor", 0 }, { "pointingHandCursor", 0 }, { "IBeamCursor", 0 }, { "crosshairCursor", 0 },
        { "openHandCursor", 0 }, { "_windowResizeNorthSouthCursor", "resizeUpDownCursor" },
        { "_windowResizeEastWestCursor", "resizeLeftRightCursor" }, { "_windowResizeNorthWestSouthEastCursor", "crosshairCursor" },
        { "_windowResizeNorthEastSouthWestCursor", "crosshairCursor" }, { 0, 0 }, { "busyButClickableCursor", "arrowCursor" },
    };
    if (win->cursor == CURSOR_NONE)
    {
        if (!win->hidden)
            msg(CLS("NSCursor"), "hide");
        win->hidden = 1;
        return;
    }
    if (win->hidden)
        msg(CLS("NSCursor"), "unhide");
    win->hidden = 0;
    const char *name = names[win->cursor][0];
    if (names[win->cursor][1] && !msg1_is(CLS("NSCursor"), "respondsToSelector:", S(name)))
        name = names[win->cursor][1];   /* the private ones may go away */
    msg(win->cursor == CURSOR_MOVE ? move_cursor() : msg(CLS("NSCursor"), name), "set");
}

static void show_pointer(host_win *win)
{
    if (win->hidden)
        msg(CLS("NSCursor"), "unhide");
    win->hidden = 0;
}

void host_cursor(host_win *win, int shape)
{
    if (shape < 0 || shape > CURSOR_BUSY)
        return;
    win->cursor = shape;
    if (win->buttons || pointer_inside(win))
        apply_cursor(win);  /* now, not at the next move */
}

void host_capture(host_win *win, int on)
{
    /* AppKit keeps sending a drag to the view the button went down in */
}

static void on_mouse(id self, id event)
{
    host_win *win = W;
    if (!win)
        return;
    ip type = (ip)msg(event, "type");
    int x, y, button = BTN_LEFT;
    view_point(win, msg_pt(event, "locationInWindow"), &x, &y);
    switch (type)
    {
    case 1:     /* NSEventTypeLeftMouseDown, Up, Dragged: Control-click is the right button */
    case 2:
    case 6:
        if (type == 1)
            win->ctrl_click = ((ip)msg(event, "modifierFlags") & MOD_CONTROL) != 0;
        button = win->ctrl_click ? BTN_RIGHT : BTN_LEFT;
        break;
    case 3:     /* right */
    case 4:
    case 7:
        button = BTN_RIGHT;
        break;
    case 25:    /* other: the middle one only */
    case 26:
    case 27:
        if ((ip)msg(event, "buttonNumber") != 2)
            return;
        button = BTN_MIDDLE;
        break;
    }
    switch (type)
    {
    case 1:
    case 3:
    case 25:
    {   /* a third click is a single one again, as on Windows and X11 */
        int clicks = (int)(ip)msg(event, "clickCount");
        win->buttons |= 1 << button;
        put_event((ui_event){ .type = EV_DOWN, .x = x, .y = y, .button = button, .clicks = clicks > 0 ? (clicks - 1) % 2 + 1 : 1 });
        break;
    }
    case 2:
    case 4:
    case 26:
        win->buttons &= ~(1 << button);
        put_event((ui_event){ .type = EV_UP, .x = x, .y = y, .button = button });
        if (W == win && !win->buttons && !pointer_inside(win))
        {   /* let go outside: it has left (the exit came while it was held) */
            show_pointer(win);
            put_event((ui_event){ .type = EV_LEAVE });
        }
        break;
    case 5:     /* moved (over the view: the tracking area's), dragged */
        if (x < 0 || y < 0 || x >= win->surf.w || y >= win->surf.h)
            break;
        /* fall through */
    case 6:
    case 7:
    case 27:
        put_event((ui_event){ .type = EV_MOVE, .x = x, .y = y });
        break;
    case 8:     /* entered, cursorUpdate: */
    case 17:
        apply_cursor(win);
        break;
    case 9:     /* exited */
        show_pointer(win);
        if (!win->buttons)
            put_event((ui_event){ .type = EV_LEAVE });
        break;
    case 22:
    {   /* touchpads say points (egui takes 50 for a wheel's line); wheels say lines */
        double dx = msg_f(event, "scrollingDeltaX"), dy = msg_f(event, "scrollingDeltaY");
        if (msg_is(event, "hasPreciseScrollingDeltas"))
        {
            dx /= 50;
            dy /= 50;
        }
        if (dx != 0 || dy != 0)
            put_event((ui_event){ .type = EV_WHEEL, .x = x, .y = y, .wheel_x = (float)-dx, .wheel_y = (float)dy });
        break;
    }
    case 30:    /* a pinch: Command-wheel, zooming by e^magnification as egui does (the app zooms by e^(0.2 y)) */
        put_event((ui_event){ .type = EV_WHEEL, .x = x, .y = y, .wheel_y = (float)(msg_f(event, "magnification") * 5), .mods = KMOD_CTRL });
        break;
    }
}

/* ---- the keyboard ---- */

/* The host.h key of a virtual key code: what the layout types for it without Shift */
static int key_of(unsigned code)
{
    static const struct
    {
        unsigned char code;
        unsigned short key;
    } keys[] = {
        { kVK_Return, KEY_ENTER }, { kVK_ANSI_KeypadEnter, KEY_ENTER }, { kVK_Tab, KEY_TAB }, { kVK_Space, KEY_SPACE },
        { kVK_Delete, KEY_BACKSPACE }, { kVK_Escape, KEY_ESCAPE }, { kVK_ForwardDelete, KEY_DELETE }, { kVK_Home, KEY_HOME },
        { kVK_End, KEY_END }, { kVK_PageUp, KEY_PAGE_UP }, { kVK_PageDown, KEY_PAGE_DOWN }, { kVK_LeftArrow, KEY_LEFT },
        { kVK_RightArrow, KEY_RIGHT }, { kVK_UpArrow, KEY_UP }, { kVK_DownArrow, KEY_DOWN }, { kVK_Help, KEY_INSERT },
        { kVK_F1, KEY_F1 }, { kVK_F2, KEY_F2 }, { kVK_F3, KEY_F3 }, { kVK_F4, KEY_F4 }, { kVK_F5, KEY_F5 }, { kVK_F6, KEY_F6 },
        { kVK_F7, KEY_F7 }, { kVK_F8, KEY_F8 }, { kVK_F9, KEY_F9 }, { kVK_F10, KEY_F10 }, { kVK_F11, KEY_F11 },
        { kVK_F12, KEY_F12 }, { kVK_ANSI_KeypadPlus, KEY_KP_ADD }, { kVK_ANSI_KeypadMinus, KEY_KP_SUBTRACT },
        { kVK_ANSI_KeypadMultiply, '*' }, { kVK_ANSI_KeypadDivide, '/' }, { kVK_ANSI_KeypadDecimal, '.' },
        { kVK_ANSI_KeypadEquals, '=' }, { kVK_ANSI_Keypad0, '0' }, { kVK_ANSI_Keypad1, '1' }, { kVK_ANSI_Keypad2, '2' },
        { kVK_ANSI_Keypad3, '3' }, { kVK_ANSI_Keypad4, '4' }, { kVK_ANSI_Keypad5, '5' }, { kVK_ANSI_Keypad6, '6' },
        { kVK_ANSI_Keypad7, '7' }, { kVK_ANSI_Keypad8, '8' }, { kVK_ANSI_Keypad9, '9' }, { kVK_Shift, KEY_SHIFT },
        { kVK_RightShift, KEY_SHIFT }, { kVK_Command, KEY_CTRL }, { kVK_RightCommand, KEY_CTRL }, { kVK_Option, KEY_ALT },
        { kVK_RightOption, KEY_ALT },
    };
    /* Where a layout's character is not ASCII: the key's place on a US keyboard, by code (space: none) */
    static const char us[] = "ASDFHGZXCV BQWERYT123465=97-80]OU[IP LJ'K;\\,/NM.  `";
    for (size_t idx = 0; idx < sizeof(keys) / sizeof(*keys); idx++)
    {
        if (keys[idx].code == code)
            return keys[idx].key;
    }
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    CFDataRef layout = source ? TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) : NULL;
    if (!layout)
    {   /* an input method (Japanese, say): its ASCII layout */
        if (source)
            CFRelease(source);
        source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
        layout = source ? TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) : NULL;
    }
    UniChar ch[4];
    UniCharCount len = 0;
    UInt32 dead = 0;
    if (layout)
        UCKeyTranslate((const UCKeyboardLayout *)CFDataGetBytePtr(layout), (UInt16)code, kUCKeyActionDisplay, 0, LMGetKbdType(),
                       kUCKeyTranslateNoDeadKeysMask, &dead, 4, &len, ch);
    if (source)
        CFRelease(source);
    if (len == 1 && ch[0] > ' ' && ch[0] < 0x7F)
        return ch[0] >= 'a' && ch[0] <= 'z' ? ch[0] - 32 : ch[0];
    return code < sizeof(us) - 1 && us[code] != ' ' ? us[code] : 0;
}

static void on_key(id self, id event)
{
    host_win *win = W;
    if (!win)
        return;
    ip type = (ip)msg(event, "type"), flags = (ip)msg(event, "modifierFlags");
    int key = key_of(MSG0(unsigned short)(event, S("keyCode")));
    if (type == 12)
    {   /* NSEventTypeFlagsChanged: a modifier went down or up */
        ip mask = key == KEY_SHIFT ? MOD_SHIFT : key == KEY_CTRL ? MOD_COMMAND : key == KEY_ALT ? MOD_OPTION : 0;
        if (mask)
            put_event((ui_event){ .type = flags & mask ? EV_KEY : EV_KEY_UP, .key = key });
        return;
    }
    if (type == 11)
    {
        if (key)
            put_event((ui_event){ .type = EV_KEY_UP, .key = key });
        return;
    }
    if (key && !win->marked)    /* (while composing, the keys are the input method's) */
        put_event((ui_event){ .type = EV_KEY, .key = key, .repeat = msg_is(event, "isARepeat") });
    if (W == win && !(flags & MOD_COMMAND))
        msg1(self, "interpretKeyEvents:", array1(event));
}

/* Every mouse and key method of the view */
static void on_input(id self, SEL cmd, id event)
{
    ip type = (ip)msg(event, "type");
    id outer = input_event;     /* (a picker's modal loop may bring another) */
    input_event = event;
    if (type >= 10 && type <= 12)   /* key down, up, flags changed */
        on_key(self, event);
    else
        on_mouse(self, event);
    input_event = outer;
}

/* NSTextInputClient: typed text, and an input method's text being composed */
static void insert_text(id self, SEL cmd, id text, text_range replace)
{
    if (msg1_is(text, "isKindOfClass:", CLS("NSAttributedString")))
        text = msg(text, "string");
    const unsigned char *utf8 = (const unsigned char *)msg(text, "UTF8String");
    host_win *win = W;
    if (!win)
        return;
    win->marked = 0;
    /* not control characters, nor the private-use ones AppKit gives function keys */
    if (!utf8 || utf8[0] < ' ' || utf8[0] == 0x7F || (utf8[0] == 0xEF && utf8[1] >= 0x9C && utf8[1] <= 0xA3))
        return;
    /* The accent menu (holding a key) replaces the character just typed: Backspace it first, while
     * the app shows a caret (elsewhere Backspace would delete a selection) */
    if (replace.length && replace.location + replace.length == win->typed && os_now() - win->caret_at < 2)
    {
        win->typed -= replace.length;
        for (unsigned long n = replace.length; n-- && W == win;)
            put_event((ui_event){ .type = EV_KEY, .key = KEY_BACKSPACE });
    }
    win->typed += (unsigned long)(ip)msg(text, "length");
    put_event((ui_event){ .type = EV_TEXT, .text = (const char *)utf8 });
}

static void set_marked(id self, SEL cmd, id text, text_range selected, text_range replace)
{
    if (W)
        W->marked = (int)(ip)msg(text, "length");
}

static void unmark(id self, SEL cmd)
{
    if (W)
        W->marked = 0;
}

static BOOL has_marked(id self, SEL cmd)
{
    return W && W->marked;
}

static text_range marked_range(id self, SEL cmd)
{
    return W && W->marked ? (text_range){ W->typed, (unsigned long)W->marked } : (text_range){ (unsigned long)LONG_MAX, 0 };
}

static text_range selected_range(id self, SEL cmd)
{
    return (text_range){ W ? W->typed + (unsigned long)W->marked : 0, 0 };
}

static id no_substring(id self, SEL cmd, text_range range, text_range *actual)
{
    return nil;
}

static id no_attributes(id self, SEL cmd)
{
    return msg(CLS("NSArray"), "array");
}

static unsigned long no_index(id self, SEL cmd, CGPoint at)
{
    return (unsigned long)LONG_MAX;    /* NSNotFound */
}

static void do_command(id self, SEL cmd, SEL command)
{
    /* Enter, arrows and the like came as EV_KEY already (and no beep) */
}

/* Where the input method shows its candidates: the caret, in screen points */
static CGRect first_rect(id self, SEL cmd, text_range range, text_range *actual)
{
    host_win *win = W;
    if (!win)
        return CGRectZero;
    double f = win->factor, top = msg_rect(self, "bounds").size.height;
    CGRect r = CGRectMake(win->ime_x / f, top - (win->ime_y + win->ime_h) / f, 1 / f, win->ime_h / f);
    r = ((CGRect (*)(id, SEL, CGRect, id))STRET)(self, S("convertRect:toView:"), r, nil);
    return ((CGRect (*)(id, SEL, CGRect))STRET)(win->window, S("convertRectToScreen:"), r);
}

void host_ime_spot(host_win *win, int x, int y, int h)
{
    win->ime_x = x;
    win->ime_y = y;
    win->ime_h = h;
    win->caret_at = os_now();
    msg(msg(win->view, "inputContext"), "invalidateCharacterCoordinates");
}

/* ---- the window's and the application's delegate, the drop target ---- */

static BOOL yes(id self, SEL cmd)
{
    return YES;
}

static BOOL yes1(id self, SEL cmd, id arg)
{
    return YES;
}

static BOOL should_close(id self, SEL cmd, id sender)
{
    put_event((ui_event){ .type = EV_CLOSE });
    return NO;  /* the app decides */
}

static unsigned long should_terminate(id self, SEL cmd, id sender)
{   /* Quit in the menu (Command-Q), the Dock: the same as the close button */
    put_event((ui_event){ .type = EV_CLOSE });
    return 0;   /* NSTerminateCancel */
}

static void focus_in(id self, SEL cmd, id note)
{
    put_event((ui_event){ .type = EV_FOCUS, .button = 1 });
}

static void focus_out(id self, SEL cmd, id note)
{
    if (W)
        show_pointer(W);
    put_event((ui_event){ .type = EV_FOCUS, .button = 0 });
}

/* EV_DROP of a list of NSURLs (file ones) or NSString paths */
static void drop_paths(id list, int x, int y)
{
    int count = (int)(ip)msg(list, "count"), n = 0;
    char **paths = calloc(count > 0 ? (size_t)count : 1, sizeof(char *));
    for (int idx = 0; idx < count; idx++)
    {
        id item = msg1(list, "objectAtIndex:", idx);
        if (!msg1_is(item, "isKindOfClass:", CLS("NSURL")))
            paths[n] = dup_str(item);
        else if (msg_is(item, "isFileURL"))
            paths[n] = dup_str(msg(item, "path"));
        n += paths[n] != NULL;
    }
    if (n)
        put_event((ui_event){ .type = EV_DROP, .x = x, .y = y, .files = (const char *const *)paths, .n_files = n });
    for (int idx = 0; idx < n; idx++)
        free(paths[idx]);
    free(paths);
}

static unsigned long drag_entered(id self, SEL cmd, id info)
{
    return 1;   /* NSDragOperationCopy */
}

static BOOL drag_perform(id self, SEL cmd, id info)
{
    host_win *win = W;
    if (!win)
        return NO;
    int x, y;
    view_point(win, msg_pt(info, "draggingLocation"), &x, &y);
    drop_paths(msg2(msg(info, "draggingPasteboard"), "readObjectsForClasses:options:", array1(CLS("NSURL")), nil), x, y);
    return YES;
}

/* Files opened from the Finder (the bundle's document types): dropped in the middle */
static void open_files(id self, SEL cmd, id sender, id names)
{
    if (W)
        drop_paths(names, W->surf.w / 2, W->surf.h / 2);
    msg1(app, "replyToOpenOrPrint:", 0);
}

static void make_view_class(void)
{
    static const struct
    {
        const char *name;
        IMP fn;
        const char *types;
    } methods[] = {
        { "updateLayer", (IMP)update_layer, "v@:" },
        { "wantsUpdateLayer", (IMP)yes, B_ "@:" },
        { "isOpaque", (IMP)yes, B_ "@:" },
        { "acceptsFirstResponder", (IMP)yes, B_ "@:" },
        { "acceptsFirstMouse:", (IMP)yes1, B_ "@:@" },
        { "viewDidChangeBackingProperties", (IMP)backing_changed, "v@:" },
        { "mouseDown:", (IMP)on_input, "v@:@" },
        { "mouseUp:", (IMP)on_input, "v@:@" },
        { "mouseDragged:", (IMP)on_input, "v@:@" },
        { "mouseMoved:", (IMP)on_input, "v@:@" },
        { "rightMouseDown:", (IMP)on_input, "v@:@" },
        { "rightMouseUp:", (IMP)on_input, "v@:@" },
        { "rightMouseDragged:", (IMP)on_input, "v@:@" },
        { "otherMouseDown:", (IMP)on_input, "v@:@" },
        { "otherMouseUp:", (IMP)on_input, "v@:@" },
        { "otherMouseDragged:", (IMP)on_input, "v@:@" },
        { "mouseEntered:", (IMP)on_input, "v@:@" },
        { "mouseExited:", (IMP)on_input, "v@:@" },
        { "cursorUpdate:", (IMP)on_input, "v@:@" },
        { "scrollWheel:", (IMP)on_input, "v@:@" },
        { "magnifyWithEvent:", (IMP)on_input, "v@:@" },
        { "keyDown:", (IMP)on_input, "v@:@" },
        { "keyUp:", (IMP)on_input, "v@:@" },
        { "flagsChanged:", (IMP)on_input, "v@:@" },
        { "insertText:replacementRange:", (IMP)insert_text, "v@:@{_NSRange=QQ}" },
        { "setMarkedText:selectedRange:replacementRange:", (IMP)set_marked, "v@:@{_NSRange=QQ}{_NSRange=QQ}" },
        { "unmarkText", (IMP)unmark, "v@:" },
        { "hasMarkedText", (IMP)has_marked, B_ "@:" },
        { "markedRange", (IMP)marked_range, "{_NSRange=QQ}@:" },
        { "selectedRange", (IMP)selected_range, "{_NSRange=QQ}@:" },
        { "attributedSubstringForProposedRange:actualRange:", (IMP)no_substring, "@@:{_NSRange=QQ}^{_NSRange=QQ}" },
        { "validAttributesForMarkedText", (IMP)no_attributes, "@@:" },
        { "firstRectForCharacterRange:actualRange:", (IMP)first_rect,
          "{CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}^{_NSRange=QQ}" },
        { "characterIndexForPoint:", (IMP)no_index, "Q@:{CGPoint=dd}" },
        { "doCommandBySelector:", (IMP)do_command, "v@::" },
        { "draggingEntered:", (IMP)drag_entered, "Q@:@" },
        { "performDragOperation:", (IMP)drag_perform, B_ "@:@" },
        { "windowShouldClose:", (IMP)should_close, B_ "@:@" },
        { "windowDidResize:", (IMP)resized, "v@:@" },
        { "windowDidChangeBackingProperties:", (IMP)resized, "v@:@" },
        { "windowDidBecomeKey:", (IMP)focus_in, "v@:@" },
        { "windowDidResignKey:", (IMP)focus_out, "v@:@" },
        { "applicationShouldTerminate:", (IMP)should_terminate, "Q@:@" },
        { "application:openFiles:", (IMP)open_files, "v@:@@" },
        { "applicationSupportsSecureRestorableState:", (IMP)yes1, B_ "@:@" },
    };
    view_class = objc_allocateClassPair(objc_getClass("NSView"), "PaintView", 0);
    for (size_t idx = 0; idx < sizeof(methods) / sizeof(*methods); idx++)
        class_addMethod(view_class, S(methods[idx].name), methods[idx].fn, methods[idx].types);
    Protocol *text_client = objc_getProtocol("NSTextInputClient");
    if (text_client)
        class_addProtocol(view_class, text_client);
    objc_registerClassPair(view_class);
}

/* The application menu: About, Hide, Quit (Command-Q, which asks the app through EV_CLOSE) */
static void make_menu(void)
{
    static const char *const items[][3] = {
        { "About Paint", "orderFrontStandardAboutPanel:", "" }, { 0 }, { "Hide Paint", "hide:", "h" },
        { "Hide Others", "hideOtherApplications:", "h" }, { "Show All", "unhideAllApplications:", "" }, { 0 },
        { "Quit Paint", "terminate:", "q" },
    };
    id bar = msg(msg(CLS("NSMenu"), "alloc"), "init"), item = msg(msg(CLS("NSMenuItem"), "alloc"), "init");
    id menu = msg1(msg(CLS("NSMenu"), "alloc"), "initWithTitle:", STR("Paint"));
    for (size_t idx = 0; idx < sizeof(items) / sizeof(*items); idx++)
    {
        if (!items[idx][0])
        {
            msg1(menu, "addItem:", msg(CLS("NSMenuItem"), "separatorItem"));
            continue;
        }
        id it = msg3(menu, "addItemWithTitle:action:keyEquivalent:", nsstr(items[idx][0]), S(items[idx][1]), nsstr(items[idx][2]));
        if (idx == 3)
            msg1(it, "setKeyEquivalentModifierMask:", MOD_OPTION | MOD_COMMAND);
    }
    msg1(bar, "addItem:", item);
    msg1(item, "setSubmenu:", menu);
    msg1(app, "setMainMenu:", bar);
    release(menu);
    release(item);
    release(bar);
}

/* ---- start-up and the loop ---- */

int host_init(void)
{
    if (app)
        return 0;
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (!session)
        return -1;  /* no window server (logged in over ssh) */
    CFRelease(session);
    void *pool = objc_autoreleasePoolPush();
    /* Cocoa takes locks (for host_wake's postEvent) once it has seen a thread of its own */
    msg3(CLS("NSThread"), "detachNewThreadSelector:toTarget:withObject:", S("class"), CLS("NSObject"), nil);
    app = msg(CLS("NSApplication"), "sharedApplication");
    msg1(app, "setActivationPolicy:", 0);  /* regular: in the Dock, with a menu bar */
    if (msg1_is(CLS("NSWindow"), "respondsToSelector:", S("setAllowsAutomaticWindowTabbing:")))
        msg1(CLS("NSWindow"), "setAllowsAutomaticWindowTabbing:", NO);
    make_view_class();
    make_menu();
    srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    scale = (int)lround(msg_f(msg(CLS("NSScreen"), "mainScreen"), "backingScaleFactor") * 100);
    msg(app, "finishLaunching");
    objc_autoreleasePoolPop(pool);
    return 0;
}

int host_scale(void)
{
    return scale;
}

void host_quit(void)
{
    quitting = 1;
    host_wake();
}

/* Any thread: an application-defined event ends the loop's wait (postEvent: is safe off the main thread) */
void host_wake(void)
{
    if (__atomic_exchange_n(&wake_pending, 1, __ATOMIC_SEQ_CST) || !app)
        return;
    void *pool = objc_autoreleasePoolPush();
    id ev = MSG(id, ip, CGPoint, ip, double, ip, id, short, ip, ip)(
        CLS("NSEvent"), S("otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:"), 15,
        CGPointZero, 0, 0.0, 0, nil, 0, 0, 0);
    msg2(app, "postEvent:atStart:", ev, NO);
    objc_autoreleasePoolPop(pool);
}

static id next_event(id until)
{
    waiting = 1;
    id ev = msg4(app, "nextEventMatchingMask:untilDate:inMode:dequeue:", ~(ip)0, until, kCFRunLoopDefaultMode, YES);
    waiting = 0;
    return ev;
}

static void send_event(id ev)
{
    ip type = (ip)msg(ev, "type");
    if (type == 15)
        return;     /* host_wake's: it only ended the wait */
    if (type == 11 && ((ip)msg(ev, "modifierFlags") & MOD_COMMAND) && msg(ev, "window"))
        msg1(msg(ev, "window"), "sendEvent:", ev);  /* NSApplication drops the key-ups of Command chords */
    else
        msg1(app, "sendEvent:", ev);
}

void host_run(int (*timeout_ms)(void *user), void *user)
{
    quitting = 0;
    run_timeout = timeout_ms;
    run_user = user;
    while (!quitting)
    {
        void *pool = objc_autoreleasePoolPush();
        id ev;
        while (!quitting && (ev = next_event(msg(CLS("NSDate"), "distantPast"))))
            send_event(ev);
        if (!quitting && __atomic_exchange_n(&wake_pending, 0, __ATOMIC_SEQ_CST))
            put_event((ui_event){ .type = EV_TICK });
        if (!quitting)
        {
            int wait = timeout_ms ? timeout_ms(user) : -1;
            double deadline = os_now() + wait / 1000.0;
            id until = wait < 0 ? msg(CLS("NSDate"), "distantFuture")
                                : MSG(id, double)(CLS("NSDate"), S("dateWithTimeIntervalSinceNow:"), wait / 1000.0);
            if ((ev = next_event(until)))
                send_event(ev);
            if (wait >= 0 && !quitting && os_now() >= deadline)
                put_event((ui_event){ .type = EV_TICK });
        }
        objc_autoreleasePoolPop(pool);
    }
}

/* The primary screen (the one with the menu bar) without the menu bar and the Dock */
rect host_work_area(void)
{
    void *pool = objc_autoreleasePoolPush();
    id screen = msg1(msg(CLS("NSScreen"), "screens"), "objectAtIndex:", 0);
    CGRect frame = msg_rect(screen, "frame"), vis = msg_rect(screen, "visibleFrame");
    double f = msg_f(screen, "backingScaleFactor");
    objc_autoreleasePoolPop(pool);
    return (rect){ (int)lround(vis.origin.x * f), (int)lround((frame.size.height - vis.origin.y - vis.size.height) * f),
                   (int)lround(vis.size.width * f), (int)lround(vis.size.height * f) };
}

/* ---- the window ---- */

host_win *host_open(const char *title, int w, int h, void (*on_event)(void *user, const ui_event *ev), void *user)
{
    void *pool = objc_autoreleasePoolPush();
    host_win *win = calloc(1, sizeof(*win));
    double f = scale / 100.0;
    CGRect content = CGRectMake(0, 0, w / f, h / f);
    win->on_event = on_event;
    win->user = user;
    win->back = win->shown = -1;
    /* titled, closable, miniaturizable, resizable; buffered */
    SEL init = S("initWithContentRect:styleMask:backing:defer:");
    win->window = MSG(id, CGRect, unsigned long, unsigned long, BOOL)(msg(CLS("NSWindow"), "alloc"), init, content, 15, 2, NO);
    win->view = MSG(id, CGRect)(msg((id)view_class, "alloc"), S("initWithFrame:"), content);
    msg1(win->window, "setReleasedWhenClosed:", NO);
    msg1(win->view, "setWantsLayer:", YES);
    msg1(win->view, "setLayerContentsRedrawPolicy:", 2);   /* updateLayer on setNeedsDisplay: only */
    msg1(msg(win->view, "layer"), "setContentsGravity:", STR("topLeft"));     /* (kCAGravityTopLeft) unscaled, while resizing too */
    msg1(msg(win->view, "layer"), "setMasksToBounds:", YES);   /* (the buffer may be bigger than the window) */
    msg1(win->window, "setContentView:", win->view);
    msg1(win->window, "setDelegate:", win->view);
    msg1(win->window, "makeFirstResponder:", win->view);
    msg1(win->window, "setCollectionBehavior:", 1 << 7);   /* full screen in its own space */
    msg1(win->window, "setTitle:", nsstr(title));
    msg(win->window, "center");
    /* entered/exited, moved, cursorUpdate:, always, the visible rect, during drags too */
    id area = MSG(id, CGRect, unsigned long, id, id)(msg(CLS("NSTrackingArea"), "alloc"), S("initWithRect:options:owner:userInfo:"),
                                                      CGRectZero, 0x01 | 0x02 | 0x04 | 0x80 | 0x200 | 0x400, win->view, nil);
    msg1(win->view, "addTrackingArea:", area);
    release(area);
    msg1(win->view, "registerForDraggedTypes:", array1(STR("public.file-url")));
    msg1(app, "setDelegate:", win->view);
    win->factor = msg_f(win->window, "backingScaleFactor");
    scale = (int)lround(win->factor * 100);
    CGRect bounds = msg_rect(win->view, "bounds");
    size_buffer(win, (int)lround(bounds.size.width * win->factor), (int)lround(bounds.size.height * win->factor));
    W = win;    /* (only now: setting the window up calls the delegate) */
    objc_autoreleasePoolPop(pool);
    return win;
}

void host_close(host_win *win)
{
    void *pool = objc_autoreleasePoolPush();
    if (W == win)
        W = NULL;
    show_pointer(win);
    msg1(app, "setDelegate:", nil);
    msg1(win->window, "setDelegate:", nil);
    msg(win->window, "close");
    release(win->window);
    release(win->view);
    drop_buffers(win);
    free(win);
    objc_autoreleasePoolPop(pool);
}

void host_show(host_win *win, int maximized)
{
    if (maximized)
    {   /* the screen without the menu bar and the Dock (EV_SIZE follows) */
        id screen = msg(win->window, "screen");
        CGRect vis = msg_rect(screen ? screen : msg(CLS("NSScreen"), "mainScreen"), "visibleFrame");
        MSG(void, CGRect, BOOL)(win->window, S("setFrame:display:"), vis, NO);
    }
    msg1(app, "activateIgnoringOtherApps:", YES);
    msg1(win->window, "makeKeyAndOrderFront:", nil);
}

void host_title(host_win *win, const char *title)
{
    void *pool = objc_autoreleasePoolPush();
    msg1(win->window, "setTitle:", nsstr(title));
    objc_autoreleasePoolPop(pool);
}

void host_min_size(host_win *win, int w, int h)
{
    MSG(void, CGSize)(win->window, S("setContentMinSize:"), CGSizeMake(w / win->factor, h / win->factor));
}

void host_fullscreen(host_win *win, int on)
{
    int now = ((ip)msg(win->window, "styleMask") & (1 << 14)) != 0;    /* NSWindowStyleMaskFullScreen */
    if (on != now)
        msg1(win->window, "toggleFullScreen:", nil);
}

/* The Dock's icon (Mac windows have none) */
void host_icon(host_win *win, const img *icon)
{
    void *pool = objc_autoreleasePoolPush();
    CFDataRef data = CFDataCreate(NULL, icon->px, (CFIndex)icon->w * icon->h * 4);
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef cg = CGImageCreate((size_t)icon->w, (size_t)icon->h, 8, 32, (size_t)icon->w * 4, srgb,
                                  kCGImageAlphaLast | kCGBitmapByteOrderDefault, provider, NULL, false, kCGRenderingIntentDefault);
    id image = MSG(id, CGImageRef, CGSize)(msg(CLS("NSImage"), "alloc"), S("initWithCGImage:size:"), cg, CGSizeZero);
    msg1(app, "setApplicationIconImage:", image);
    release(image);
    CGImageRelease(cg);
    CGDataProviderRelease(provider);
    CFRelease(data);
    objc_autoreleasePoolPop(pool);
}

/* ---- the clipboard ---- */

static id pasteboard(void)
{
    return msg(CLS("NSPasteboard"), "generalPasteboard");
}

void host_clip_set_text(const char *utf8)
{
    void *pool = objc_autoreleasePoolPush();
    id pb = pasteboard();
    msg(pb, "clearContents");
    msg2(pb, "setString:forType:", nsstr(utf8), STR("public.utf8-plain-text"));
    objc_autoreleasePoolPop(pool);
}

char *host_clip_text(void)
{
    void *pool = objc_autoreleasePoolPush();
    char *text = dup_str(msg1(pasteboard(), "stringForType:", STR("public.utf8-plain-text")));
    objc_autoreleasePoolPop(pool);
    return text;
}

/* As PNG: what Mac programs paste today (a TIFF as well would be the whole picture uncompressed) */
void host_clip_set_image(const img *im)
{
    size_t len = 0;
    uint8_t *png = img_png(im, &len);
    if (!png)
        return;
    void *pool = objc_autoreleasePoolPush();
    id data = msg3(CLS("NSData"), "dataWithBytesNoCopy:length:freeWhenDone:", png, len, YES);
    id pb = pasteboard();
    msg(pb, "clearContents");
    msg2(pb, "setData:forType:", data, STR("public.png"));
    objc_autoreleasePoolPop(pool);
}

/* Any other picture (TIFF, and what the pasteboard converts to it) through NSBitmapImageRep, to straight RGBA */
static img *decode_picture(id data)
{
    id rep = msg1(CLS("NSBitmapImageRep"), "imageRepWithData:", data);
    CGImageRef cg = rep ? (CGImageRef)msg(rep, "CGImage") : NULL;
    int w = cg ? (int)CGImageGetWidth(cg) : 0, h = cg ? (int)CGImageGetHeight(cg) : 0;
    img *im = w > 0 && h > 0 ? img_new(w, h, (rgba){ 0, 0, 0, 0 }) : NULL;
    if (!im)
        return NULL;
    CGContextRef ctx = CGBitmapContextCreate(im->px, (size_t)w, (size_t)h, 8, (size_t)w * 4, srgb,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (!ctx)
    {
        img_free(im);
        return NULL;
    }
    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
    CGContextRelease(ctx);
    for (size_t idx = 0, n = (size_t)w * (size_t)h * 4; idx < n; idx += 4)
    {
        uint8_t *px = im->px + idx;
        int a = px[3];
        for (int c = 0; a && a < 255 && c < 3; c++)
            px[c] = (uint8_t)((px[c] * 255 + a / 2) / a > 255 ? 255 : (px[c] * 255 + a / 2) / a);
    }
    return im;
}

img *host_clip_image(void)
{
    void *pool = objc_autoreleasePoolPush();
    id pb = pasteboard(), data = msg1(pb, "dataForType:", STR("public.png"));
    const char *err = NULL;
    img *im = data ? img_load_mem(msg(data, "bytes"), (size_t)(ip)msg(data, "length"), &err) : NULL;
    if (!im && (data = msg1(pb, "dataForType:", STR("public.tiff"))))
        im = decode_picture(data);
    objc_autoreleasePoolPop(pool);
    return im;
}

int host_clip_has_image(void)
{
    void *pool = objc_autoreleasePoolPush();
    id types[2] = { STR("public.png"), STR("public.tiff") };
    id list = msg2(CLS("NSArray"), "arrayWithObjects:count:", types, 2);
    int has = msg1(pasteboard(), "availableTypeFromArray:", list) != nil;
    objc_autoreleasePoolPop(pool);
    return has;
}

/* ---- the file pickers ---- */

/* Every filter's extensions ("*.png;*.jpg" -> png, jpg), as rfd does on the Mac: one list, no "All files" */
static void allow_types(id panel, const char *const *filters)
{
    id types = msg(CLS("NSMutableArray"), "array");
    for (int idx = 0; filters && filters[idx] && filters[idx + 1]; idx += 2)
    {
        const char *at = filters[idx + 1];
        while (*at)
        {
            size_t len = strcspn(at, ";");
            if (len > 2 && at[0] == '*' && at[1] == '.' && at[2] != '*')
            {
                char ext[32];
                snprintf(ext, sizeof(ext), "%.*s", (int)(len - 2), at + 2);
                id name = nsstr(ext);
                if (!msg1_is(types, "containsObject:", name))
                    msg1(types, "addObject:", name);
            }
            at += len + (at[len] == ';');
        }
    }
    if ((ip)msg(types, "count"))
        msg1(panel, "setAllowedFileTypes:", types);
}

static char *run_panel(host_win *owner, id panel, const char *title, const char *const *filters)
{
    allow_types(panel, filters);
    msg1(panel, "setTitle:", nsstr(title));
    msg1(panel, "setMessage:", nsstr(title));   /* (newer pickers show no title) */
    if (owner)
        show_pointer(owner);
    ip answer = (ip)msg(panel, "runModal");
    if (owner)  /* AppKit leaves no window key after the panel: the keyboard would go nowhere */
        msg1(owner->window, "makeKeyAndOrderFront:", nil);
    return answer == 1 ? dup_str(msg(msg(panel, "URL"), "path")) : NULL;    /* NSModalResponseOK */
}

char *host_open_file(host_win *owner, const char *title, const char *const *filters, int *unavailable)
{
    void *pool = objc_autoreleasePoolPush();
    id panel = msg(CLS("NSOpenPanel"), "openPanel");
    msg1(panel, "setCanChooseDirectories:", NO);
    msg1(panel, "setAllowsMultipleSelection:", NO);
    char *path = run_panel(owner, panel, title, filters);
    objc_autoreleasePoolPop(pool);
    if (unavailable)
        *unavailable = 0;
    return path;
}

/* `name` may be a whole path: the picker then starts in its folder */
char *host_save_file(host_win *owner, const char *title, const char *name, const char *const *filters, int *unavailable)
{
    void *pool = objc_autoreleasePoolPush();
    id panel = msg(CLS("NSSavePanel"), "savePanel");
    const char *slash = name ? strrchr(name, '/') : NULL;
    if (slash && slash > name)
    {
        char *dir = strndup(name, (size_t)(slash - name));
        msg1(panel, "setDirectoryURL:", msg2(CLS("NSURL"), "fileURLWithPath:isDirectory:", nsstr(dir), YES));
        free(dir);
    }
    msg1(panel, "setNameFieldStringValue:", nsstr(slash ? slash + 1 : name));
    msg1(panel, "setAllowsOtherFileTypes:", YES);
    msg1(panel, "setCanCreateDirectories:", YES);
    char *path = run_panel(owner, panel, title, filters);
    objc_autoreleasePoolPop(pool);
    if (unavailable)
        *unavailable = 0;
    return path;
}

/* A web address, or a file or folder (a path): the Finder shows a folder */
void host_open_url(const char *url)
{
    void *pool = objc_autoreleasePoolPush();
    id u = url[0] == '/' ? msg1(CLS("NSURL"), "fileURLWithPath:", nsstr(url)) : msg1(CLS("NSURL"), "URLWithString:", nsstr(url));
    if (u)
        msg1(msg(CLS("NSWorkspace"), "sharedWorkspace"), "openURL:", u);
    objc_autoreleasePoolPop(pool);
}
