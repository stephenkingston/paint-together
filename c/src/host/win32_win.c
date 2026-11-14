/*
 * Paint (C port) - the platform host on Windows (ui/host.h): Win32 and GDI
 *
 * The window's surface IS a top-down 32-bit DIB section: the app draws into
 * its bits and host_present BitBlts the dirty part to the window; WM_PAINT
 * does the same from the last frame, so the app never sees an expose here.
 * Built for XP and later: anything newer (per-monitor DPI, horizontal wheel)
 * is looked up at run time or simply never arrives on old systems.
 *
 * Win32 calls back into the window procedure from inside calls the app makes
 * (SetWindowPos sends WM_SIZE at once, a file picker runs its own message
 * loop). So events met while the app is still handling one are held and
 * delivered after it returns, and while a picker is open the window only
 * repaints; input waits for the picker to close.
 */
#include <windows.h>
#include <commdlg.h>
#include <imm.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "os/os.h"
#include "ui/host.h"

#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef MAPVK_VK_TO_CHAR
#define MAPVK_VK_TO_CHAR 2
#endif
#define WM_PAINT_WAKE (WM_APP + 1)

struct host_win
{
    HWND hwnd;
    HDC mem;                    /* holds the DIB section, for BitBlt */
    HBITMAP dib, old_bitmap;
    surface surf;
    int cap_w, cap_h;           /* the DIB's size: kept while the window fits in it */
    int min_w, min_h, fullscreen, inside, buttons;     /* inside: the pointer, as far as we told the app */
    DWORD style_before;
    WINDOWPLACEMENT placement;  /* from before full screen */
    HICON big_icon, small_icon;
    WCHAR high_surrogate;
    void (*on_event)(void *user, const ui_event *ev);
    void *user;
};

static host_win *W;             /* Paint has one window */
static HINSTANCE instance;
static DWORD main_thread;
static volatile LONG wake_pending;
static int quitting, scale = 100, dispatching, modal, tick_pending;
static HCURSOR cursor;
static ui_event held[16];       /* events met while the app was busy (no pointers in them) */
static int n_held;

static const WCHAR class_name[] = L"PaintWindow";

/* ---- text ---- */

static WCHAR *to_wide(const char *utf8)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    WCHAR *wide = malloc(sizeof(WCHAR) * (size_t)(len > 0 ? len : 1));
    if (len <= 0 || !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, len))
        wide[0] = 0;
    return wide;
}

static char *to_utf8(const WCHAR *wide)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *utf8 = malloc(len > 0 ? (size_t)len : 1);
    if (len <= 0 || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, len, NULL, NULL))
        utf8[0] = 0;
    return utf8;
}

/* ---- events ---- */

static int mods_now(void)
{
    int mods = (GetKeyState(VK_SHIFT) < 0 ? KMOD_SHIFT : 0) | (GetKeyState(VK_CONTROL) < 0 ? KMOD_CTRL : 0) |
               (GetKeyState(VK_MENU) < 0 ? KMOD_ALT : 0);
    if (GetKeyState(VK_RMENU) < 0 && GetKeyState(VK_LCONTROL) < 0)
        mods &= ~(KMOD_CTRL | KMOD_ALT);    /* AltGr, which Windows sends as Ctrl + right Alt */
    return mods;
}

static void put_event(ui_event ev)
{
    if (!W)
        return;
    ev.mods = ev.mods ? ev.mods : mods_now();
    ev.time = os_now();
    if (modal || dispatching)
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
    while (n_held || tick_pending)
    {
        ui_event next = n_held ? held[0] : (ui_event){ .type = EV_TICK, .mods = mods_now(), .time = os_now() };
        if (n_held)
            memmove(held, held + 1, sizeof(*held) * (size_t)--n_held);
        else
            tick_pending = 0;
        W->on_event(W->user, &next);
    }
    dispatching = 0;
}

/* ---- the back buffer ---- */

static void release_buffer(host_win *win)
{
    if (win->mem)
    {
        SelectObject(win->mem, win->old_bitmap);
        DeleteObject(win->dib);
        DeleteDC(win->mem);
    }
    win->mem = NULL;
    win->dib = NULL;
    win->surf.px = NULL;
    win->cap_w = win->cap_h = 0;
}

