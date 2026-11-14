/*
 * Paint (C port) - the platform host on X11 (ui/host.h), spoken over the socket
 *
 * The window's surface is a MIT-SHM segment the server reads from: the app
 * draws straight into it, ShmPutImage shows the dirty part, and the next
 * frame waits for the server's ShmCompletion before touching it again. Without
 * SHM (a remote display) it is plain memory sent in PutImage strips. The
 * window manager learns what it needs from ICCCM and EWMH properties.
 *
 * Keys go through the core keyboard map (GetKeyboardMapping): Shift, Caps
 * Lock, Num Lock and AltGr's third level. There is no input method (XIM):
 * dead keys and compose sequences type nothing. The clipboard and XDND drops
 * are ICCCM selections, with INCR for big transfers both ways; while waiting
 * for another program's answer the clipboard is still served, and other
 * events wait in the queue. The Open/Save pickers are in portal_linux.c.
 */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include "host/x11_wire.h"
#include "os/os.h"
#include "ui/host.h"

#define EVENT_MASK (1 | 2 | 4 | 8 | 0x10 | 0x20 | 0x40 | 0x8000 | 0x20000 | 0x200000 | 0x400000)
#define WM_MASK (0x80000 | 0x100000)    /* SubstructureNotify | SubstructureRedirect: messages to the WM */
#define DOUBLE_CLICK_MS 400
#define WAIT_MS 2000                    /* for another program's part of a clipboard transfer */

enum
{
    A_WM_PROTOCOLS, A_WM_DELETE_WINDOW, A_NET_WM_PING, A_UTF8_STRING, A_NET_WM_NAME, A_NET_WM_ICON, A_NET_WM_PID,
    A_NET_WM_STATE, A_STATE_FULLSCREEN, A_STATE_MAX_V, A_STATE_MAX_H, A_NET_WORKAREA, A_NET_CURRENT_DESKTOP,
    A_NET_SUPPORTING_WM_CHECK, A_CLIPBOARD, A_TARGETS, A_TIMESTAMP, A_INCR, A_TEXT_PLAIN, A_IMAGE_PNG, A_IMAGE_BMP,
    A_IMAGE_JPEG, A_URI_LIST, A_PAINT_SEL, A_XDND_AWARE, A_XDND_ENTER, A_XDND_POSITION, A_XDND_STATUS, A_XDND_LEAVE,
    A_XDND_DROP, A_XDND_FINISHED, A_XDND_SELECTION, A_XDND_TYPE_LIST, A_XDND_COPY, ATOMS
};

static const char *const atom_names[ATOMS] = {
    "WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_PING", "UTF8_STRING", "_NET_WM_NAME", "_NET_WM_ICON", "_NET_WM_PID",
    "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ",
    "_NET_WORKAREA", "_NET_CURRENT_DESKTOP", "_NET_SUPPORTING_WM_CHECK", "CLIPBOARD", "TARGETS", "TIMESTAMP", "INCR",
    "text/plain;charset=utf-8", "image/png", "image/bmp", "image/jpeg", "text/uri-list", "PAINT_SELECTION",
    "XdndAware", "XdndEnter", "XdndPosition", "XdndStatus", "XdndLeave", "XdndDrop", "XdndFinished", "XdndSelection",
    "XdndTypeList", "XdndActionCopy"
};

struct host_win
{
    uint32_t id, gc;
    surface surf;
    int cap_w, cap_h;                   /* the buffer's size: kept while the window fits in it */
    uint8_t *shm_addr;
    uint32_t shm_seg;
    int mapped, focused, fullscreen, maximized, min_w, min_h;
    rect before_full;                   /* (full screen by hand, with no window manager) */
    int buttons, leave_pending;         /* held buttons; the pointer left while they were held */
    rect exposed;
    int drop_ok, drop_x, drop_y;        /* XDND: the source offers files; where (root) */
    void (*on_event)(void *user, const ui_event *ev);
    void *user;
};

static host_win *W;                     /* Paint has one window */
static uint32_t atoms[ATOMS], helper, cursors[CURSOR_BUSY + 1], last_time;
static int shm_major, shm_event, puts_pending, xfixes_event = -1, quitting, scale = 100, wake_pipe[2] = { -1, -1 };
static volatile int wake_pending;
static uint32_t *keymap;                /* keysyms_per * (max - min + 1) */
static int keysyms_per;
static unsigned alt_mask, altgr_mask, numlock_mask;
static uint8_t keys_down[32];           /* a bit per keycode, for auto-repeat */
static int mods_now;

static struct
{
    uint32_t time;
    int button, x, y, count;
} last_click;

/* What we hold on the clipboard: text or a PNG */
static char *clip_text;
static uint8_t *clip_png;
static size_t clip_png_len;
static uint32_t clip_time;
static int clip_has_image = -1;         /* another owner's TARGETS checked: 0/1, -1 not yet */
static double clip_checked;

/* INCR transfers we are sending: the next piece goes when the requestor deletes the property */
typedef struct
{
    uint32_t win, prop, type;
    const uint8_t *data;
    size_t len, at;
    double started;
} incr_send;
static incr_send sends[4];

static void handle(const uint8_t *ev);

/* ---- small helpers ---- */

