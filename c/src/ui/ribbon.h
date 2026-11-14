/*
 * Paint (C port) - the controls of the Windows "Scenic Ribbon" classic Paint is built from (Rust: ribbon.rs)
 *
 * Large and small buttons, split buttons, groups, tabs, menu rows, swatches,
 * the status bar's pieces, and the drawing-together badges and notices
 * from app.rs. They lay themselves out in the kit's current layout, like
 * the Rust ones do in their Ui. Sizes are points; icons are ICON_* with
 * their argument (see icons.h); label text may hold '\n'.
 */
#ifndef UI_RIBBON_H
#define UI_RIBBON_H

#include "ui/icons.h"
#include "ui/kit.h"

/* Fill and border for a control in a state (nothing when idle) */
void rb_frame(surface *dst, rect r, int hovered, int pressed, int checked);

/* A large button: 32 px icon over a label (dropdown: ends with a small arrow) */
kit_resp rb_big_button(int icon, uint32_t arg, const char *label, int dropdown, int checked, int enabled);
/* The same with the app drawing the 32 px picture: *picture is where (after the frame is drawn) */
kit_resp rb_big_button_custom(const char *label, int dropdown, int checked, int enabled, rect *picture);
/* A large split button (Paste, Select, Brushes): the icon part acts, the label part
 * (*menu) opens a menu */
kit_resp rb_big_split(int icon, uint32_t arg, const char *label, int checked, int enabled, kit_resp *menu);
/* A small button: 16 px icon (icon < 0: none), label, optional arrow */
kit_resp rb_small_button(int icon, uint32_t arg, const char *label, int dropdown, int checked, int enabled);
/* A square 16 px icon button (tools, text styles, the Quick Access Toolbar) with a tooltip */
kit_resp rb_icon_button(int icon, uint32_t arg, int checked, int enabled, const char *tooltip);

/* A ribbon group: content centred in the content area (content_h high), the
 * caption (or none) underneath, a separator after. Content is a row with 2 pt gaps. */
void rb_group_begin(const char *caption, int content_h);
void rb_group_end(void);

/* The tab strip: the blue File tab, the tabs (contextual: the yellow-banded
 * one), and the line under them that opens beneath the selected tab */
kit_resp rb_file_tab(void);
kit_resp rb_tab(const char *label, int selected, int contextual);
void rb_tab_line(int x0, int x1, int y, const rect *selected);
/* The selected tab's outline (rb_tab draws it; for the tab drawn again over the ribbon body) */
void rb_selected_tab_outline(rect r);

/* Menus */
void rb_menu_header(const char *text);
kit_resp rb_menu_item(int icon, uint32_t arg, const char *label, int checked, int enabled);    /* icon < 0: none */
kit_resp rb_file_menu_item(int icon, const char *label, int arrow, int highlighted);

/* Colour swatches with Paint's grey-and-white double border */
kit_resp rb_swatch(color c, int side_pt);
void rb_paint_swatch(surface *dst, rect r, color c, int hovered);
void rb_empty_swatch(int side_pt);

/* A gallery of icons (the shapes): n cells of cell_pt in cols x rows, the
 * scroll strip on the right whose "more" button is *more; names give the
 * tooltips. Returns the cell clicked, or -1. */
int rb_gallery(int icon, int n, int cols, int rows, int cell_pt, int checked, const char *const *names, kit_resp *more);
/* A preset row in the Size menu: a bar `size` px thick and "Npx" */
kit_resp rb_size_row(float size, int checked);
/* A slider with its value: thin grey track, blue fill and thumb (log: logarithmic) */
int rb_value_slider(float *value, float min, float max, int log, const char *suffix);

/* The status bar */
void rb_status_item(int icon, const char *text, int width_pt);
kit_resp rb_status_link(int icon, const char *text);
int rb_zoom_slider(float *t, float tick, int width_pt);    /* 1 when changed */
kit_resp rb_status_button(int minus);

/* Drawing together */
void rb_avatar(const char *name, color c, int size_pt);    /* a friend's coloured badge with their initial */
int rb_kid_row(const char *label, const char *name, color c, const char *kick_tip);   /* 1: kick clicked */
kit_resp rb_nearby_row(const char *name, const char *text, int compatible);
kit_resp rb_more_button(const char *count);
/* A notice at the door: maybe a friend's badge, the text, a green tick (when
 * tick_tip) and a red cross. Size it with rb_door_pill_size for its overlay. */
void rb_door_pill_size(const char *who, const char *text, int tick, int *w, int *h);
int rb_door_pill(const char *who, color who_color, const char *text, const char *tick_tip, const char *cross_tip);   /* 1 yes, 0 no, -1 nothing */

#endif