/* Fit the DIB to the window: a new one when it grows past it, or is a quarter of it */
static void size_buffer(host_win *win, int w, int h)
{
    w = w > 0 ? w : 1;
    h = h > 0 ? h : 1;
    if (w > win->cap_w || h > win->cap_h || (size_t)w * (size_t)h * 4 < (size_t)win->cap_w * (size_t)win->cap_h)
    {
        release_buffer(win);
        int cap_w = (w + 31) & ~31, cap_h = (h + 31) & ~31;
        BITMAPINFO info = { { sizeof(BITMAPINFOHEADER), cap_w, -cap_h, 1, 32, BI_RGB } };   /* top-down */
        void *bits = NULL;
        win->dib = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!win->dib)
        {   /* out of memory: a tiny window is better than a crash */
            cap_w = cap_h = w = h = 1;
            info.bmiHeader.biWidth = 1;
            info.bmiHeader.biHeight = -1;
            win->dib = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
        }
        win->mem = CreateCompatibleDC(NULL);
        win->old_bitmap = SelectObject(win->mem, win->dib);
        win->surf.px = bits;
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
    GdiFlush();
    return &win->surf;
}

void host_present(host_win *win, rect dirty)
{
    int x0 = dirty.x > 0 ? dirty.x : 0, y0 = dirty.y > 0 ? dirty.y : 0;
    int x1 = dirty.x + dirty.w < win->surf.w ? dirty.x + dirty.w : win->surf.w;
    int y1 = dirty.y + dirty.h < win->surf.h ? dirty.y + dirty.h : win->surf.h;
    if (x1 <= x0 || y1 <= y0)
        return;
    HDC dc = GetDC(win->hwnd);
    BitBlt(dc, x0, y0, x1 - x0, y1 - y0, win->mem, x0, y0, SRCCOPY);
    ReleaseDC(win->hwnd, dc);
}

/* ---- DPI ---- */

typedef BOOL(WINAPI *awareness_context_fn)(HANDLE);
typedef HRESULT(WINAPI *awareness_fn)(int);
typedef BOOL(WINAPI *aware_fn)(void);
typedef UINT(WINAPI *dpi_for_window_fn)(HWND);
typedef BOOL(WINAPI *adjust_for_dpi_fn)(RECT *, DWORD, BOOL, DWORD, UINT);
typedef BOOL(WINAPI *nonclient_fn)(HWND);

static dpi_for_window_fn dpi_for_window;
static adjust_for_dpi_fn adjust_for_dpi;

static FARPROC user32(const char *name)
{
    return GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
}

/* Per-monitor (v2 on Windows 10, else 8.1's), else system-wide (Vista); XP has none of them */
static void dpi_aware(void)
{
    awareness_context_fn context = (awareness_context_fn)(void *)user32("SetProcessDpiAwarenessContext");
    HMODULE shcore = context ? NULL : LoadLibraryW(L"shcore.dll");
    awareness_fn awareness = shcore ? (awareness_fn)(void *)GetProcAddress(shcore, "SetProcessDpiAwareness") : NULL;
    aware_fn aware = (aware_fn)(void *)user32("SetProcessDPIAware");
    if (!(context && context((HANDLE)-4)) && !(awareness && awareness(2) == S_OK) && aware)
        aware();
    dpi_for_window = (dpi_for_window_fn)(void *)user32("GetDpiForWindow");
    adjust_for_dpi = (adjust_for_dpi_fn)(void *)user32("AdjustWindowRectExForDpi");
}

static int scale_of(int dpi)
{
    int percent = (dpi * 100 / 96 + 12) / 25 * 25;
    return percent < 100 ? 100 : percent;
}

static int window_dpi(HWND hwnd)
{
    if (hwnd && dpi_for_window)
        return (int)dpi_for_window(hwnd);
    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return dpi;
}

/* The outer size of a window whose client area is w x h */
static SIZE frame_size(HWND hwnd, int w, int h, DWORD style)
{
    RECT r = { 0, 0, w, h };
    if (adjust_for_dpi)
        adjust_for_dpi(&r, style, FALSE, 0, (UINT)window_dpi(hwnd));
    else
        AdjustWindowRectEx(&r, style, FALSE, 0);
    return (SIZE){ r.right - r.left, r.bottom - r.top };
}

/* ---- the keyboard ---- */

typedef struct
{
    WORD vk;
    short key;
} vk_key;

static int find_key(const vk_key *list, size_t count, WPARAM vk)
{
    for (size_t idx = 0; idx < count; idx++)
    {
        if (list[idx].vk == vk)
            return list[idx].key;
    }
    return 0;
}

