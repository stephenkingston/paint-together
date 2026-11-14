/*
 * Paint (C port) - the app's state and behaviour, without its drawing (Rust: app.rs, minus the UI)
 *
 * One `app` holds the picture, the tools, the selection, the text box, the
 * live shape, the open dialogs' values, and drawing together. The views
 * (src/app/view_*.c) read its fields to draw the window and call the
 * functions below for every ribbon control, menu item, dialog button and
 * canvas event. Nothing here draws.
 *
 * How a frame goes (what app.rs's update did, per event instead of per frame):
 *  - main: app_cli() for the command line, then app_new() + app_start().
 *    Set a->win once the window exists, and app_set_scale() from host_scale().
 *  - Every event: input on the canvas area goes to app_canvas_event (it sets
 *    a->cursor, the system pointer to show, and a->tool_icon, a tool drawn as
 *    the pointer); keys to app_key / app_key_up and typed text to app_text
 *    when no dialog or text field of the views has the keyboard; EV_CLOSE to
 *    app_close_requested; EV_DROP to app_request(a, PENDING_OPEN, path).
 *  - After handling any event, and on EV_TICK: app_tick(a, os_now()) takes in
 *    the network, keeps the airbrush spraying, expires notices and sends my
 *    drawing to friends. Then draw if app_needs_repaint() (or anything of the
 *    views changed), and give host_run app_wake_ms() as its timeout.
 *  - Drawing the canvas: app_set_view() with the picture area's rectangle
 *    first (it applies a pending zoom-to-fit and keeps the scroll in range),
 *    then app_canvas_rect() is where the picture goes. app_take_dirty() says
 *    what changed in the picture since last time (for a cached zoomed copy);
 *    app_friends_overlay() is friends' unfinished drawing, shown over it.
 *
 * Coordinates: picture ("canvas") points are floats in picture pixels, as
 * the engine uses. Screen positions are the window's device pixels; `ppp`
 * is device pixels per point (host_scale() / 100), so zoom 1.0 shows one
 * picture pixel per point, as the Rust did, and the margins, handles and
 * tolerances keep their size in points.
 */
#ifndef APP_APP_H
#define APP_APP_H

#include <stdint.h>
#include "collab/discovery.h"
#include "collab/net.h"
#include "collab/session.h"
#include "paint/brush.h"
#include "paint/history.h"
#include "paint/live_shape.h"
#include "paint/text_edit.h"
#include "ui/host.h"

#define APP_MARGIN 8.0f         /* gap round the picture in the scroll area, points */
#define APP_RULER 18.0f         /* the rulers' width, points */
#define APP_DEFAULT_W 1152
#define APP_DEFAULT_H 648

typedef enum
{
    TAB_HOME,
    TAB_VIEW,
    TAB_TOGETHER,
    TAB_TEXT,           /* only while the Text tool is chosen */
} app_tab;

typedef enum
{
    TOOL_SELECT,
    TOOL_FREE_SELECT,
    TOOL_PENCIL,
    TOOL_FILL,
    TOOL_TEXT,
    TOOL_ERASER,
    TOOL_PICKER,
    TOOL_MAGNIFIER,
    TOOL_BRUSH,
    TOOL_SHAPE,
} app_tool;

/* An action waiting on the "save changes?" prompt (Rust's Pending) */
enum
{
    PENDING_NONE,
    PENDING_NEW,
    PENDING_OPEN,       /* arg: the path, or none to ask with the picker */
    PENDING_QUIT,
    PENDING_JOIN,       /* arg: the address of a friend's drawing */
};

typedef struct
{
    int kind;
    char *arg;          /* owned; may be NULL */
} pending;

/* A mouse interaction in progress on the canvas (Rust's Op) */
enum
{
    OP_IDLE,
    OP_STROKE,
    OP_SHAPE_CREATE,    /* dragging out a new shape */
    OP_SHAPE_HANDLE,    /* dragging one of the live shape's handles */
    OP_SHAPE_MOVE,
    OP_TEXT_MOVE,       /* dragging the text box; a click without a drag reopens the editor */
    OP_TEXT_SCALE,      /* dragging a corner of the text box scales the text */
    OP_SELECTING,
    OP_MOVING_SEL,
    OP_RESIZE_CANVAS,
    OP_PAN,             /* middle-button drag */
};