static rect clip_rect(rect r, int w, int h)
{
    int x0 = r.x > 0 ? r.x : 0, y0 = r.y > 0 ? r.y : 0;
    int x1 = r.x + r.w < w ? r.x + r.w : w, y1 = r.y + r.h < h ? r.y + r.h : h;
    return (rect){ x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
}

static void change_attr(uint32_t win, uint32_t mask, uint32_t value)
{
    uint8_t *req = xw_req(2, 0, 16);
    xw_put32(req + 4, win);
    xw_put32(req + 8, mask);
    xw_put32(req + 12, value);
}

static int utf8_put(char *out, uint32_t ch)
{
    int len = ch < 0x80 ? 1 : ch < 0x800 ? 2 : ch < 0x10000 ? 3 : 4;
    static const uint8_t lead[5] = { 0, 0, 0xC0, 0xE0, 0xF0 };
    for (int idx = len - 1; idx > 0; idx--, ch >>= 6)
        out[idx] = (char)(0x80 | (ch & 63));
    out[0] = (char)(lead[len] | ch);
    return len;
}

/* Deliver an event; the surface must be free of the server first, since the handler may draw */
static void shm_settle(void);

static void put_event(ui_event ev)
{
    if (!W)
        return;
    shm_settle();
    ev.mods = ev.mods ? ev.mods : mods_now;
    ev.time = os_now();
    W->on_event(W->user, &ev);
}

/* ---- the back buffer ---- */

static int is_completion(const uint8_t *ev, void *arg)
{
    return shm_major && (ev[0] & 0x7F) == shm_event;
}

/* Wait until the server has read every ShmPutImage sent (a second at most: an error sends none) */
static void shm_settle(void)
{
    uint8_t ev[32];
    double give_up = os_now() + 1;
    while (puts_pending > 0)
    {
        if (xw_take(ev, is_completion, NULL))
            puts_pending--;
        else if (os_now() > give_up || xw_wait(100, -1) < 0)
            puts_pending = 0;
    }
}

static void release_buffer(host_win *win)
{
    shm_settle();
    if (win->shm_addr)
    {
        xw_put32(xw_req(shm_major, 2, 8) + 4, win->shm_seg);   /* ShmDetach */
        shmdt(win->shm_addr);
    }
    else
        free(win->surf.px);
    win->shm_addr = NULL;
    win->surf.px = NULL;
    win->cap_w = win->cap_h = 0;
}

/* A SysV segment the server attaches to; marked for removal at once, so it goes when both let go */
static uint8_t *shm_buffer(host_win *win, size_t bytes)
{
    int id = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
    uint8_t *addr = id >= 0 ? shmat(id, NULL, 0) : (void *)-1;
    if (addr == (void *)-1)
    {
        if (id >= 0)
            shmctl(id, IPC_RMID, NULL);
        return NULL;
    }
    win->shm_seg = xw_new_id();
    uint8_t *req = xw_req(shm_major, 1, 16);    /* ShmAttach, read-only */
    xw_put32(req + 4, win->shm_seg);
    xw_put32(req + 8, (uint32_t)id);
    req[12] = 1;
    int failed = xw_sync();
    shmctl(id, IPC_RMID, NULL);
    if (failed)
    {   /* (a server that cannot see our memory, e.g. in another container) */
        shmdt(addr);
        return NULL;
    }
    return addr;
}

/* Fit the buffer to the window: a new one when it grows past it, or is a quarter of it */
static void size_buffer(host_win *win, int w, int h)
{
    w = w > 0 ? w : 1;
    h = h > 0 ? h : 1;
    if (w > win->cap_w || h > win->cap_h || (size_t)w * (size_t)h * 4 < (size_t)win->cap_w * (size_t)win->cap_h)
    {
        release_buffer(win);
        int cap_w = (w + 31) & ~31, cap_h = (h + 31) & ~31;
        size_t bytes = 4 * (size_t)cap_w * (size_t)cap_h;
        win->shm_addr = shm_major ? shm_buffer(win, bytes) : NULL;
        shm_major = win->shm_addr ? shm_major : 0;  /* it failed once: plain memory from now on */
        win->surf.px = win->shm_addr ? (uint32_t *)win->shm_addr : malloc(bytes);
        if (!win->surf.px)
        {   /* out of memory: a tiny window is better than a crash */
            cap_w = cap_h = w = h = 1;
            win->surf.px = calloc(1, 4);
        }
        win->cap_w = cap_w;
        win->cap_h = cap_h;
    }
    win->surf.w = w;
    win->surf.h = h;
    win->surf.stride = win->cap_w;
    win->surf.clip = (rect){ 0, 0, w, h };
}

surface *host_surface(host_win *win)
{
    shm_settle();
    return &win->surf;
}

void host_present(host_win *win, rect dirty)
{
    rect r = clip_rect(dirty, win->surf.w, win->surf.h);
    if (!r.w || !r.h)
        return;
    if (win->shm_addr)
    {
        uint8_t *req = xw_req(shm_major, 3, 40);    /* ShmPutImage, with a completion event */
        xw_put32(req + 4, win->id);
        xw_put32(req + 8, win->gc);
        xw_put16(req + 12, (unsigned)win->cap_w);
        xw_put16(req + 14, (unsigned)win->cap_h);
        xw_put16(req + 16, (unsigned)r.x);
        xw_put16(req + 18, (unsigned)r.y);
        xw_put16(req + 20, (unsigned)r.w);
        xw_put16(req + 22, (unsigned)r.h);
        xw_put16(req + 24, (unsigned)r.x);
        xw_put16(req + 26, (unsigned)r.y);
        req[28] = (uint8_t)xw.depth;
        req[29] = 2;    /* ZPixmap */
        req[30] = 1;
        xw_put32(req + 32, win->shm_seg);
        puts_pending++;
    }
    else
        xw_put_image(win->id, win->gc, win->surf.px + (size_t)r.y * (size_t)win->surf.stride + r.x, win->surf.stride,
                     r.x, r.y, r.w, r.h);
    xw_flush();
}

/* ---- the keyboard ---- */

static uint32_t keysym_at(int code, int col)
{
    if (!keymap || code < xw.min_keycode || code > xw.max_keycode || col >= keysyms_per)
        return 0;
    return keymap[(size_t)(code - xw.min_keycode) * (size_t)keysyms_per + (size_t)col];
}

static void load_keymap(void)
{
    int count = xw.max_keycode - xw.min_keycode + 1;
    uint8_t *req = xw_req(101, 0, 8);   /* GetKeyboardMapping */
    req[4] = (uint8_t)xw.min_keycode;
    req[5] = (uint8_t)count;
    size_t len;
    const uint8_t *reply = xw_reply(xw.seq, &len);
    free(keymap);
    keymap = NULL;
    if (reply && len >= 32 + 4 * (size_t)reply[1] * (size_t)count)
    {
        keysyms_per = reply[1];
        keymap = malloc(4 * (size_t)keysyms_per * (size_t)count);
        memcpy(keymap, reply + 32, 4 * (size_t)keysyms_per * (size_t)count);
    }
    xw_req(119, 0, 4);                  /* GetModifierMapping: which Mod bit holds Alt, AltGr, Num Lock */
    reply = xw_reply(xw.seq, &len);
    alt_mask = altgr_mask = numlock_mask = 0;
    int per = reply ? reply[1] : 0;
    for (int mod = 3; reply && mod < 8 && len >= 32 + 8 * (size_t)per; mod++)
    {
        for (int idx = 0; idx < per; idx++)
        {
            for (int col = 0, code = reply[32 + mod * per + idx]; code && col < keysyms_per; col++)
            {
                uint32_t sym = keysym_at(code, col);
                alt_mask |= sym == 0xFFE9 || sym == 0xFFEA ? 1u << mod : 0;     /* Alt_L, Alt_R */
                altgr_mask |= sym == 0xFE03 || sym == 0xFF7E ? 1u << mod : 0;   /* ISO_Level3_Shift, Mode_switch */
                numlock_mask |= sym == 0xFF7F ? 1u << mod : 0;
            }
        }
    }
    alt_mask = alt_mask ? alt_mask : 8;     /* Mod1 */
}

/* Is up the capital of low (both Unicode)? For Caps Lock, which shifts letters only */
static int capital_of(uint32_t up, uint32_t low)
{
    if ((low >= 'a' && low <= 'z') || (low >= 0xE0 && low <= 0xFE && low != 0xF7) || (low >= 0x3B1 && low <= 0x3CB) ||
        (low >= 0x430 && low <= 0x44F))
        return up == low - 32;
    if ((low >= 0x100 && low <= 0x24F) || (low >= 0x460 && low <= 0x52F))
        return up == low - 1;
    if (low >= 0x450 && low <= 0x45F)
        return up == low - 80;
    return (low == 0xFF && up == 0x178) || (low == 0x3AC && up == 0x386) || (low >= 0x3AD && low <= 0x3AF && up == low - 37) ||
           (low == 0x3CC && up == 0x38C) || (low >= 0x3CD && low <= 0x3CE && up == low - 63);
}

/* The keysym a key gives in a modifier state, by the core protocol's rules: AltGr picks the third
 * level (or the second group), Num Lock flips Shift on the keypad and Caps Lock on letters */
static uint32_t lookup(int code, unsigned state)
{
    int col = 0;
    if (state & altgr_mask)
    {
        if (keysym_at(code, 4))
            col = 4;
        else if (keysym_at(code, 2) && (keysym_at(code, 2) != keysym_at(code, 0) || keysym_at(code, 3) != keysym_at(code, 1)))
            col = 2;
    }
    uint32_t lower = keysym_at(code, col), upper = keysym_at(code, col + 1);
    int shift = state & 1;
    upper = upper ? upper : lower;
    if (state & numlock_mask && upper >= 0xFF80 && upper <= 0xFFBD)
        shift = !shift;
    else if (state & 2 && capital_of(keysym_unicode(upper), keysym_unicode(lower)))
        shift = !shift;
    return shift ? upper : lower;
}

static const uint16_t special_keys[][2] = {
    { 0xFF08, KEY_BACKSPACE }, { 0xFF09, KEY_TAB }, { 0xFE20, KEY_TAB }, { 0xFF0D, KEY_ENTER }, { 0xFF8D, KEY_ENTER },
    { 0xFF1B, KEY_ESCAPE }, { 0xFFFF, KEY_DELETE }, { 0xFF9F, KEY_DELETE }, { 0xFF51, KEY_LEFT }, { 0xFF96, KEY_LEFT },
    { 0xFF52, KEY_UP }, { 0xFF97, KEY_UP }, { 0xFF53, KEY_RIGHT }, { 0xFF98, KEY_RIGHT }, { 0xFF54, KEY_DOWN },
    { 0xFF99, KEY_DOWN }, { 0xFF50, KEY_HOME }, { 0xFF95, KEY_HOME }, { 0xFF57, KEY_END }, { 0xFF9C, KEY_END },
    { 0xFF55, KEY_PAGE_UP }, { 0xFF9A, KEY_PAGE_UP }, { 0xFF56, KEY_PAGE_DOWN }, { 0xFF9B, KEY_PAGE_DOWN },
    { 0xFF63, KEY_INSERT }, { 0xFF9E, KEY_INSERT }, { 0xFFAB, KEY_KP_ADD }, { 0xFFAD, KEY_KP_SUBTRACT },
    { 0xFFE1, KEY_SHIFT }, { 0xFFE2, KEY_SHIFT }, { 0xFFE3, KEY_CTRL }, { 0xFFE4, KEY_CTRL }, { 0xFFE9, KEY_ALT },
    { 0xFFEA, KEY_ALT },
};

/* What a US keyboard has on evdev keycodes 10..61, for layouts whose keys are not Latin */
static const char us_keys[] = "1234567890-=\0\0QWERTYUIOP[]\0\0ASDFGHJKL;'`\0\\ZXCVBNM,./";

static int printable_key(uint32_t sym)
{
    return sym >= 0x20 && sym < 0x7F ? (sym >= 'a' && sym <= 'z' ? (int)sym - 32 : (int)sym) : 0;
}

/* host.h's key: special keys by keysym, others as the unshifted ASCII character */
static int key_of(int code, unsigned state)
{
    uint32_t sym = keysym_at(code, 0), now = lookup(code, state);
    if (now >= 0xFF80 && now <= 0xFFBD)
        sym = now;      /* the keypad, as Num Lock makes it */
    if (sym >= 0xFFBE && sym <= 0xFFC9)
        return KEY_F1 + (int)(sym - 0xFFBE);
    for (size_t idx = 0; idx < sizeof(special_keys) / sizeof(*special_keys); idx++)
    {
        if (special_keys[idx][0] == sym)
            return special_keys[idx][1];
    }
    int key = printable_key(keysym_unicode(sym) < 0x80 ? keysym_unicode(sym) : 0);
    key = key ? key : printable_key(keysym_at(code, 2));    /* a Latin second layout */
    if (!key && code >= 10 && code < 10 + (int)sizeof(us_keys) - 1)
        key = (unsigned char)us_keys[code - 10];
    return key;
}

static int mods_of(unsigned state)
{
    return (state & 1 ? KMOD_SHIFT : 0) | (state & 4 ? KMOD_CTRL : 0) | (state & alt_mask ? KMOD_ALT : 0);
}

static void key_event(const uint8_t *ev)
{
    int code = ev[1], press = (ev[0] & 0x7F) == XE_KEY_PRESS, down = keys_down[code / 8] >> (code % 8) & 1;
    unsigned state = xw_get16(ev + 28);
    int key = key_of(code, state), mods = mods_of(state);
    int bit = key == KEY_SHIFT ? KMOD_SHIFT : key == KEY_CTRL ? KMOD_CTRL : key == KEY_ALT ? KMOD_ALT : 0;
    mods_now = press ? mods_now | bit : mods_now & ~bit;    /* (the state is from before the event) */
    if (!press)
    {   /* auto-repeat sends a release and a press with the same time: no release then */
        xw_read();
        const uint8_t *next = xw_peek();
        if (next && (next[0] & 0x7F) == XE_KEY_PRESS && next[1] == code && xw_get32(next + 4) - xw_get32(ev + 4) <= 1)
            return;
        keys_down[code / 8] &= (uint8_t)~(1u << (code % 8));
        if (key)
            put_event((ui_event){ .type = EV_KEY_UP, .key = key, .mods = mods });
        return;
    }
    keys_down[code / 8] |= (uint8_t)(1u << (code % 8));
    if (key)
        put_event((ui_event){ .type = EV_KEY, .key = key, .repeat = down, .mods = mods });
    uint32_t ch = keysym_unicode(lookup(code, state));
    if (ch >= 0x20 && ch != 0x7F && !(mods & (KMOD_CTRL | KMOD_ALT)))
    {
        char text[8];
        text[utf8_put(text, ch)] = 0;
        put_event((ui_event){ .type = EV_TEXT, .text = text, .mods = mods });
    }
}

/* ---- the clipboard ---- */

static int is_background(const uint8_t *ev, void *arg);
static void serve(const uint8_t *ev);

typedef struct
{
    int type;
    uint32_t win, atom;
} wanted;

static int is_wanted(const uint8_t *ev, void *arg)
{
    const wanted *want = arg;
    int type = ev[0] & 0x7F;
    if (type == XE_SELECTION_NOTIFY && want->type == type)
        return xw_get32(ev + 8) == want->win;
    return type == XE_PROPERTY && want->type == type && xw_get32(ev + 4) == want->win && xw_get32(ev + 8) == want->atom &&
           ev[16] == 0;     /* NewValue */
}

/* Wait for an event while the clipboard is served (other events stay queued): 1, or 0 after ms */
static int wait_for(const wanted *want, int ms, uint8_t ev[32])
{
    double until = os_now() + ms / 1000.0;
    for (;;)
    {
        uint8_t other[32];
        while (xw_take(other, is_background, NULL))
            serve(other);
        if (xw_take(ev, is_wanted, (void *)want))
            return 1;
        int left = (int)((until - os_now()) * 1000);
        if (left <= 0 || xw_wait(left, -1) < 0)
            return 0;
    }
}

/* A selection converted to target in our helper's property, whole or by INCR: malloc'd, or NULL if refused */
static uint8_t *convert(uint32_t selection, uint32_t target, uint32_t time, size_t *len)
{
    uint8_t *req = xw_req(24, 0, 24), ev[32];
    uint32_t prop = atoms[A_PAINT_SEL], type;
    xw_put32(req + 4, helper);
    xw_put32(req + 8, selection);
    xw_put32(req + 12, target);
    xw_put32(req + 16, prop);
    xw_put32(req + 20, time);
    if (!wait_for(&(wanted){ XE_SELECTION_NOTIFY, helper, 0 }, WAIT_MS, ev) || !xw_get32(ev + 20))
        return NULL;
    while (xw_take(ev, is_wanted, &(wanted){ XE_PROPERTY, helper, prop }))
        continue;   /* (the owner's writing it, already read) */
    uint8_t *data = xw_get_prop(helper, prop, 0, 1, len, &type);
    if (!data || type != atoms[A_INCR])
        return data;
    free(data);     /* INCR: deleting it asked for the first piece; a piece of 0 bytes ends it */
    size_t total = 0, piece;
    data = NULL;
    for (;;)
    {
        if (!wait_for(&(wanted){ XE_PROPERTY, helper, prop }, WAIT_MS, ev))
        {
            free(data);
            return NULL;
        }
        uint8_t *part = xw_get_prop(helper, prop, 0, 1, &piece, NULL);
        if (part && !piece)
        {
            free(part);
            *len = total;
            return data ? data : calloc(1, 1);
        }
        if (part)
        {
            uint8_t *grown = realloc(data, total + piece + 1);
            if (!grown)
            {
                free(part);
                free(data);
                return NULL;
            }
            data = grown;
            memcpy(data + total, part, piece);
            total += piece;
            data[total] = 0;
            free(part);
        }
    }
}

static uint32_t clipboard_owner(void)
{
    xw_req32(23, atoms[A_CLIPBOARD]);   /* GetSelectionOwner */
    const uint8_t *reply = xw_reply(xw.seq, NULL);
    return reply ? xw_get32(reply + 8) : 0;
}

static void take_clipboard(char *text, uint8_t *png, size_t png_len)
{
    free(clip_text);
    free(clip_png);
    clip_text = text;
    clip_png = png;
    clip_png_len = png_len;
    memset(sends, 0, sizeof(sends));    /* (a transfer of the old contents is cut off) */
    clip_time = last_time;
    uint8_t *req = xw_req(22, 0, 16);   /* SetSelectionOwner */
    xw_put32(req + 4, helper);
    xw_put32(req + 8, atoms[A_CLIPBOARD]);
    xw_put32(req + 12, clip_time);
    xw_flush();
}

void host_clip_set_text(const char *utf8)
{
    take_clipboard(strdup(utf8 ? utf8 : ""), NULL, 0);
}

void host_clip_set_image(const img *im)
{
    size_t len = 0;
    uint8_t *png = img_png(im, &len);
    if (png)
        take_clipboard(NULL, png, len);
}

char *host_clip_text(void)
{
    uint32_t owner = clipboard_owner();
    size_t len;
    if (owner == helper)
        return clip_text ? strdup(clip_text) : NULL;
    if (!owner)
        return NULL;
    char *text = (char *)convert(atoms[A_CLIPBOARD], atoms[A_UTF8_STRING], last_time, &len);
    if (text)
        return text;
    uint8_t *latin1 = convert(atoms[A_CLIPBOARD], XA_STRING, last_time, &len);
    text = latin1 ? malloc(2 * len + 1) : NULL;
    size_t at = 0;
    for (size_t idx = 0; text && idx < len; idx++)
        at += (size_t)utf8_put(text + at, latin1[idx]);
    if (text)
        text[at] = 0;
    free(latin1);
    return text;
}

/* The picture type to ask another owner for (from its TARGETS), or 0 */
static uint32_t image_target(void)
{
    size_t len;
    uint32_t best = 0;
    uint8_t *list = convert(atoms[A_CLIPBOARD], atoms[A_TARGETS], last_time, &len);
    for (int pref = A_IMAGE_JPEG; list && pref >= A_IMAGE_PNG; pref--)
    {   /* PNG, else BMP, else JPEG */
        for (size_t at = 0; at + 4 <= len; at += 4)
            best = xw_get32(list + at) == atoms[pref] ? atoms[pref] : best;
    }
    free(list);
    return best;
}

img *host_clip_image(void)
{
    uint32_t owner = clipboard_owner(), target;
    size_t len = clip_png_len;
    const char *err;
    if (owner == helper)
        return clip_png ? img_load_mem(clip_png, len, &err) : NULL;
    uint8_t *data = owner && (target = image_target()) ? convert(atoms[A_CLIPBOARD], target, last_time, &len) : NULL;
    img *im = data ? img_load_mem(data, len, &err) : NULL;
    free(data);
    return im;
}

/* Kept until XFIXES says the owner changed (without XFIXES, for a second) */
int host_clip_has_image(void)
{
    uint32_t owner;
    if (clip_png || clip_text)
        return clip_png != NULL;
    if (clip_has_image < 0 || (xfixes_event < 0 && os_now() - clip_checked > 1))
    {
        owner = clipboard_owner();
        clip_has_image = owner && owner != helper && image_target() != 0;
        clip_checked = os_now();
    }
    return clip_has_image;
}

static void send_piece(int idx)
{
    size_t piece = xw.max_req - 64, len = sends[idx].len - sends[idx].at < piece ? sends[idx].len - sends[idx].at : piece;
    xw_set_prop(sends[idx].win, sends[idx].prop, sends[idx].type, 8, sends[idx].data + sends[idx].at, len);
    sends[idx].at += len;
    if (!len)
    {   /* that was the empty piece that ends it */
        change_attr(sends[idx].win, 0x800, 0);
        sends[idx].win = 0;
    }
}

/* Put data in a requestor's property: at once, or by INCR when it is bigger than a request */
static int send_data(uint32_t requestor, uint32_t prop, uint32_t type, const void *data, size_t len)
{
    if (len < xw.max_req - 64)
    {
        xw_set_prop(requestor, prop, type, 8, data, len);
        return 1;
    }
    for (int idx = 0; idx < 4; idx++)
    {
        if (!sends[idx].win || os_now() - sends[idx].started > 10)
        {
            sends[idx] = (incr_send){ requestor, prop, type, data, len, 0, os_now() };
            change_attr(requestor, 0x800, 0x400000);    /* PropertyChange: we see it deleted */
            uint32_t size = (uint32_t)len;
            xw_set_prop(requestor, prop, atoms[A_INCR], 32, &size, 1);
            return 1;
        }
    }
    return 0;
}

static void answer(const uint8_t *ev)
{
    uint32_t requestor = xw_get32(ev + 12), selection = xw_get32(ev + 16), target = xw_get32(ev + 20);
    uint32_t prop = xw_get32(ev + 24) ? xw_get32(ev + 24) : target;     /* (an obsolete client: 0) */
    int ok = selection == atoms[A_CLIPBOARD] && (clip_text || clip_png);
    if (ok && target == atoms[A_TARGETS])
    {
        uint32_t list[5] = { atoms[A_TARGETS], atoms[A_TIMESTAMP], atoms[A_IMAGE_PNG] };
        if (clip_text)
        {
            list[2] = atoms[A_UTF8_STRING];
            list[3] = atoms[A_TEXT_PLAIN];
            list[4] = XA_STRING;
        }
        xw_set_prop(requestor, prop, XA_ATOM, 32, list, clip_text ? 5 : 3);
    }
    else if (ok && target == atoms[A_TIMESTAMP])
        xw_set_prop(requestor, prop, XA_INTEGER, 32, &clip_time, 1);
    else if (ok && clip_text && (target == atoms[A_UTF8_STRING] || target == atoms[A_TEXT_PLAIN]))
        ok = send_data(requestor, prop, target, clip_text, strlen(clip_text));
    else if (ok && clip_text && target == XA_STRING)
    {   /* Latin-1, for old programs */
        size_t len = 0;
        char *latin1 = malloc(strlen(clip_text) + 1);
        for (const uint8_t *at = (const uint8_t *)clip_text; *at; at++)
        {
            uint32_t ch = *at;
            if (ch >= 0xC0)
            {   /* a sequence: its code point, if it fits */
                int more = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1;
                ch &= 0x3F >> more;
                for (; more && (at[1] & 0xC0) == 0x80; more--)
                    ch = ch << 6 | (*++at & 0x3F);
            }
            latin1[len++] = ch < 256 ? (char)ch : '?';
        }
        ok = len < xw.max_req - 64 && send_data(requestor, prop, XA_STRING, latin1, len);
        free(latin1);   /* (more than a request's worth goes as UTF-8 only: INCR keeps a pointer to the data) */
    }
    else if (ok && clip_png && target == atoms[A_IMAGE_PNG])
        ok = send_data(requestor, prop, target, clip_png, clip_png_len);
    else
        ok = 0;
    uint8_t notify[32] = { XE_SELECTION_NOTIFY };
    memcpy(notify + 4, ev + 4, 4);      /* time */
    xw_put32(notify + 8, requestor);
    xw_put32(notify + 12, selection);
    xw_put32(notify + 16, target);
    xw_put32(notify + 20, ok ? prop : 0);
    xw_send_event(requestor, 0, notify);
    xw_flush();
}

/* What must be answered even while we wait: requests for our clipboard, and its INCR transfers */
static int is_background(const uint8_t *ev, void *arg)
{
    int type = ev[0] & 0x7F;
    return type == XE_SELECTION_REQUEST || type == XE_SELECTION_CLEAR || (type == XE_PROPERTY && ev[16] == 1) ||
           type == xfixes_event || (shm_major && type == shm_event);
}

static void serve(const uint8_t *ev)
{
    int type = ev[0] & 0x7F;
    if (type == XE_SELECTION_REQUEST)
        answer(ev);
    else if (type == XE_SELECTION_CLEAR && xw_get32(ev + 12) == atoms[A_CLIPBOARD])
    {   /* another program has copied */
        free(clip_text);
        free(clip_png);
        clip_text = NULL;
        clip_png = NULL;
        memset(sends, 0, sizeof(sends));
        clip_has_image = -1;
    }
    else if (type == XE_PROPERTY)
    {
        for (int idx = 0; idx < 4; idx++)
        {
            if (sends[idx].win && sends[idx].win == xw_get32(ev + 4) && sends[idx].prop == xw_get32(ev + 8))
                send_piece(idx);
        }
        xw_flush();
    }
    else if (type == xfixes_event)
        clip_has_image = -1;
    else if (puts_pending > 0)
        puts_pending--;     /* a ShmCompletion */
}

/* ---- drops (XDND version 5, as a target, file lists only) ---- */

char *uri_to_path(const char *uri, size_t len)
{
    if (len < 6 || strncmp(uri, "file:", 5))
        return NULL;
    const char *at = uri + 5, *end = uri + len;
    if (end - at >= 2 && at[0] == '/' && at[1] == '/')
    {   /* file://host/path: the host is ours (or empty) */
        at += 2;
        while (at < end && *at != '/')
            at++;
    }
    char *path = malloc((size_t)(end - at) + 1), *put = path;
    for (; at < end; at++)
    {
        int hi, lo;
        if (*at == '%' && end - at >= 3 && sscanf(at + 1, "%1x%1x", &hi, &lo) == 2)
        {
            *put++ = (char)(hi << 4 | lo);
            at += 2;
        }
        else
            *put++ = *at;
    }
    *put = 0;
    return path;
}

static void drop_files(char *list, int x, int y)
{
    char *paths[64];
    int count = 0;
    for (char *line = list, *next; line && *line && count < 64; line = next)
    {
        size_t len = strcspn(line, "\r\n");
        next = line[len] ? line + len + 1 : NULL;
        char *path = line[0] != '#' ? uri_to_path(line, len) : NULL;
        if (path)
            paths[count++] = path;
    }
    if (count)
        put_event((ui_event){ .type = EV_DROP, .x = x, .y = y, .files = (const char *const *)paths, .n_files = count });
    while (count > 0)
        free(paths[--count]);
}

static void drop_message(const uint8_t *ev)
{
    uint32_t type = xw_get32(ev + 8), source = xw_get32(ev + 12), reply[5] = { W->id };
    const uint8_t *data = ev + 12;  /* data.l[0..4] */
    if (type == atoms[A_XDND_ENTER])
    {
        size_t len = 12;
        uint8_t *more = xw_get32(data + 4) & 1 ? xw_get_prop(source, atoms[A_XDND_TYPE_LIST], XA_ATOM, 0, &len, NULL) : NULL;
        const uint8_t *list = more ? more : data + 8;
        W->drop_ok = 0;
        for (size_t at = 0; list && at + 4 <= len; at += 4)
            W->drop_ok |= xw_get32(list + at) == atoms[A_URI_LIST];
        free(more);
    }
    else if (type == atoms[A_XDND_POSITION])
    {
        W->drop_x = (int16_t)(xw_get32(data + 8) >> 16);
        W->drop_y = (int16_t)xw_get32(data + 8);
        reply[1] = (uint32_t)W->drop_ok;
        reply[4] = W->drop_ok ? atoms[A_XDND_COPY] : 0;
        xw_client_message(source, 0, source, atoms[A_XDND_STATUS], reply);
    }
    else if (type == atoms[A_XDND_LEAVE])
        W->drop_ok = 0;
    else if (type == atoms[A_XDND_DROP])
    {
        size_t len;
        char *files = W->drop_ok ? (char *)convert(atoms[A_XDND_SELECTION], atoms[A_URI_LIST], xw_get32(data + 8), &len) : NULL;
        uint8_t *req = xw_req(40, 0, 16);   /* TranslateCoordinates: root to the window */
        xw_put32(req + 4, xw.root);
        xw_put32(req + 8, W->id);
        xw_put16(req + 12, (unsigned)W->drop_x);
        xw_put16(req + 14, (unsigned)W->drop_y);
        const uint8_t *at = xw_reply(xw.seq, NULL);
        int x = at ? (int16_t)xw_get16(at + 12) : 0, y = at ? (int16_t)xw_get16(at + 14) : 0;
        reply[1] = files != NULL;
        reply[2] = files ? atoms[A_XDND_COPY] : 0;
        xw_client_message(source, 0, source, atoms[A_XDND_FINISHED], reply);
        xw_flush();
        if (files)
            drop_files(files, x, y);
        free(files);
        W->drop_ok = 0;
    }
}

/* ---- events ---- */

static void pointer_event(const uint8_t *ev)
{
    int type = ev[0] & 0x7F, button = ev[1], x = (int16_t)xw_get16(ev + 24), y = (int16_t)xw_get16(ev + 26);
    uint32_t time = xw_get32(ev + 4);
    int mods = mods_of(xw_get16(ev + 28)), btn = button == 1 ? BTN_LEFT : button == 3 ? BTN_RIGHT : BTN_MIDDLE;
    if (type == XE_MOTION)
    {
        put_event((ui_event){ .type = EV_MOVE, .x = x, .y = y, .mods = mods });
        return;
    }
    if (button >= 4 && button <= 7)
    {   /* the wheel: 4 up, 5 down, 6 left, 7 right */
        if (type == XE_BUTTON_PRESS)
            put_event((ui_event){ .type = EV_WHEEL, .x = x, .y = y, .mods = mods, .wheel_y = button == 4 ? 1.0f : button == 5 ? -1.0f : 0,
                                  .wheel_x = button == 6 ? -1.0f : button == 7 ? 1.0f : 0 });
        return;
    }
    if (button > 3)
        return;
    if (type == XE_BUTTON_PRESS)
    {
        int near = abs(x - last_click.x) <= 4 * scale / 100 && abs(y - last_click.y) <= 4 * scale / 100;
        int twice = last_click.count == 1 && last_click.button == button && time - last_click.time <= DOUBLE_CLICK_MS && near;
        last_click.count = twice ? 2 : 1;
        last_click.time = time;
        last_click.button = button;
        last_click.x = x;
        last_click.y = y;
        W->buttons |= 1 << button;
        put_event((ui_event){ .type = EV_DOWN, .x = x, .y = y, .button = btn, .clicks = last_click.count, .mods = mods });
        return;
    }
    W->buttons &= ~(1 << button);
    put_event((ui_event){ .type = EV_UP, .x = x, .y = y, .button = btn, .mods = mods });
    if (!W->buttons && W->leave_pending)
    {   /* it was dragged out of the window and let go there */
        W->leave_pending = 0;
        if (x < 0 || y < 0 || x >= W->surf.w || y >= W->surf.h)
            put_event((ui_event){ .type = EV_LEAVE });
    }
}

static void focus_event(int in, const uint8_t *ev)
{
    int detail = ev[1], mode = ev[8];
    if (mode == 1 || mode == 2 || detail >= 5 || in == W->focused)
        return;     /* not for grabs, nor the pointer's focus */
    W->focused = in;
    if (!in)
    {   /* their releases go elsewhere */
        memset(keys_down, 0, sizeof(keys_down));
        mods_now = 0;
    }
    put_event((ui_event){ .type = EV_FOCUS, .button = in });
}

static void handle(const uint8_t *ev)
{
    int type = ev[0] & 0x7F;
    if (is_background(ev, NULL))
    {
        serve(ev);
        return;
    }
    if (type == XE_MAPPING && ev[4] != 2)
    {
        load_keymap();
        return;
    }
    if (!W)
        return;
    if (type >= XE_KEY_PRESS && type <= XE_LEAVE)
    {
        last_time = xw_get32(ev + 4);
        mods_now = mods_of(xw_get16(ev + 28));
    }
    if (type == XE_KEY_PRESS || type == XE_KEY_RELEASE)
        key_event(ev);
    else if (type >= XE_BUTTON_PRESS && type <= XE_MOTION)
        pointer_event(ev);
    else if (type == XE_ENTER)
        W->leave_pending = 0;
    else if (type == XE_LEAVE && ev[30] != 1)
    {   /* (not when our own grab starts) */
        if (W->buttons)
            W->leave_pending = 1;
        else
            put_event((ui_event){ .type = EV_LEAVE });
    }
    else if (type == XE_FOCUS_IN || type == XE_FOCUS_OUT)
        focus_event(type == XE_FOCUS_IN, ev);
    else if (type == XE_EXPOSE)
    {   /* one event for the whole of a series */
        rect r = { (int)xw_get16(ev + 8), (int)xw_get16(ev + 10), (int)xw_get16(ev + 12), (int)xw_get16(ev + 14) };
        rect *u = &W->exposed;
        if (!u->w || !u->h)
            *u = r;
        else
        {
            int x1 = u->x + u->w > r.x + r.w ? u->x + u->w : r.x + r.w, y1 = u->y + u->h > r.y + r.h ? u->y + u->h : r.y + r.h;
            u->x = u->x < r.x ? u->x : r.x;
            u->y = u->y < r.y ? u->y : r.y;
            u->w = x1 - u->x;
            u->h = y1 - u->y;
        }
        if (!xw_get16(ev + 16))
        {
            r = W->exposed;
            W->exposed = (rect){ 0 };
            put_event((ui_event){ .type = EV_EXPOSE, .x = r.x, .y = r.y, .w = r.w, .h = r.h });
        }
    }
    else if (type == XE_CONFIGURE && xw_get32(ev + 8) == W->id)
    {
        int w = (int)xw_get16(ev + 20), h = (int)xw_get16(ev + 22);
        if (w != W->surf.w || h != W->surf.h)
        {
            size_buffer(W, w, h);
            put_event((ui_event){ .type = EV_SIZE, .w = W->surf.w, .h = W->surf.h });
        }
    }
    else if (type == XE_MAP || type == XE_UNMAP)
        W->mapped = type == XE_MAP;
    else if (type == XE_PROPERTY && xw_get32(ev + 4) == W->id && xw_get32(ev + 8) == atoms[A_NET_WM_STATE] && W->mapped)
    {   /* what the window manager made of full screen and maximized (its own keys can change them) */
        size_t len;
        uint8_t *list = xw_get_prop(W->id, atoms[A_NET_WM_STATE], XA_ATOM, 0, &len, NULL);
        W->fullscreen = W->maximized = 0;
        for (size_t at = 0; list && at + 4 <= len; at += 4)
        {
            W->fullscreen |= xw_get32(list + at) == atoms[A_STATE_FULLSCREEN];
            W->maximized |= xw_get32(list + at) == atoms[A_STATE_MAX_V];
        }
        free(list);
    }
    else if (type == XE_CLIENT_MESSAGE && xw_get32(ev + 8) == atoms[A_WM_PROTOCOLS])
    {
        if (xw_get32(ev + 12) == atoms[A_WM_DELETE_WINDOW])
            put_event((ui_event){ .type = EV_CLOSE });
        else if (xw_get32(ev + 12) == atoms[A_NET_WM_PING])
        {   /* the window manager asks if we are alive: hand it back through the root */
            uint8_t pong[32];
            memcpy(pong, ev, 32);
            pong[0] = XE_CLIENT_MESSAGE;
            xw_put32(pong + 4, xw.root);
            xw_send_event(xw.root, WM_MASK, pong);
        }
    }
    else if (type == XE_CLIENT_MESSAGE)
        drop_message(ev);
}

/* ---- start-up and the loop ---- */

/* PAINT_SCALE (percent), else the desktop's Xft.dpi (96 is 100%) to the nearest quarter */
static int detect_scale(void)
{
    const char *env = getenv("PAINT_SCALE");
    if (env && atoi(env) >= 50)
        return atoi(env);
    char *res = (char *)xw_get_prop(xw.root, XA_RESOURCE_MANAGER, XA_STRING, 0, NULL, NULL);
    char *dpi = res ? strstr(res, "Xft.dpi:") : NULL;
    double value = dpi ? atof(dpi + 8) : 96;
    free(res);
    int percent = (int)(value * 100 / 96 / 25 + 0.5) * 25;
    return percent < 100 ? 100 : percent;
}

static void make_cursors(void)
{
    /* The core cursor font's glyphs (each followed by its mask) for CURSOR_ARROW .. CURSOR_BUSY:
     * left_ptr, hand2, xterm, crosshair, fleur, sb_v_double_arrow, sb_h_double_arrow,
     * bottom_right_corner, bottom_left_corner, (none), watch */
    static const uint8_t glyphs[CURSOR_BUSY + 1] = { 68, 60, 152, 34, 52, 116, 108, 14, 12, 0, 150 };
    uint32_t font = xw_new_id(), pixmap = xw_new_id(), gc = xw_new_id();
    uint8_t *req = xw_req(45, 0, 12 + 6);   /* OpenFont "cursor" */
    xw_put32(req + 4, font);
    xw_put16(req + 8, 6);
    memcpy(req + 12, "cursor", 6);
    for (int idx = 0; idx <= CURSOR_BUSY; idx++)
    {
        cursors[idx] = xw_new_id();
        if (idx == CURSOR_NONE)
            continue;
        req = xw_req(94, 0, 32);    /* CreateGlyphCursor: black on white */
        xw_put32(req + 4, cursors[idx]);
        xw_put32(req + 8, font);
        xw_put32(req + 12, font);
        xw_put16(req + 16, glyphs[idx]);
        xw_put16(req + 18, glyphs[idx] + 1u);
        xw_put16(req + 26, 0xFFFF);
        xw_put16(req + 28, 0xFFFF);
        xw_put16(req + 30, 0xFFFF);
    }
    xw_req32(46, font);     /* CloseFont: the cursors keep it */
    /* The invisible one: a 1x1 bitmap, cleared, as its own mask */
    req = xw_req(53, 1, 16);
    xw_put32(req + 4, pixmap);
    xw_put32(req + 8, xw.root);
    xw_put16(req + 12, 1);
    xw_put16(req + 14, 1);
    req = xw_req(55, 0, 20);    /* CreateGC, foreground 0 */
    xw_put32(req + 4, gc);
    xw_put32(req + 8, pixmap);
    xw_put32(req + 12, 4);
    req = xw_req(70, 0, 20);    /* PolyFillRectangle */
    xw_put32(req + 4, pixmap);
    xw_put32(req + 8, gc);
    xw_put16(req + 16, 1);
    xw_put16(req + 18, 1);
    req = xw_req(93, 0, 32);    /* CreateCursor */
    xw_put32(req + 4, cursors[CURSOR_NONE]);
    xw_put32(req + 8, pixmap);
    xw_put32(req + 12, pixmap);
    xw_req32(60, gc);
    xw_req32(54, pixmap);
}

int host_init(void)
{
    if (W || xw.fd >= 0)
        return 0;
    if (xw_open() || pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK))
        return -1;
    xw_atoms(atom_names, ATOMS, atoms);
    helper = xw_new_id();   /* an unmapped window that holds the clipboard and receives selections */
    uint8_t *req = xw_req(1, 0, 36);
    xw_put32(req + 4, helper);
    xw_put32(req + 8, xw.root);
    xw_put16(req + 12, (unsigned)-10);
    xw_put16(req + 14, (unsigned)-10);
    xw_put16(req + 16, 1);
    xw_put16(req + 18, 1);
    xw_put16(req + 22, 2);  /* InputOnly */
    xw_put32(req + 28, 0x800);
    xw_put32(req + 32, 0x400000);   /* PropertyChange */
    const char *no_shm = getenv("PAINT_NO_SHM");
    if (xw.local && !(no_shm && atoi(no_shm)))
        shm_major = xw_ext("MIT-SHM", &shm_event);
    int xfixes = xw_ext("XFIXES", &xfixes_event);
    if (xfixes)
    {   /* to hear when another program copies */
        req = xw_req(xfixes, 0, 12);    /* QueryVersion 5.0 (required first) */
        xw_put32(req + 4, 5);
        xw_reply(xw.seq, NULL);
        req = xw_req(xfixes, 2, 16);    /* SelectSelectionInput: owner set, window destroyed, client gone */
        xw_put32(req + 4, helper);
        xw_put32(req + 8, atoms[A_CLIPBOARD]);
        xw_put32(req + 12, 7);
    }
    else
        xfixes_event = -1;
    load_keymap();
    scale = detect_scale();
    make_cursors();
    xw_flush();
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

void host_wake(void)
{
    if (!__sync_lock_test_and_set(&wake_pending, 1))
    {
        ssize_t ignored = write(wake_pipe[1], "", 1);
        (void)ignored;
    }
}

void host_run(int (*timeout_ms)(void *user), void *user)
{
    uint8_t ev[32];
    quitting = 0;
    while (!quitting)
    {
        while (!quitting && xw_take(ev, NULL, NULL))
            handle(ev);
        if (quitting)
            break;
        int wait = timeout_ms ? timeout_ms(user) : -1;
        double deadline = os_now() + wait / 1000.0;
        int ready = xw_wait(wait, wake_pipe[0]);
        if (ready < 0)
        {
            fprintf(stderr, "x11: the display connection was lost\n");
            break;
        }
        int tick = (ready & 2) || (wait >= 0 && os_now() >= deadline);
        if (ready & 2)
        {
            char drain[64];
            __sync_lock_release(&wake_pending);
            while (read(wake_pipe[0], drain, sizeof(drain)) > 0)
                continue;
        }
        while (!quitting && xw_take(ev, NULL, NULL))
            handle(ev);
        if (tick && !quitting)
            put_event((ui_event){ .type = EV_TICK });
    }
}

/* The primary monitor (RandR 1.5), else the screen, less _NET_WORKAREA's panels */
rect host_work_area(void)
{
    rect area = { 0, 0, xw.screen_w, xw.screen_h };
    int randr = xw_ext("RANDR", NULL);
    if (randr)
    {
        uint8_t *req = xw_req(randr, 0, 12);    /* QueryVersion 1.5 */
        xw_put32(req + 4, 1);
        xw_put32(req + 8, 5);
        const uint8_t *reply = xw_reply(xw.seq, NULL);
        if (reply && (xw_get32(reply + 8) > 1 || xw_get32(reply + 12) >= 5))
        {
            req = xw_req(randr, 42, 12);    /* GetMonitors, active ones */
            xw_put32(req + 4, xw.root);
            req[8] = 1;
            size_t len;
            reply = xw_reply(xw.seq, &len);
            size_t at = 32;
            for (uint32_t idx = 0, count = reply ? xw_get32(reply + 12) : 0; idx < count && at + 24 <= len; idx++)
            {
                const uint8_t *mon = reply + at;
                if (idx == 0 || mon[4])
                    area = (rect){ (int16_t)xw_get16(mon + 8), (int16_t)xw_get16(mon + 10), (int)xw_get16(mon + 12),
                                   (int)xw_get16(mon + 14) };
                if (mon[4])
                    break;
                at += 24 + 4 * (size_t)xw_get16(mon + 6);
            }
        }
    }
    size_t len;
    uint32_t desk = xw_get_prop32(xw.root, atoms[A_NET_CURRENT_DESKTOP], XA_CARDINAL, 0, 0);
    uint8_t *work = xw_get_prop(xw.root, atoms[A_NET_WORKAREA], XA_CARDINAL, 0, &len, NULL);
    if (work && len >= 16 * ((size_t)desk + 1))
    {
        const uint8_t *at = work + 16 * desk;
        rect panel = { (int)xw_get32(at), (int)xw_get32(at + 4), (int)xw_get32(at + 8), (int)xw_get32(at + 12) };
        rect cut = clip_rect((rect){ panel.x - area.x, panel.y - area.y, panel.w, panel.h }, area.w, area.h);
        if (cut.w && cut.h)
            area = (rect){ area.x + cut.x, area.y + cut.y, cut.w, cut.h };
    }
    free(work);
    return area;
}

/* ---- the window ---- */

static void write_states(host_win *win)
{
    uint32_t list[3];
    int count = 0;
    if (win->fullscreen)
        list[count++] = atoms[A_STATE_FULLSCREEN];
    if (win->maximized)
    {
        list[count++] = atoms[A_STATE_MAX_V];
        list[count++] = atoms[A_STATE_MAX_H];
    }
    xw_set_prop(win->id, atoms[A_NET_WM_STATE], XA_ATOM, 32, list, (size_t)count);
}

static void write_size_hints(host_win *win)
{
    uint32_t hints[18] = { 16 };    /* PMinSize */
    hints[5] = (uint32_t)win->min_w;
    hints[6] = (uint32_t)win->min_h;
    xw_set_prop(win->id, XA_WM_NORMAL_HINTS, XA_WM_SIZE_HINTS, 32, hints, 18);
}

host_win *host_open(const char *title, int w, int h, void (*on_event)(void *user, const ui_event *ev), void *user)
{
    host_win *win = calloc(1, sizeof(*win));
    rect area = host_work_area();
    win->on_event = on_event;
    win->user = user;
    win->id = xw_new_id();
    win->gc = xw_new_id();
    uint8_t *req = xw_req(1, xw.depth, 32 + 4 * 6);     /* CreateWindow */
    xw_put32(req + 4, win->id);
    xw_put32(req + 8, xw.root);
    xw_put16(req + 12, (unsigned)(area.x + (area.w - w) / 2));
    xw_put16(req + 14, (unsigned)(area.y + (area.h - h) / 2));
    xw_put16(req + 16, (unsigned)w);
    xw_put16(req + 18, (unsigned)h);
    xw_put16(req + 22, 1);  /* InputOutput */
    xw_put32(req + 24, xw.visual);
    /* background None (we paint it all), border 0, NorthWest bit gravity, events, colormap, cursor */
    xw_put32(req + 28, 0x1 | 0x8 | 0x10 | 0x800 | 0x2000 | 0x4000);
    uint32_t values[6] = { 0, 0, 1, EVENT_MASK, xw.colormap, cursors[CURSOR_ARROW] };
    memcpy(req + 32, values, sizeof(values));
    req = xw_req(55, 0, 20);    /* CreateGC, no graphics exposures */
    xw_put32(req + 4, win->gc);
    xw_put32(req + 8, win->id);
    xw_put32(req + 12, 0x10000);
    uint32_t protocols[2] = { atoms[A_WM_DELETE_WINDOW], atoms[A_NET_WM_PING] }, version = 5;
    uint32_t pid = (uint32_t)getpid(), wm_hints[9] = { 1 | 2, 1, 1 };     /* Input: yes; NormalState */
    char machine[256] = "";
    gethostname(machine, sizeof(machine) - 1);
    xw_set_prop(win->id, atoms[A_WM_PROTOCOLS], XA_ATOM, 32, protocols, 2);
    xw_set_prop(win->id, atoms[A_XDND_AWARE], XA_ATOM, 32, &version, 1);
    xw_set_prop(win->id, XA_WM_CLASS, XA_STRING, 8, "paint\0Paint", 12);
    xw_set_prop(win->id, XA_WM_HINTS, XA_WM_HINTS, 32, wm_hints, 9);
    xw_set_prop(win->id, XA_WM_CLIENT_MACHINE, XA_STRING, 8, machine, strlen(machine));
    xw_set_prop(win->id, atoms[A_NET_WM_PID], XA_CARDINAL, 32, &pid, 1);
    W = win;
    host_title(win, title);
    write_size_hints(win);
    size_buffer(win, w, h);
    xw_flush();
    return win;
}

void host_close(host_win *win)
{
    release_buffer(win);
    xw_req32(4, win->id);   /* DestroyWindow */
    xw_req32(60, win->gc);
    xw_flush();
    if (W == win)
        W = NULL;
    free(win);
}

void host_show(host_win *win, int maximized)
{
    win->maximized = maximized;
    if (win->mapped)
    {
        uint32_t data[5] = { (uint32_t)maximized, atoms[A_STATE_MAX_V], atoms[A_STATE_MAX_H], 1 };
        xw_client_message(xw.root, WM_MASK, win->id, atoms[A_NET_WM_STATE], data);
    }
    else
        write_states(win);
    xw_req32(8, win->id);   /* MapWindow */
    xw_flush();
}

void host_title(host_win *win, const char *title)
{
    size_t len = strlen(title), at = 0;
    char *latin1 = malloc(len + 1);
    xw_set_prop(win->id, atoms[A_NET_WM_NAME], atoms[A_UTF8_STRING], 8, title, len);
    for (const uint8_t *in = (const uint8_t *)title; *in; in++)
    {   /* WM_NAME is Latin-1: two-byte sequences up to U+00FF, '?' for the rest */
        if (*in < 0x80)
            latin1[at++] = (char)*in;
        else if (*in >= 0xC2 && *in <= 0xC3 && (in[1] & 0xC0) == 0x80)
        {
            latin1[at++] = (char)((*in & 3) << 6 | (in[1] & 0x3F));
            in++;
        }
        else if ((*in & 0xC0) != 0x80)
            latin1[at++] = '?';
    }
    xw_set_prop(win->id, XA_WM_NAME, XA_STRING, 8, latin1, at);
    free(latin1);
    xw_flush();
}

void host_min_size(host_win *win, int w, int h)
{
    win->min_w = w;
    win->min_h = h;
    write_size_hints(win);
    xw_flush();
}

/* The window manager's full screen (EWMH), else with none the screen's size by hand */
void host_fullscreen(host_win *win, int on)
{
    if (on == win->fullscreen)
        return;
    win->fullscreen = on;
    if (xw_get_prop32(xw.root, atoms[A_NET_SUPPORTING_WM_CHECK], XA_WINDOW, 0, 0))
    {
        uint32_t data[5] = { (uint32_t)on, atoms[A_STATE_FULLSCREEN], 0, 1 };
        if (win->mapped)
            xw_client_message(xw.root, WM_MASK, win->id, atoms[A_NET_WM_STATE], data);
        else
            write_states(win);
        xw_flush();
        return;
    }
    if (on)
    {
        uint8_t *req = xw_req(40, 0, 16);   /* where it is now, to go back to */
        xw_put32(req + 4, win->id);
        xw_put32(req + 8, xw.root);
        const uint8_t *reply = xw_reply(xw.seq, NULL);
        win->before_full = (rect){ reply ? (int16_t)xw_get16(reply + 12) : 0, reply ? (int16_t)xw_get16(reply + 14) : 0,
                                   win->surf.w, win->surf.h };
    }
    rect to = on ? (rect){ 0, 0, xw.screen_w, xw.screen_h } : win->before_full;
    uint8_t *req = xw_req(12, 0, 12 + 4 * 4);  /* ConfigureWindow x y w h */
    xw_put32(req + 4, win->id);
    xw_put16(req + 8, 1 | 2 | 4 | 8);
    uint32_t values[4] = { (uint32_t)to.x, (uint32_t)to.y, (uint32_t)to.w, (uint32_t)to.h };
    memcpy(req + 12, values, sizeof(values));
    xw_flush();
}

void host_cursor(host_win *win, int shape)
{
    if (shape >= 0 && shape <= CURSOR_BUSY)
    {
        change_attr(win->id, 0x4000, cursors[shape]);
        xw_flush();
    }
}

/* owner_events False: everything comes to this window, in its coordinates */
void host_capture(host_win *win, int on)
{
    if (on)
    {
        uint8_t *req = xw_req(26, 0, 24);   /* GrabPointer (its reply is not needed) */
        xw_put32(req + 4, win->id);
        xw_put16(req + 8, 4 | 8 | 0x40);
        req[10] = req[11] = 1;  /* asynchronous */
    }
    else
        xw_req32(27, 0);        /* UngrabPointer */
    xw_flush();
}

void host_ime_spot(host_win *win, int x, int y, int h)
{
    (void)win;  /* no input method on X11 here */
}

/* _NET_WM_ICON: the picture, then halves of it down to 16 pixels, as ARGB rows */
void host_icon(host_win *win, const img *icon)
{
    size_t total = 0;
    for (int w = icon->w, h = icon->h; w >= 16 && h >= 16; w /= 2, h /= 2)
        total += 2 + (size_t)w * (size_t)h;
    uint32_t *prop = malloc(4 * (total + 2 + (size_t)icon->w * (size_t)icon->h)), *at = prop;
    img level = *icon;
    uint8_t *half = NULL;
    for (int first = 1; first || (level.w >= 16 && level.h >= 16); first = 0)
    {
        *at++ = (uint32_t)level.w;
        *at++ = (uint32_t)level.h;
        for (int idx = 0; idx < level.w * level.h; idx++)
        {
            const uint8_t *px = level.px + 4 * (size_t)idx;
            *at++ = (uint32_t)px[3] << 24 | (uint32_t)px[0] << 16 | (uint32_t)px[1] << 8 | px[2];
        }
        int hw = level.w / 2, hh = level.h / 2;
        if (hw < 16 || hh < 16)
            break;
        uint8_t *next = malloc(4 * (size_t)hw * (size_t)hh);
        for (int y = 0; y < hh; y++)
        {   /* a 2x2 box, weighted by alpha */
            for (int x = 0; x < hw; x++)
            {
                const uint8_t *p[4] = { img_at(&level, 2 * x, 2 * y), img_at(&level, 2 * x + 1, 2 * y),
                                        img_at(&level, 2 * x, 2 * y + 1), img_at(&level, 2 * x + 1, 2 * y + 1) };
                uint8_t *out = next + 4 * ((size_t)y * (size_t)hw + (size_t)x);
                int a = p[0][3] + p[1][3] + p[2][3] + p[3][3];
                for (int c = 0; c < 3; c++)
                    out[c] = a ? (uint8_t)((p[0][c] * p[0][3] + p[1][c] * p[1][3] + p[2][c] * p[2][3] + p[3][c] * p[3][3]) / a) : 0;
                out[3] = (uint8_t)(a / 4);
            }
        }
        free(half);
        half = next;
        level = (img){ hw, hh, half };
    }
    free(half);
    xw_set_prop(win->id, atoms[A_NET_WM_ICON], XA_CARDINAL, 32, prop, (size_t)(at - prop));
    free(prop);
    xw_flush();
}

/* ---- for portal_linux.c ---- */

uint32_t x11_host_window(void)
{
    return W ? W->id : 0;
}

static int is_input(const uint8_t *ev, void *arg)
{
    int type = ev[0] & 0x7F;
    return type >= XE_KEY_PRESS && type <= XE_LEAVE;
}

static int is_expose(const uint8_t *ev, void *arg)
{
    return (ev[0] & 0x7F) == XE_EXPOSE;
}

int x11_modal_wait(int fd, int timeout_ms)
{
    double until = os_now() + timeout_ms / 1000.0;
    uint8_t ev[32];
    if (xw.fd < 0)
    {
        struct pollfd pfd = { fd, POLLIN, 0 };
        return poll(&pfd, 1, timeout_ms) > 0;
    }
    xw_req32(27, 0);    /* UngrabPointer: the click that opened the picker may hold it */
    for (;;)
    {
        while (xw_take(ev, is_background, NULL))
            serve(ev);
        while (xw_take(ev, is_input, NULL))
            continue;
        while (W && xw_take(ev, is_expose, NULL))
        {   /* repaint from the back buffer, which holds the last frame */
            rect r = { (int)xw_get16(ev + 8), (int)xw_get16(ev + 10), (int)xw_get16(ev + 12), (int)xw_get16(ev + 14) };
            host_present(W, r);
        }
        int left = timeout_ms < 0 ? -1 : (int)((until - os_now()) * 1000);
        if (timeout_ms >= 0 && left <= 0)
            return 0;
        int ready = xw_wait(left, fd);
        if (ready < 0)
            return -1;
        if (ready & 2)
            return 1;
    }
}
