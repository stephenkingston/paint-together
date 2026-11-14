/*
 * Paint (C port) - the widget kit's state, shared by kit.c, kit_widgets.c and ribbon.c
 *
 * Everything is in fixed arrays: a frame allocates nothing.
 */
#ifndef UI_KIT_INTERNAL_H
#define UI_KIT_INTERNAL_H

#include <stdio.h>
#include "ui/font.h"
#include "ui/kit.h"

enum
{
    MAX_EVENTS = 128,
    MAX_KEYS = 32,
    MAX_LAYOUT = 48,
    MAX_IDS = 24,
    MAX_SLOTS = 8,      /* overlays alive at once */
    MAX_LAYERS = 8,
    MAX_OPEN = 4,       /* nested popups */
    MAX_WINS = 4,
    MAX_FOCUSABLE = 64,
    MAX_SCROLLS = 16,
    MAX_TOASTS = 4,
    SEEN = 1024,        /* ids met this frame (open addressing) */
};

enum
{
    DIR_COL,
    DIR_ROW,
    DIR_RTL,
};

typedef struct
{
    rect max;
    int dir, gap, cross, is_area;
    int x, y;           /* cursor: next x (rows; the left edge so far for RTL) or y (columns) */
    int n;              /* children placed */
    int ex, ey;         /* how far the children reach (ex: leftmost for RTL) */
} lay;

typedef struct
{
    kit_id id;
    surface *s;
    rect r;             /* where it was last shown */
    rect assumed;       /* where this frame's drawing assumed it is */
    int alive, fresh, popup, modal, dialog, framed, fit, flags, dismiss, transparent;
    int shadow_dx, shadow_dy, shadow_blur, shadow_alpha;
    int bar, wanted_w, wanted_h, grab_x, grab_y;
} overlay;

typedef struct
{
    int slot, depth, flags, close;
} popup;

typedef struct
{
    kit_id id;
    int x, y;           /* offset, pixels */
    int cw, ch;         /* content size last frame */
    int alive;
} scroll_state;

typedef struct
{
    kit_id id;
    rect view, saved_clip;
    int flags, slot, bar_v, bar_h;
} scroll_frame;

typedef struct
{
    surface *window, *dst;
    double now, repaint_at;
    int cursor;

    ui_event queue[MAX_EVENTS], ev[MAX_EVENTS];
    int nq, n_ev;
    char qtext[1024];
    size_t qtext_at;

    kit_input in;
    int down, down_btn, down_x, down_y, down_clicks, down_claimed;
    int up, up_btn, up_x, up_y;
    float wheel_x, wheel_y;
    struct
    {
        int key, mods, taken;
    } keys[MAX_KEYS];
    int n_keys;
    char text[128];
    int text_taken;

    kit_id ids[MAX_IDS];
    int n_ids;
    kit_id seen[SEEN];

    lay lay[MAX_LAYOUT];
    int nl;

    kit_id active, focus, press_focus;
    int active_btn, active_clicks, active_seen, focus_seen, down_claimed_focus;
    kit_id focusable[MAX_FOCUSABLE];
    int n_focusable;

    /* layers: the window is 0, overlay slot s is s + 1 */
    overlay slots[MAX_SLOTS];
    int order[MAX_SLOTS], n_order, prev_order[MAX_SLOTS], n_prev;
    int layer, hover_layer, down_layer;
    struct
    {
        int layer, nl;
        surface *dst;
    } layer_stack[MAX_LAYERS];
    int n_layers;
    struct
    {
        kit_id id;
        rect anchor;
        int alive;
    } open[MAX_OPEN];
    int n_open;
    popup popups[MAX_OPEN];
    int n_popups;
    int win_slot[MAX_WINS], n_wins;
    int overlay_slot;
    int again;

    kit_id tip_id;
    double tip_since;
    int tip_x, tip_y, tip_seen, tip_shown;
    char tip_text[512];

    /* the text field being edited */
    kit_id te_id;
    int te_caret, te_anchor, te_scroll;
    double te_blink;
    char num_buf[32];
    float drag_start_value;
    int drag_moved, drag_x0;     /* a number being dragged */
    int scroll_grab;             /* where a scroll bar's thumb was taken */

    /* the open combo box */
    kit_id combo_id;
    int combo_item, combo_hot, combo_found, combo_reveal;
    char find[32];
    double find_at;
    int find_pending;
    int combo_px, combo_py;      /* the pointer when the list was last drawn (hover beats the arrow keys only when it moves) */

    scroll_state scrolls[MAX_SCROLLS];
    scroll_frame sf[4];
    int n_sf;

    struct
    {
        int start_x, start_y, w;
    } boxes[4];
    int n_boxes;

    struct
    {
        char text[128];
        double until;
    } toasts[MAX_TOASTS];
    int n_toasts;
} kit_state;

extern kit_state K;

int hover_ok(rect r);
void tooltip_draw(void);
int slot_find(kit_id id);
int layer_begin(int s, rect r, color fill);
void layer_end(int s, rect final);

#endif