enum
{
    PEN_PENCIL,
    PEN_ERASER,
    PEN_BRUSH,
};

/* The canvas resize handles */
enum
{
    HANDLE_NONE = -1,
    HANDLE_RIGHT,
    HANDLE_BOTTOM,
    HANDLE_CORNER,
};

typedef struct
{
    int kind;           /* OP_* */
    int button;         /* BTN_* that started it */
    /* OP_STROKE */
    int pen;            /* PEN_* */
    brush_kind brush;
    img *snapshot;      /* the picture when the stroke began */
    mask *mask;
    rgba color;
    float opacity, size;
    pt last;
    irect dirty;
    /* the shape, text and selection ops */
    int handle;         /* OP_SHAPE_HANDLE, OP_TEXT_SCALE: which; OP_RESIZE_CANVAS: HANDLE_* */
    pt grab;            /* where it was grabbed */
    pt start_a, start_b;    /* the shape's geometry when grabbed; start_a is the text's position for OP_TEXT_MOVE */
    int moved;          /* OP_TEXT_MOVE */
    float start_size;   /* OP_TEXT_SCALE */
    frect start_bounds;
    pt sel_start, sel_end;  /* OP_SELECTING: the dragged box... */
    pt *lasso;          /* ...and the free-form outline so far */
    int n_lasso, cap_lasso;
    int origin_x, origin_y; /* OP_MOVING_SEL: the selection's corner when grabbed */
    int size_w, size_h; /* OP_RESIZE_CANVAS: the new size so far */
    float pan_x, pan_y; /* OP_PAN: the last pointer position (screen) */
} app_op;

/* Selected pixels that have been picked up and can be moved around */
typedef struct
{
    img *pixels;
    img *base;          /* the canvas underneath (the lifted area filled with Color 2) */
    /* The canvas before the pixels were lifted, for undo. Outside `origin` it is
     * `base`, so only its pixels inside `origin` are kept (`lifted`), unless
     * pasting enlarged the canvas: then all of it (`before`). */
    img *before;
    img *lifted;
    irect origin;       /* where the pixels were lifted from; empty for pastes */
    int has_hole;       /* the lifted pixels as they were then, and the Color 2 left behind */
    patch hole;
    rgba hole_color;
    int canvas_changed; /* pasting enlarged the canvas */
} floating;

typedef struct
{
    irect rect;
    pt *lasso;          /* free-form outline, relative to rect's top-left; NULL for a rectangle */
    int n_lasso;
    floating *fl;       /* NULL until the pixels are picked up */
} selection;

typedef struct
{
    pt pos;
    char *text;         /* UTF-8 */
    img *snapshot;
    mask *mask;
    irect dirty;
    frect bounds;       /* in picture pixels */
    int editing;        /* typing goes into the box (otherwise the text is just placed, still movable) */
    text_cursor cursor;
    caret_layout layout;    /* caret positions for the text as last rendered (relative to pos) */
    char *preedit;      /* an input method's uncommitted text, shown at the caret */
    double blink;       /* when the caret last moved, so it shows solid while typing */
    /* what was last rendered */
    int rendered;
    char *r_text;
    text_style r_style;
    rgba r_fg, r_bg;
    pt r_pos;
} text_box;

/* A short notice at the bottom of the picture */
typedef struct
{
    char text[160];
    double until;       /* when it disappears (fade it out over its last 0.5 s) */
    int group;          /* 0, or 1 "joined" / 2 "left": a burst of arrivals becomes one notice */
    int count;
} toast;

#define APP_MAX_TOASTS 4

/* The dialogs (each NULL / 0 when closed; several may be open, drawn in this order) */
typedef struct
{
    int percent;        /* by percentage, else pixels */
    float w, h;
    int keep_aspect;
    int base_w, base_h; /* what is resized: the selection's or the picture's size */
} resize_dialog;

/* When the platform has no file picker: ask for a path in a dialog of our own */
enum
{
    ASK_NONE,
    ASK_OPEN,
    ASK_SAVE,           /* ask_ext: the extension to add when the path has none */
    ASK_PASTE_FROM,
};