static int key_of(WPARAM vk)
{
    static const vk_key keys[] = {
        { VK_BACK, KEY_BACKSPACE }, { VK_TAB, KEY_TAB }, { VK_RETURN, KEY_ENTER }, { VK_ESCAPE, KEY_ESCAPE },
        { VK_SPACE, KEY_SPACE }, { VK_DELETE, KEY_DELETE }, { VK_LEFT, KEY_LEFT }, { VK_RIGHT, KEY_RIGHT },
        { VK_UP, KEY_UP }, { VK_DOWN, KEY_DOWN }, { VK_HOME, KEY_HOME }, { VK_END, KEY_END }, { VK_PRIOR, KEY_PAGE_UP },
        { VK_NEXT, KEY_PAGE_DOWN }, { VK_INSERT, KEY_INSERT }, { VK_ADD, KEY_KP_ADD }, { VK_SUBTRACT, KEY_KP_SUBTRACT },
        { VK_SHIFT, KEY_SHIFT }, { VK_CONTROL, KEY_CTRL }, { VK_MENU, KEY_ALT }, { VK_MULTIPLY, '*' },
        { VK_DIVIDE, '/' }, { VK_DECIMAL, '.' },
    };
    static const vk_key us_keys[] = {   /* where a layout's character is not ASCII: the key's place on a US keyboard */
        { VK_OEM_1, ';' }, { VK_OEM_PLUS, '=' }, { VK_OEM_COMMA, ',' }, { VK_OEM_MINUS, '-' }, { VK_OEM_PERIOD, '.' },
        { VK_OEM_2, '/' }, { VK_OEM_3, '`' }, { VK_OEM_4, '[' }, { VK_OEM_5, '\\' }, { VK_OEM_6, ']' }, { VK_OEM_7, '\'' },
    };
    if (vk >= VK_F1 && vk <= VK_F12)
        return KEY_F1 + (int)(vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return '0' + (int)(vk - VK_NUMPAD0);
    int key = find_key(keys, sizeof(keys) / sizeof(*keys), vk);
    if (key)
        return key;
    UINT ch = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0x7FFFFFFF;    /* unshifted; the top bit: a dead key */
    if (ch > ' ' && ch < 0x7F)
        return ch >= 'a' && ch <= 'z' ? (int)ch - 32 : (int)ch;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return (int)vk;
    return find_key(us_keys, sizeof(us_keys) / sizeof(*us_keys), vk);
}

static void char_event(host_win *win, WPARAM unit)
{
    WCHAR text16[3] = { (WCHAR)unit };
    if (unit >= 0xD800 && unit <= 0xDBFF)
    {   /* the first half of a pair: wait for the second */
        win->high_surrogate = (WCHAR)unit;
        return;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF)
    {
        if (!win->high_surrogate)
            return;
        text16[0] = win->high_surrogate;
        text16[1] = (WCHAR)unit;
    }
    win->high_surrogate = 0;
    if (unit < ' ' || unit == 0x7F || (mods_now() & KMOD_CTRL))
        return;     /* control characters, and Ctrl's chords */
    char *text = to_utf8(text16);
    put_event((ui_event){ .type = EV_TEXT, .text = text });
    free(text);
}

/* ---- the window procedure ---- */

static void pointer_down(host_win *win, int button, int clicks, LPARAM lp)
{
    win->buttons |= 1 << button;
    put_event((ui_event){ .type = EV_DOWN, .x = (short)LOWORD(lp), .y = (short)HIWORD(lp), .button = button, .clicks = clicks });
}

static void pointer_up(host_win *win, int button, LPARAM lp)
{
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    win->buttons &= ~(1 << button);
    put_event((ui_event){ .type = EV_UP, .x = x, .y = y, .button = button });
    if (!win->buttons && win->inside && (x < 0 || y < 0 || x >= win->surf.w || y >= win->surf.h))
    {   /* let go outside: it has left (WM_MOUSELEAVE waits for the capture to end) */
        win->inside = 0;
        put_event((ui_event){ .type = EV_LEAVE });
    }
}

static void wheel(host_win *win, WPARAM wp, LPARAM lp, int horizontal)
{
    POINT at = { (short)LOWORD(lp), (short)HIWORD(lp) };
    float notches = (float)(short)HIWORD(wp) / WHEEL_DELTA;
    ScreenToClient(win->hwnd, &at);
    put_event((ui_event){ .type = EV_WHEEL, .x = at.x, .y = at.y, .wheel_x = horizontal ? notches : 0, .wheel_y = horizontal ? 0 : notches });
}

static void drop_files(host_win *win, HDROP drop)
{
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    char **paths = calloc(count ? count : 1, sizeof(char *));
    POINT at = { 0, 0 };
    DragQueryPoint(drop, &at);
    for (UINT idx = 0; idx < count; idx++)
    {
        UINT len = DragQueryFileW(drop, idx, NULL, 0);
        WCHAR *path = malloc(sizeof(WCHAR) * (len + 1));
        DragQueryFileW(drop, idx, path, len + 1);
        paths[idx] = to_utf8(path);
        free(path);
    }
    DragFinish(drop);
    if (count)
        put_event((ui_event){ .type = EV_DROP, .x = at.x, .y = at.y, .files = (const char *const *)paths, .n_files = (int)count });
    for (UINT idx = 0; idx < count; idx++)
        free(paths[idx]);
    free(paths);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    host_win *win = W && W->hwnd == hwnd ? W : NULL;
    if (msg == WM_NCCREATE)
    {   /* per-monitor v1 (Windows 10 1607) scales the title bar only when asked */
        nonclient_fn nonclient = (nonclient_fn)(void *)user32("EnableNonClientDpiScaling");
        if (nonclient)
            nonclient(hwnd);
    }
    if (!win)
        return DefWindowProcW(hwnd, msg, wp, lp);
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
               win->mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && (x != win->surf.w || y != win->surf.h) && x > 0 && y > 0)
        {
            size_buffer(win, x, y);
            put_event((ui_event){ .type = EV_SIZE, .w = win->surf.w, .h = win->surf.h });
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (win->min_w || win->min_h)
        {
            SIZE outer = frame_size(hwnd, win->min_w, win->min_h, (DWORD)GetWindowLongW(hwnd, GWL_STYLE));
            ((MINMAXINFO *)lp)->ptMinTrackSize = (POINT){ outer.cx, outer.cy };
        }
        return 0;
    case WM_DPICHANGED:
    {   /* moved to a monitor of another DPI: the size Windows suggests, then EV_SIZE */
        RECT *to = (RECT *)lp;
        scale = scale_of(HIWORD(wp));
        SetWindowPos(hwnd, NULL, to->left, to->top, to->right - to->left, to->bottom - to->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        put_event((ui_event){ .type = EV_CLOSE });
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        put_event((ui_event){ .type = EV_FOCUS, .button = msg == WM_SETFOCUS });
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) != HTCLIENT)
            break;
        SetCursor(cursor);
        return TRUE;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        int key = key_of(wp);
        if (key)
            put_event((ui_event){ .type = EV_KEY, .key = key, .repeat = (lp >> 30) & 1 });
        if (msg == WM_SYSKEYDOWN && (wp == VK_F4 || wp == VK_SPACE))
            break;  /* Alt+F4 and the window menu still work; other Alt keys do not open the menu */
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (key_of(wp))
            put_event((ui_event){ .type = EV_KEY_UP, .key = key_of(wp) });
        return 0;
    case WM_CHAR:
        char_event(win, wp);
        return 0;
    case WM_SYSCHAR:
        return 0;   /* (no beep for Alt+letter) */
    case WM_MOUSEMOVE:
        if (!win->inside && x >= 0 && y >= 0 && x < win->surf.w && y < win->surf.h)
        {
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd, 0 };
            win->inside = TrackMouseEvent(&track);
        }
        put_event((ui_event){ .type = EV_MOVE, .x = x, .y = y });
        return 0;
    case WM_MOUSELEAVE:
        if (win->inside && !win->buttons)
            put_event((ui_event){ .type = EV_LEAVE });
        win->inside = 0;
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        pointer_down(win, BTN_LEFT, msg == WM_LBUTTONDBLCLK ? 2 : 1, lp);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        pointer_down(win, BTN_RIGHT, msg == WM_RBUTTONDBLCLK ? 2 : 1, lp);
        return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        pointer_down(win, BTN_MIDDLE, msg == WM_MBUTTONDBLCLK ? 2 : 1, lp);
        return 0;
    case WM_LBUTTONUP:
        pointer_up(win, BTN_LEFT, lp);
        return 0;
    case WM_RBUTTONUP:
        pointer_up(win, BTN_RIGHT, lp);
        return 0;
    case WM_MBUTTONUP:
        pointer_up(win, BTN_MIDDLE, lp);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        wheel(win, wp, lp, msg == WM_MOUSEHWHEEL);
        return 0;
    case WM_DROPFILES:
        drop_files(win, (HDROP)wp);
        return 0;
    case WM_PAINT_WAKE:
        InterlockedExchange(&wake_pending, 0);
        put_event((ui_event){ .type = EV_TICK });
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- start-up and the loop ---- */

int host_init(void)
{
    if (instance)
        return 0;
    instance = GetModuleHandleW(NULL);
    main_thread = GetCurrentThreadId();
    dpi_aware();
    CoInitialize(NULL);     /* for the shell's parts of the Open/Save pickers */
    scale = scale_of(window_dpi(NULL));
    cursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));   /* res/paint.rc's icon, once it has one */
    wc.lpszClassName = class_name;
    return RegisterClassExW(&wc) ? 0 : -1;
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
    if (InterlockedExchange(&wake_pending, 1))
        return;
    if (W)
        PostMessageW(W->hwnd, WM_PAINT_WAKE, 0, 0);
    else
        PostThreadMessageW(main_thread, WM_PAINT_WAKE, 0, 0);
}

