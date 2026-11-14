/*
 * Paint (C port) - the immediate-mode widget kit (what egui is to the Rust app)
 *
 * Every frame the app calls kit_begin, then its widget functions, which
 * draw at once and say what happened (clicked, changed...), then kit_end.
 * Nothing is kept between frames but a little state by id: the widget
 * under the pointer and the one being pressed, keyboard focus, the open
 * popups, scroll offsets and window positions.
 *
 * Units: sizes passed to kit and ribbon functions are points, the numbers
 * in the Rust code (S() turns them into pixels); rectangles are pixels.
 * Functions ending in _px take pixels.
 *
 * Layout is egui's: a stack of rows and columns. kit_alloc() takes the next
 * piece of the current one; a nested row or column takes its place in its
 * parent when it ends. kit_area() starts a layout in an explicit rectangle.
 *
 * Layers: popups, menus and dialogs are drawn into their own small
 * surfaces while the app builds them (wherever in the frame that is) and
 * put on top of the window, in the order opened, by kit_end; then
 * kit_on_top's painting (the toasts), and the tooltip last. kit_dst() is the surface the current layer draws into: draw
 * custom things there, never into the window's surface directly. The
 * topmost layer under the pointer (as of the previous frame) gets the
 * pointer; a modal dialog blocks everything below it.
 *
 * Driving it: hand every host event to kit_event(); build a frame when
 * kit_wants_frame() (from host_run's timeout callback, so a burst of events
 * makes one frame); if kit_end() returns 1 an overlay changed size and the
 * frame must be built again before it is shown; give host_run
 * kit_timeout_ms() so tooltips, the caret and toasts animate.
 */
#ifndef UI_KIT_H
#define UI_KIT_H

#include "ui/gfx.h"
#include "ui/host.h"
#include "ui/theme.h"

typedef uint32_t kit_id;

/* ---- the frame ---- */
void kit_init(void);
void kit_event(const ui_event *ev);         /* queue a host event for the next frame */
int kit_wants_frame(void);                  /* events are waiting, or a repaint is due */
void kit_begin(surface *window, double now);
int kit_end(void);                          /* 1: build the frame again before presenting it */
int kit_timeout_ms(void);                   /* for host_run: when the next frame is needed (-1 none) */
void kit_repaint(void);                     /* another frame soon */
void kit_repaint_after(double seconds);
int kit_cursor(void);                       /* the pointer shape the widgets want (CURSOR_*) */
void kit_set_cursor(int shape);             /* the app's choice (e.g. over the canvas) */
double kit_now(void);
surface *kit_dst(void);                     /* where the current layer draws */
rect kit_screen(void);                      /* the whole window */

/* ---- input ---- */
typedef struct
{
    int x, y;           /* the pointer, last known */
    int inside;         /* in the window */
    int held[3];        /* buttons down at the end of the frame (BTN_*) */
    int mods;           /* KMOD_* */
} kit_input;
const kit_input *kit_in(void);
/* This frame's events, in order (the canvas follows every move of a stroke) */
const ui_event *kit_events(int *count);
/* Whether window point (x, y) is free of overlays (popups, dialogs): the canvas's input */
int kit_free_at(int x, int y);
/* A key pressed this frame that no widget took, with exactly these modifiers
 * (Shift is ignored when mods has no KMOD_SHIFT and the key is not a letter);
 * taking it stops later calls seeing it */
int kit_key(int key, int mods);
/* Text typed this frame that no widget took (NULL if none) */
const char *kit_text(void);
/* Wheel notches over r this frame, taking them (0 if none, or an overlay is over r) */
float kit_wheel(rect r, float *wheel_x);

/* ---- ids ---- */
kit_id kit_id_of(const char *key);          /* key hashed under the current parent id */
kit_id kit_id_n(const char *key, int n);
void kit_push_id(const char *key);
void kit_push_n(int n);
void kit_pop_id(void);

/* ---- layout ---- */
enum
{
    KIT_TOP = 0,        /* rows: children at the top (egui's horizontal_top) */
    KIT_CENTER = 1,     /* rows: children centred in the row's height */
};
void kit_row(int gap_pt);                   /* left to right */
void kit_row_centered(int gap_pt, int h_pt);    /* left to right, centred in a row h high (egui's horizontal) */
void kit_row_rtl(int gap_pt, int h_pt);     /* right to left from the right edge, centred (fixed-size children only) */
void kit_col(int gap_pt);                   /* top to bottom */
rect kit_end_layout(void);                  /* the space it used, now taken in the parent */
void kit_area(rect r, int row, int gap_pt); /* a layout filling r (row or column); doesn't take space anywhere */
rect kit_area_end(void);
rect kit_alloc(int w_pt, int h_pt);         /* the next piece of the current layout */
rect kit_alloc_px(int w, int h);
void kit_space(int pt);                     /* empty space along the layout */
void kit_space_px(int px);
rect kit_avail(void);                       /* what is left of the current layout */
void kit_set_gap(int pt);
void kit_min_width(int pt);                 /* the current layout reports at least this wide */
void kit_min_height(int pt);

/* ---- interaction ---- */
enum
{
    SENSE_HOVER = 0,
    SENSE_CLICK = 1,
    SENSE_DRAG = 2,     /* also clicks */
    SENSE_FOCUS = 4,    /* takes the keyboard focus when clicked (and by Tab) */
};
typedef struct
{
    rect r;
    kit_id id;
    int x, y;           /* the pointer */
    unsigned hovered : 1, held : 1, clicked : 1, double_clicked : 1, right_clicked : 1, drag_started : 1, dragging : 1, released : 1,
        changed : 1, focused : 1, enabled : 1;
} kit_resp;
kit_resp kit_interact(rect r, kit_id id, int sense);
kit_resp kit_interact_off(rect r, kit_id id, int sense, int enabled);
const kit_resp *kit_last(void);             /* the last widget's response (for a check box's or text field's tooltip) */
void kit_focus(kit_id id);                  /* give a widget the keyboard focus (0: nobody) */
kit_id kit_focused(void);
/* A tooltip for the widget, shown after the pointer rests on it */
void kit_tooltip(const kit_resp *r, const char *text);
void kit_hover_cursor(const kit_resp *r, int shape);