/* Moving the room to a new host after the host left */
typedef struct
{
    int active;
    kid_list candidates;    /* whom to try, in order; whoever reaches themselves hosts */
    os_sock spare;          /* the listener kept ready for hosting, while no attempt holds it */
} handover;

typedef struct
{
    uint64_t id, foreign;
} replace_mark;

typedef struct app
{
    host_win *win;      /* for the pickers, title and full screen; may be NULL (tests) */
    double now;         /* the app's clock (os_now), set by app_tick and events */
    float ppp;          /* device pixels per point */

    /* ---- the document ---- */
    img *img;           /* the picture on screen */
    char *path;         /* NULL: Untitled */
    history *history;
    uint64_t saved_state;
    int has_meta;       /* size on disk and modification time of the open file */
    int64_t file_size, file_mtime;
    char *recent[9];    /* recent pictures, newest first */
    int n_recent;

    /* ---- tools and colours ---- */
    app_tool tool, prev_tool;
    app_tool select_mode;   /* what the Select button picks: TOOL_SELECT or TOOL_FREE_SELECT */
    brush_kind brush;
    shape_kind shape;
    int shape_outline, shape_fill;
    float size_pencil, size_brush, size_eraser, size_shape;
    float fill_tolerance;   /* percent */
    rgba primary, secondary;    /* Color 1 and Color 2 */
    int editing_secondary;  /* the palette sets Color 2 */
    rgba custom[10];
    int has_custom[10];
    int transparent_selection;
    text_style text_style;  /* family is -1 until the Text tool is first used (fonts are found then) */
    uint64_t rng;

    /* ---- what's on the canvas ---- */
    app_op op;
    selection *sel;
    text_box *text;
    live_shape *live;       /* the shape just drawn, still movable and resizable */

    /* ---- the view ---- */
    float zoom;
    float scroll_x, scroll_y;   /* the scroll area's offset, device pixels (app_set_view keeps it in range) */
    rect view;              /* the picture area (inside the rulers), device pixels */
    int fit_pending;
    int show_grid, show_status, show_rulers, show_thumbnail, full_screen;
    app_tab tab;
    int ribbon_minimized;
    irect dirty;            /* what changed in the picture since app_take_dirty */
    int dirty_full;
    irect preview_dirty;    /* what changed since my drawing in progress was last sent */

    /* ---- the pointer (set by app_canvas_event) ---- */
    int cursor;             /* CURSOR_* to show */
    int tool_icon;          /* ICON_* drawn as the pointer at tool_x tool_y (its tip there), or -1 */
    float tool_x, tool_y;
    int hover;              /* the pointer is over the canvas area, at hover_x hover_y (screen) */
    float hover_x, hover_y;
    int has_cursor_px;      /* the picture pixel under the pointer (status bar, ruler markers) */
    int cursor_px_x, cursor_px_y;
    int has_pointer;        /* the pointer over the picture in picture pixels (for friends to see) */
    pt pointer;
    int menu_open;          /* the selection tools' right-click menu was asked for, at menu_x menu_y (screen) */
    int menu_x, menu_y;

    /* ---- dialogs and menus (the views draw them; see the functions below) ---- */
    int file_menu_open, file_menu_formats;
    pending confirm;        /* "Do you want to save changes to <name>?" (kind PENDING_NONE: closed) */
    pending confirm_everyone;   /* New/Open while drawing together: "... for everyone drawing with you?" */
    resize_dialog *resize;  /* Resize and Skew */
    int properties;         /* Image Properties, with its width and height fields */
    float prop_w, prop_h;
    int about_open;
    int edit_colors;        /* Edit Colors, with the colour as hue/saturation/luminance 0..1 */
    float hsl[3];
    char *error;            /* a message box */
    char *join_dialog;      /* "Join a drawing", with the address typed so far (NULL: closed) */
    int ask_kind;           /* ASK_*: "type a path" (the platform has no picker) */
    char ask_ext[8];
    char ask_name[256];     /* the name to start the field with */
    pending ask_then;       /* what to do once the save it asks for is done */
    int allow_close, quit;

    /* ---- drawing together ---- */
    session *session;       /* while drawing together */
    uint64_t saved_version; /* session->version when last saved */
    link *link;             /* the connection to the room (hosting or joined) */
    int has_invite;         /* hosting: where friends can join */
    os_addr invite;
    char me_name[64];       /* my name and colour when drawing together */
    uint8_t me_color[3];
    int ask_first;          /* drawings I start: friends wait for me to let them in */
    char *asking;           /* waiting to be let in: the host's name */
    double last_share;      /* when my preview and pointer were last sent */
    discovery *discovery;   /* finding drawings nearby (started with app_discover) */
    int lan_discovery;      /* use mDNS (off in tests) */
    toast toasts[APP_MAX_TOASTS];
    int n_toasts;
    handover handover;
    replace_mark *marks;    /* my whole-picture edits (by undo step): friends' edit count then */
    int n_marks;

    /* private */
    char last_text_[16];    /* the text typed since the last key went down (an IME may commit it again) */
    double last_spray_;
    int right_menu_;        /* a right press with a selection tool, waiting for its release */
    int has_last_pt_;       /* the pointer during a drag, in picture pixels */
    pt last_pt_;
    char title_[300];
} app;