void host_run(int (*timeout_ms)(void *user), void *user)
{
    MSG msg;
    quitting = 0;
    while (!quitting)
    {
        while (!quitting && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_PAINT_WAKE && !msg.hwnd)
            {   /* (posted before the window was open) */
                InterlockedExchange(&wake_pending, 0);
                put_event((ui_event){ .type = EV_TICK });
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quitting)
            break;
        int wait = timeout_ms ? timeout_ms(user) : -1;
        double deadline = os_now() + wait / 1000.0;
        DWORD got = MsgWaitForMultipleObjectsEx(0, NULL, wait < 0 ? INFINITE : (DWORD)wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait >= 0 && (got == WAIT_TIMEOUT || os_now() >= deadline))
            put_event((ui_event){ .type = EV_TICK });
    }
}

rect host_work_area(void)
{
    RECT r;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    return (rect){ r.left, r.top, r.right - r.left, r.bottom - r.top };
}

/* ---- the window ---- */

host_win *host_open(const char *title, int w, int h, void (*on_event)(void *user, const ui_event *ev), void *user)
{
    host_win *win = calloc(1, sizeof(*win));
    rect area = host_work_area();
    SIZE outer = frame_size(NULL, w, h, WS_OVERLAPPEDWINDOW);
    WCHAR *wide = to_wide(title);
    win->on_event = on_event;
    win->user = user;
    W = win;
    size_buffer(win, w, h);
    win->hwnd = CreateWindowExW(0, class_name, wide, WS_OVERLAPPEDWINDOW, area.x + (area.w - outer.cx) / 2,
                                area.y + (area.h - outer.cy) / 2, outer.cx, outer.cy, NULL, NULL, instance, NULL);
    free(wide);
    if (!win->hwnd)
    {
        W = NULL;
        release_buffer(win);
        free(win);
        return NULL;
    }
    scale = scale_of(window_dpi(win->hwnd));
    DragAcceptFiles(win->hwnd, TRUE);
    if (wake_pending)
        PostMessageW(win->hwnd, WM_PAINT_WAKE, 0, 0);
    return win;
}