/* ---- widgets ---- */
/* Text in the body font: one line in a row, wrapped to the width in a column */
rect kit_label(const char *text);
rect kit_label_ex(const char *text, float size_pt, int style, color c);
rect kit_label_wrap(const char *text, float size_pt, color c);
rect kit_label_w(const char *text, int w_px);   /* in a cell w wide (a grid's column) */
kit_resp kit_button(const char *label);     /* egui's default button */
/* theme::dialog_button: at least min_w_pt (0: 80) x 24; is_default: the blue border */
kit_resp kit_dialog_button(const char *label, int is_default, int enabled, int min_w_pt);
int kit_checkbox(const char *label, int *on);           /* 1 when changed */
int kit_checkbox_off(const char *label, int *on, int enabled);
int kit_radio(const char *label, int selected, int enabled);    /* 1 when clicked */
void kit_separator(void);
void kit_rule(int vertical);                /* a 1 px line across the layout, no spacing */

/* Single-line text field. buf holds cap bytes (NUL-terminated); max_chars limits
 * what may be typed (0: cap). Returns KIT_CHANGED / KIT_SUBMITTED / KIT_LOST_FOCUS bits. */
enum
{
    KIT_CHANGED = 1,
    KIT_SUBMITTED = 2,      /* Enter */
    KIT_LOST_FOCUS = 4,
    KIT_GAINED_FOCUS = 8,
};
enum
{
    FIELD_DISABLED = 1,
    FIELD_DIGITS = 2,       /* only 0-9 can be typed */
};
int kit_text_field(const char *id, char *buf, int cap, int max_chars, int width_pt, const char *hint, int flags);
/* A number: drag across it to change, or click and type (egui's DragValue) */
int kit_number(const char *id, float *value, float min, float max, int width_pt);

/* A drop-down list: kit_combo shows the closed box with `shown` in it and is
 * 1 while the list is open; then call kit_combo_item for each choice (1 when
 * picked: the list closes) and kit_combo_end. Typing finds an item. */
int kit_combo(const char *id, const char *shown, int width_pt, int list_h_pt);
int kit_combo_item(const char *text, int selected);
void kit_combo_end(void);

/* A scrolling area w x h (points; 0: what is available). Content goes in a
 * column; kit_scroll_end draws the bars. */
enum
{
    SCROLL_V = 1,
    SCROLL_H = 2,
    SCROLL_HIDE_BARS = 4,
};
void kit_scroll_begin(const char *id, int w_pt, int h_pt, int flags);
void kit_scroll_end(void);
void kit_scroll_to(const char *id, int x, int y);   /* pixels of content at the top-left */
void kit_scroll_offset(const char *id, int *x, int *y);

/* ---- overlays ---- */
enum
{
    POPUP_STAY_OPEN = 1,    /* clicks inside don't close it (egui's CloseOnClickOutside) */
};
/* A menu below `anchor`, opened and closed by clicking it; 1 while open:
 * build its rows, then kit_popup_end. width_pt is the whole menu's, as
 * egui's Popup::width (0: the content's) */
int kit_popup_menu(const kit_resp *anchor, int width_pt, int flags);
/* The same, opened by a right click on `on`, at the pointer; width_pt is
 * its content's (egui's ui.set_width), as for kit_submenu */
int kit_context_menu(const kit_resp *on, int width_pt);
/* A row of the popup being built that opens a menu beside it, on hover or a
 * click (egui's menu_button in a menu); 1 while open: build its rows, then
 * kit_popup_end. Closing it (a pick, kit_popup_close) closes its parents too. */
int kit_submenu(const char *label, int width_pt);
void kit_popup_end(void);
void kit_popup_close(void);                 /* close the popup being built (egui's ui.close) */
int kit_popup_open(kit_id anchor);
void kit_popup_toggle(kit_id anchor);       /* open or close a popup by its anchor's id */
/* A free-standing overlay at r (the File menu, the door notices): r.h 0 sizes
 * it to its content. `frame`: a white card with a border and shadow; else it
 * starts transparent (draw a pill in it). Ends with kit_overlay_end. */
void kit_overlay_begin(const char *id, rect r, int frame);
rect kit_overlay_end(void);
/* A window inside the window, with a title bar and a close button.
 * Modal ones dim everything else, are centred, and close on Escape or a
 * click outside; others can be dragged by the title and resized.
 * *closed is set when the user closed it. 1 if shown: build the body, then kit_window_end. */
enum
{
    WIN_MODAL = 1,
    WIN_RESIZABLE = 2,
};
int kit_window_begin(const char *id, const char *title, int w_pt, int h_pt, int flags, int *closed);
void kit_window_end(void);
/* egui's dialog of app.rs: modal, grey body, white title bar */
int kit_dialog_begin(const char *id, const char *title, int w_pt, int *dismissed);
void kit_dialog_end(void);
/* A Windows group box: a blue caption over a framed area */
void kit_group_box_begin(const char *title, int w_pt);
void kit_group_box_end(void);

/* Paint over everything (popups, menus, dialogs) but the tooltip, taking no
 * input (egui's Foreground layer painter, the toasts): kit_end calls
 * draw(arg) after the overlays, with kit_dst() the window. One per frame. */
void kit_on_top(void (*draw)(void *arg), void *arg);

#endif