/* ---- start and stop ---- */

/* The command line: `--export-icon PATH [SIZE]` writes the logo PNG and returns
 * the exit status; otherwise -1, with *path (a picture to open) or *join
 * (`--join ADDRESS`) set (or NULL). */
int app_cli(int argc, char **argv, const char **path, const char **join);
/* A blank picture with Paint's defaults (Rust's with_context): no settings read but recent pictures */
app *app_new(void);
/* The real app (Rust's PaintApp::new): my name and colour from the settings (or
 * a fresh one), discovery on, then open `path` or join `join` (either may be NULL) */
void app_start(app *a, const char *path, const char *join);
void app_free(app *a);
void app_set_scale(app *a, float ppp);

/* ---- the loop ---- */

/* Pump the network, spray, expire notices, send to friends. 1 if something changed. */
int app_tick(app *a, double now);
/* How long host_run may wait before the next app_tick (ms), or -1 */
int app_wake_ms(const app *a);
/* Something in the picture or an operation is in progress: draw again */
int app_needs_repaint(const app *a);
/* The window title ("*Untitled - Paint", " (drawing together)"); call host_title when it changes */
const char *app_title(app *a);
/* The file name shown ("Untitled" for a new picture) */
const char *app_doc_name(const app *a, char *buf, size_t cap);
/* EV_CLOSE: 1 to close now; 0 when it asks to save first (a->confirm is then open) */
int app_close_requested(app *a);
/* There are unsaved changes (also something floating, typed or still live) */
int app_is_dirty(const app *a);

/* ---- the canvas ---- */

void app_set_view(app *a, rect view);       /* each frame, before drawing the canvas */
rect app_canvas_rect(const app *a);         /* the picture's screen rectangle (zoom * ppp per pixel) */
void app_content_size(const app *a, float *w, float *h);   /* the scroll area's content, device pixels */
void app_scroll_to(app *a, float x, float y);           /* scroll bars: clamped to the content */
/* Pointer events over the picture area (EV_MOVE EV_DOWN EV_UP EV_WHEEL EV_LEAVE;
 * also every move and the release while a drag that started here goes on).
 * Sets a->cursor, a->tool_icon, a->hover, the status bar position and
 * a->menu_open. Returns 1 if anything shown may have changed. */
int app_canvas_event(app *a, const ui_event *ev);
/* Where the tool icon's tip sits on the icon's 20x20 grid (the pointer is drawn so the tip is on it) */
pt app_tool_tip(int icon);
/* Picture pixels between gridlines at this zoom */
int app_grid_step(float zoom);
/* The changed part of the picture since the last call (whole: the picture was replaced
 * or resized); 0 if nothing changed. Includes friends' drawing in progress. */
int app_take_dirty(app *a, irect *out);
/* Friends' drawing in progress (same size as the picture, transparent elsewhere), or NULL */
const img *app_friends_overlay(const app *a);
/* r of im with overlay's non-transparent pixels on top, as RGBA rows into out (w(r) * h(r) * 4 bytes) */
void app_with_friends(const img *im, const img *overlay, irect r, uint8_t *out);
/* The live shape's handles and the text box's corner handles are drawn while
 * these say so (picture pixels; the text box's, expanded 3 points, are its corners) */