void host_close(host_win *win)
{
    if (W == win)
        W = NULL;
    DestroyWindow(win->hwnd);
    release_buffer(win);
    if (win->big_icon)
        DestroyIcon(win->big_icon);
    if (win->small_icon)
        DestroyIcon(win->small_icon);
    free(win);
}

void host_show(host_win *win, int maximized)
{
    ShowWindow(win->hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(win->hwnd);
}

void host_title(host_win *win, const char *title)
{
    WCHAR *wide = to_wide(title);
    SetWindowTextW(win->hwnd, wide);
    free(wide);
}

void host_min_size(host_win *win, int w, int h)
{
    win->min_w = w;
    win->min_h = h;
}

/* Borderless and the monitor's size; back to the old placement after */
void host_fullscreen(host_win *win, int on)
{
    if (on == win->fullscreen)
        return;
    win->fullscreen = on;
    if (on)
    {
        MONITORINFO mon = { sizeof(mon) };
        win->placement.length = sizeof(win->placement);
        GetWindowPlacement(win->hwnd, &win->placement);
        win->style_before = (DWORD)GetWindowLongW(win->hwnd, GWL_STYLE);
        GetMonitorInfoW(MonitorFromWindow(win->hwnd, MONITOR_DEFAULTTONEAREST), &mon);
        SetWindowLongW(win->hwnd, GWL_STYLE, (LONG)((win->style_before & ~WS_OVERLAPPEDWINDOW) | WS_POPUP));
        SetWindowPos(win->hwnd, HWND_TOP, mon.rcMonitor.left, mon.rcMonitor.top, mon.rcMonitor.right - mon.rcMonitor.left,
                     mon.rcMonitor.bottom - mon.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    else
    {
        SetWindowLongW(win->hwnd, GWL_STYLE, (LONG)win->style_before);
        SetWindowPlacement(win->hwnd, &win->placement);
        SetWindowPos(win->hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

void host_cursor(host_win *win, int shape)
{
    static const WORD ids[CURSOR_BUSY + 1] = { 32512, 32649, 32513, 32515, 32646, 32645, 32644, 32642, 32643, 0, 32514 };
    if (shape < 0 || shape > CURSOR_BUSY)
        return;
    cursor = shape == CURSOR_NONE ? NULL : LoadCursorW(NULL, MAKEINTRESOURCEW(ids[shape]));
    if (!cursor && shape == CURSOR_HAND)
        cursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);     /* (no IDC_HAND before Windows 2000) */
    POINT screen, at;
    RECT client;
    GetCursorPos(&screen);
    at = screen;
    ScreenToClient(win->hwnd, &at);
    GetClientRect(win->hwnd, &client);
    if (GetCapture() == win->hwnd || (PtInRect(&client, at) && WindowFromPoint(screen) == win->hwnd))
        SetCursor(cursor);  /* now, not at the next move */
}

void host_capture(host_win *win, int on)
{
    if (on)
        SetCapture(win->hwnd);
    else if (GetCapture() == win->hwnd)
        ReleaseCapture();
}

void host_ime_spot(host_win *win, int x, int y, int h)
{
    HIMC imc = ImmGetContext(win->hwnd);
    if (!imc)
        return;
    COMPOSITIONFORM form = { CFS_POINT, { x, y } };
    CANDIDATEFORM candidates = { 0, CFS_EXCLUDE, { x, y }, { x, y, x + 1, y + h } };
    ImmSetCompositionWindow(imc, &form);
    ImmSetCandidateWindow(imc, &candidates);
    ImmReleaseContext(win->hwnd, imc);
}

/* An HICON of the picture scaled to size x size (a box filter, weighted by alpha) */
static HICON make_icon(const img *im, int size)
{
    BITMAPV5HEADER head = { sizeof(head), size, -size, 1, 32, BI_BITFIELDS };
    head.bV5RedMask = 0xFF0000;
    head.bV5GreenMask = 0xFF00;
    head.bV5BlueMask = 0xFF;
    head.bV5AlphaMask = 0xFF000000u;
    uint32_t *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&head, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color)
        return NULL;
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            int x0 = x * im->w / size, x1 = (x + 1) * im->w / size, y0 = y * im->h / size, y1 = (y + 1) * im->h / size;
            uint32_t sum[4] = { 0 }, count = 0;
            x1 = x1 > x0 ? x1 : x0 + 1;
            y1 = y1 > y0 ? y1 : y0 + 1;
            for (int sy = y0; sy < y1; sy++)
            {
                for (int sx = x0; sx < x1; sx++, count++)
                {
                    const uint8_t *px = img_at(im, sx, sy);
                    for (int c = 0; c < 3; c++)
                        sum[c] += px[c] * px[3];
                    sum[3] += px[3];
                }
            }
            uint32_t a = sum[3];
            bits[y * size + x] = a ? (a / count) << 24 | (sum[0] / a) << 16 | (sum[1] / a) << 8 | sum[2] / a : 0;
        }
    }
    void *zeros = calloc((size_t)(size + 15) / 16 * 2, (size_t)size);     /* the AND mask: alpha decides */
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros);
    free(zeros);
    ICONINFO info = { TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void host_icon(host_win *win, const img *icon)
{
    HICON big = make_icon(icon, GetSystemMetrics(SM_CXICON)), small = make_icon(icon, GetSystemMetrics(SM_CXSMICON));
    SendMessageW(win->hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(win->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
    if (win->big_icon)
        DestroyIcon(win->big_icon);
    if (win->small_icon)
        DestroyIcon(win->small_icon);
    win->big_icon = big;
    win->small_icon = small;
}

/* ---- the clipboard ---- */

static UINT png_format(void)
{
    static UINT format;
    format = format ? format : RegisterClipboardFormatW(L"PNG");
    return format;
}

/* Another program may hold it a moment: try for a quarter of a second */
static int open_clipboard(void)
{
    for (int tries = 0; tries < 25; tries++)
    {
        if (OpenClipboard(W ? W->hwnd : NULL))
            return 1;
        Sleep(10);
    }
    return 0;
}

static HGLOBAL global_copy(const void *data, size_t len)
{
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, len);
    void *at = mem ? GlobalLock(mem) : NULL;
    if (!at)
    {
        if (mem)
            GlobalFree(mem);
        return NULL;
    }
    memcpy(at, data, len);
    GlobalUnlock(mem);
    return mem;
}

static void set_data(UINT format, HGLOBAL mem)
{
    if (mem && !SetClipboardData(format, mem))
        GlobalFree(mem);
}

void host_clip_set_text(const char *utf8)
{
    size_t lines = 0;
    for (const char *at = utf8; *at; at++)
        lines += *at == '\n';
    char *crlf = malloc(strlen(utf8) + lines + 1), *put = crlf;
    for (const char *at = utf8; *at; at++)
    {   /* Windows ends lines with CR LF */
        if (*at == '\n' && (at == utf8 || at[-1] != '\r'))
            *put++ = '\r';
        *put++ = *at;
    }
    *put = 0;
    WCHAR *wide = to_wide(crlf);
    free(crlf);
    if (open_clipboard())
    {
        EmptyClipboard();
        set_data(CF_UNICODETEXT, global_copy(wide, sizeof(WCHAR) * (wcslen(wide) + 1)));
        CloseClipboard();
    }
    free(wide);
}

char *host_clip_text(void)
{
    char *text = NULL;
    if (!open_clipboard())
        return NULL;
    HANDLE mem = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *wide = mem ? GlobalLock(mem) : NULL;
    if (wide)
    {
        text = to_utf8(wide);
        GlobalUnlock(mem);
    }
    CloseClipboard();
    char *put = text;
    for (const char *at = text; at && *at; at++)
    {   /* back to LF */
        if (!(at[0] == '\r' && at[1] == '\n'))
            *put++ = *at;
    }
    if (put)
        *put = 0;
    return text;
}

/* CF_DIBV5 (straight alpha, bottom-up) for every program, and PNG for those that read it */
void host_clip_set_image(const img *im)
{
    size_t pixels = (size_t)im->w * (size_t)im->h, png_len = 0;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + 4 * pixels);
    BITMAPV5HEADER *head = dib ? GlobalLock(dib) : NULL;
    if (head)
    {
        *head = (BITMAPV5HEADER){ sizeof(BITMAPV5HEADER), im->w, im->h, 1, 32, BI_BITFIELDS };
        head->bV5SizeImage = (DWORD)(4 * pixels);
        head->bV5RedMask = 0xFF0000;
        head->bV5GreenMask = 0xFF00;
        head->bV5BlueMask = 0xFF;
        head->bV5AlphaMask = 0xFF000000u;
        head->bV5CSType = 0x73524742;   /* LCS_sRGB */
        head->bV5Intent = LCS_GM_IMAGES;
        uint8_t *out = (uint8_t *)(head + 1);
        for (int y = 0; y < im->h; y++)
        {
            const uint8_t *row = img_at(im, 0, im->h - 1 - y);
            for (int x = 0; x < im->w; x++, row += 4, out += 4)
            {
                out[0] = row[2];
                out[1] = row[1];
                out[2] = row[0];
                out[3] = row[3];
            }
        }
        GlobalUnlock(dib);
    }
    uint8_t *png = img_png(im, &png_len);
    if (open_clipboard())
    {
        EmptyClipboard();
        if (head)
            set_data(CF_DIBV5, dib);
        if (png)
            set_data(png_format(), global_copy(png, png_len));
        CloseClipboard();
    }
    else if (dib)
        GlobalFree(dib);
    free(png);
}

/* A packed DIB (CF_DIB or CF_DIBV5) of 24 or 32 bits a pixel; its alpha only when it has some */
static img *from_dib(const uint8_t *data, size_t len)
{
    const BITMAPINFOHEADER *head = (const BITMAPINFOHEADER *)data;
    if (len < sizeof(*head) || head->biSize < sizeof(*head) || head->biPlanes != 1 ||
        (head->biBitCount != 24 && head->biBitCount != 32) ||
        (head->biCompression != BI_RGB && head->biCompression != BI_BITFIELDS))
        return NULL;
    int w = head->biWidth, h = head->biHeight < 0 ? -head->biHeight : head->biHeight, bottom_up = head->biHeight > 0;
    size_t offset = head->biSize + (head->biCompression == BI_BITFIELDS && head->biSize == sizeof(*head) ? 12 : 0);
    size_t stride = ((size_t)w * head->biBitCount / 8 + 3) & ~(size_t)3;
    uint32_t masks[4] = { 0xFF0000, 0xFF00, 0xFF, head->biBitCount == 32 ? 0xFF000000u : 0 };
    if (head->biCompression == BI_BITFIELDS)
    {   /* (masks after a plain header, or inside a V4/V5 one) */
        memcpy(masks, data + sizeof(*head), head->biSize >= 56 ? 16 : 12);
        masks[3] = head->biSize >= 56 ? masks[3] : 0;
    }
    if (w <= 0 || h <= 0 || offset + stride * (size_t)h > len)
        return NULL;
    int shift[4], alpha_seen = 0;
    for (int c = 0; c < 4; c++)
    {
        shift[c] = 0;
        while (masks[c] && !(masks[c] >> shift[c] & 1))
            shift[c]++;
    }
    img *im = img_new(w, h, (rgba){ 0, 0, 0, 255 });
    if (!im)
        return NULL;
    for (int y = 0; y < h; y++)
    {
        const uint8_t *row = data + offset + stride * (size_t)(bottom_up ? h - 1 - y : y);
        for (int x = 0; x < w; x++)
        {
            uint32_t px = head->biBitCount == 32 ? (uint32_t)row[4 * x] | (uint32_t)row[4 * x + 1] << 8 |
                                                   (uint32_t)row[4 * x + 2] << 16 | (uint32_t)row[4 * x + 3] << 24 :
                                                   (uint32_t)row[3 * x] | (uint32_t)row[3 * x + 1] << 8 | (uint32_t)row[3 * x + 2] << 16;
            uint8_t *out = img_at(im, x, y);
            for (int c = 0; c < 4; c++)
                out[c] = masks[c] ? (uint8_t)((px & masks[c]) >> shift[c]) : 255;
            alpha_seen |= out[3] != 0;
        }
    }
    for (size_t idx = 0; !alpha_seen && idx < (size_t)w * (size_t)h; idx++)
        im->px[4 * idx + 3] = 255;  /* all zero: the alpha byte is padding, not transparency */
    return im;
}

img *host_clip_image(void)
{
    static const UINT dib_formats[2] = { CF_DIBV5, CF_DIB };
    img *im = NULL;
    const char *err;
    if (!open_clipboard())
        return NULL;
    HANDLE mem = GetClipboardData(png_format());
    const uint8_t *data = mem ? GlobalLock(mem) : NULL;
    if (data)
    {
        im = img_load_mem(data, GlobalSize(mem), &err);
        GlobalUnlock(mem);
    }
    for (int idx = 0; !im && idx < 2; idx++)
    {
        mem = GetClipboardData(dib_formats[idx]);
        data = mem ? GlobalLock(mem) : NULL;
        if (data)
        {
            im = from_dib(data, GlobalSize(mem));
            GlobalUnlock(mem);
        }
    }
    CloseClipboard();
    return im;
}

int host_clip_has_image(void)
{
    return IsClipboardFormatAvailable(png_format()) || IsClipboardFormatAvailable(CF_DIB) ||
           IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_BITMAP);
}

/* ---- the pickers and the shell ---- */

/* "Label\0*.png;*.jpg\0...\0\0", as the common dialogs take filters */
static WCHAR *filter_list(const char *const *filters)
{
    size_t len = 2;
    for (int idx = 0; filters && filters[idx]; idx++)
        len += strlen(filters[idx]) + 1;
    char *list = calloc(len, 1), *put = list;
    for (int idx = 0; filters && filters[idx] && filters[idx + 1]; idx++)
    {
        strcpy(put, filters[idx]);
        put += strlen(filters[idx]) + 1;
    }
    WCHAR *wide = malloc(sizeof(WCHAR) * len);
    int count = MultiByteToWideChar(CP_UTF8, 0, list, (int)len, wide, (int)len);
    if (count <= 0)
        wide[0] = wide[1] = 0;
    free(list);
    return wide;
}

static char *pick(host_win *owner, int save, const char *title, const char *name, const char *const *filters, int *unavailable)
{
    WCHAR *path = calloc(32768, sizeof(WCHAR)), *wide_title = to_wide(title), *list = filter_list(filters), ext[16] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    char *chosen = NULL;
    if (unavailable)
        *unavailable = 0;
    if (save && name)
    {
        WCHAR *wide_name = to_wide(name), *dot = wcsrchr(wide_name, L'.');
        wcsncpy(path, wide_name, 32767);
        if (dot && wcslen(dot + 1) < 16)
            wcscpy(ext, dot + 1);
        free(wide_name);
    }
    ofn.hwndOwner = owner ? owner->hwnd : NULL;
    ofn.lpstrFilter = list[0] ? list : NULL;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = path;
    ofn.nMaxFile = 32768;
    ofn.lpstrTitle = wide_title;
    ofn.lpstrDefExt = save ? (ext[0] ? ext : L"png") : NULL;    /* (the chosen filter's extension wins) */
    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (owner)
        host_capture(owner, 0);
    modal++;
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    modal--;
    if (ok)
        chosen = to_utf8(path);
    free(path);
    free(wide_title);
    free(list);
    return chosen;      /* (what came meanwhile is delivered when the app's handler returns) */
}

char *host_open_file(host_win *owner, const char *title, const char *const *filters, int *unavailable)
{
    return pick(owner, 0, title, NULL, filters, unavailable);
}

char *host_save_file(host_win *owner, const char *title, const char *name, const char *const *filters, int *unavailable)
{
    return pick(owner, 1, title, name, filters, unavailable);
}

void host_open_url(const char *url)
{
    WCHAR *wide = to_wide(url);
    ShellExecuteW(NULL, L"open", wide, NULL, NULL, SW_SHOWNORMAL);
    free(wide);
}