int app_shows_shape_handles(const app *a);

/* The same operations in picture coordinates, as mouse input drives them
 * (tests, and anything that wants to draw without the screen). `anchor` is
 * the magnifier's point, relative to the view's top-left (device pixels). */
void app_begin(app *a, pt p, int button, pt anchor);
void app_update(app *a, const pt *pts, int n, int shift, float dt);
void app_end(app *a);

/* ---- keyboard ---- */

/* A key went down (host key codes, KMOD_*), when no dialog or text field of the
 * views has the keyboard: typing into the text box, then Paint's shortcuts.
 * 1 if it was used. */
int app_key(app *a, int key, int mods);
void app_key_up(app *a, int key, int mods);
/* Typed text (EV_TEXT), an input method's composition and its commit */
void app_text(app *a, const char *utf8);
void app_ime_preedit(app *a, const char *utf8);
void app_ime_commit(app *a, const char *utf8);
/* "Ctrl+S" / "⌘S": a shortcut for a tooltip */
const char *app_shortcut(int mods, int key, char *buf, size_t cap);
/* Where an input method's window goes: the caret, screen pixels; 0 if not typing */
int app_caret_rect(const app *a, float *x, float *y, float *h);
/* The caret is shown now (it blinks) */
int app_caret_on(const app *a);

/* ---- File menu ---- */

void app_request(app *a, int what, const char *arg);    /* New, Open, Quit, Join: asks to save first if needed */
int app_save(app *a);                       /* 1 if saved */
int app_save_as(app *a);
/* "Save as" > PNG/JPEG/BMP/GIF picture ("png", "jpg", "bmp", "gif"), or NULL: Other formats */
int app_save_as_format(app *a, const char *ext);
void app_open_properties(app *a);
void app_properties_ok(app *a);             /* applies prop_w x prop_h (the canvas grows or shrinks) */
void app_properties_cancel(app *a);
/* The "save changes?" answer: 0 Save, 1 Don't Save, 2 Cancel */
void app_confirm_answer(app *a, int choice);
void app_confirm_everyone_answer(app *a, int go);
/* The "type a path" dialog's OK (path) and Cancel */
void app_ask_path_ok(app *a, const char *path);
void app_ask_path_cancel(app *a);
void app_dismiss_error(app *a);
/* Properties' "Last saved" and "Size on disk" texts ("Not available") */
void app_format_time(int64_t secs, char *buf, size_t cap);  /* "10/10/2026 13:45", local time */
void app_format_bytes(int64_t b, char *buf, size_t cap);    /* "512 bytes", "1.5KB", "2.0MB" */

/* ---- Home tab ---- */

void app_set_tool(app *a, app_tool tool);
void app_pick_brush(app *a, brush_kind k);  /* the Brushes gallery: also chooses the Brush tool */
void app_pick_shape(app *a, shape_kind k);  /* the Shapes gallery: stamps the live shape, chooses the Shape tool */
void app_set_shape_outline(app *a, int on);
void app_set_shape_fill(app *a, int on);
/* The size the Size button edits for the current tool (pencil, eraser, shape, else brush;
 * NULL for the Fill tool, which has its tolerance), with its presets and maximum */
float *app_tool_size(app *a, const float **presets, float *max);
void app_set_select_mode(app *a, app_tool mode);    /* Select's menu: rectangular or free-form */
void app_select_all(app *a);
void app_delete_selection(app *a);
void app_toggle_transparent_selection(app *a);
void app_copy(app *a);
void app_cut(app *a);
void app_paste(app *a);
void app_paste_from(app *a);                /* Paste from: a picture file */
void app_paste_image(app *a, img *pixels);  /* takes it; floats at the top-left of the view */
void app_crop(app *a);
void app_open_resize(app *a);               /* Resize and Skew */
void app_resize_set_mode(app *a, int percent);
void app_resize_set_w(app *a, float w);     /* keeps the aspect when asked */
void app_resize_set_h(app *a, float h);
void app_resize_ok(app *a);
void app_resize_cancel(app *a);
enum
{
    ROTATE_RIGHT,
    ROTATE_LEFT,
    ROTATE_180,
    FLIP_VERTICAL,
    FLIP_HORIZONTAL,
};
void app_rotate(app *a, int how);           /* the selection if there is one, else the picture */
void app_invert(app *a);
void app_clear_image(app *a);
/* Transform the selection or the picture with f (rotate, flip, resize...) */
void app_transform(app *a, img *(*f)(const img *src, void *user), void *user);
void app_set_active_color(app *a, rgba c);  /* a palette swatch: Color 1 or 2, whichever is being edited */
rgba app_active_color(const app *a);
void app_open_edit_colors(app *a);
void app_edit_colors_ok(app *a);
void app_edit_colors_cancel(app *a);
void app_add_custom_color(app *a, rgba c);
/* Paint's palette: 20 colours and their names */
extern const uint8_t APP_PALETTE[20][3];
extern const char *const APP_PALETTE_NAMES[20];
extern const uint32_t APP_BASIC_COLORS[48];     /* the Edit Colors dialog's, 0xRRGGBB */
rgba hsl_to_rgb(const float hsl[3]);
void rgb_to_hsl(rgba c, float hsl[3]);

/* ---- Text tab ---- */

void app_set_text_style(app *a, text_style st);     /* font, size, bold..., opaque: restyles the text box */
extern const float APP_TEXT_SIZES[16];

/* ---- View tab, zoom and the status bar ---- */

void app_zoom_at(app *a, float z, const pt *anchor);    /* anchor: view-relative device pixels, NULL = the middle */
void app_zoom_step(app *a, int up, const pt *anchor);
void app_zoom_fit(app *a);
float app_zoom_slider(const app *a);        /* the status bar's slider position 0..1 (0.125x..8x), tick at 100% */
float app_zoom_slider_tick(void);
void app_set_zoom_slider(app *a, float t);
void app_set_full_screen(app *a, int on);
/* The status bar's selection size (w h) and canvas size (during a canvas resize, the new one) */
int app_selection_size(const app *a, int *w, int *h);
void app_canvas_size(const app *a, int *w, int *h);

/* ---- Together tab ---- */

int app_hosting(const app *a);              /* this app runs the room */
void app_start_together(app *a);
void app_join_together(app *a, const char *addr);   /* (Join dialog: app_request(PENDING_JOIN, addr)) */
void app_leave_together(app *a);            /* also "Stop waiting" while asking to be let in */
void app_set_ask_first(app *a, int on);     /* "Let friends in without asking" is !ask_first (or session->ask_first when hosting) */
void app_kick(app *a, peer_id kid);
void app_let_in(app *a, const peer_id *kids, int n, int yes);   /* the door notices' answers */
void app_set_me_name(app *a, const char *name);     /* tidied and saved */
void app_set_me_color(app *a, const uint8_t c[3]);
void app_open_join_dialog(app *a);
void app_close_join_dialog(app *a);
void app_discover(app *a);                  /* look for drawings nearby (the Together tab and Join dialog call it) */
const nearby *app_nearby(const app *a, int *n);     /* drawings found nearby */
const kid *app_find_kid(const app *a, peer_id id);
/* The status bar's "Drawing with 2 friends" (0 if not drawing together) */
int app_together_status(const app *a, char *buf, size_t cap);

/* ---- drawing together, the parts tests drive (Rust's names) ---- */

void app_net_receive(app *a, from_hub *msg);        /* uses up msg */
void app_pump_network(app *a);
void app_flush_network(app *a);
/* What to send the hub now: my finished edits, and (at most 30 times a second)
 * my drawing in progress and pointer. Malloc'd, *n of them. */
to_hub *app_net_outbox(app *a, double now, int *n);

/* ---- the rest of Rust's PaintApp methods, for the views and tests ---- */

void app_undo(app *a);
void app_redo(app *a);
void app_settle(app *a);                    /* finish whatever is in progress */
void app_commit_all(app *a);                /* stamp the text, selection and live shape */
void app_commit_selection(app *a);
void app_commit_text(app *a);
void app_discard_text(app *a);
void app_commit_shape(app *a);
void app_discard_shape(app *a);
void app_render_live_shape(app *a);
void app_update_text_preview(app *a);
void app_refresh_float(app *a);
void app_load_image(app *a, img *im);       /* takes it */
int app_open_path(app *a, const char *path);

#endif
