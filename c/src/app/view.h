/*
 * Paint (C port) - the window's views: everything app.rs drew (Rust: app.rs's UI functions)
 *
 * The views read the app's state and call its actions; they keep nothing of
 * the document. view_build draws one frame with the widget kit (between
 * kit_begin and kit_end); view_input then hands the canvas, keys and typed
 * text that no widget took to the app.
 *
 *  view_main.c     the frame, title bar, tab strip, status bar, File menu,
 *                  full screen, thumbnail, toasts and the door notices
 *  view_ribbon.c   the Home, View, Text and Together tabs
 *  view_canvas.c   the picture area: scroll bars, rulers, gridlines, the
 *                  marching ants, handles, caret, friends' pointers, and
 *                  the right-click menu
 *  view_dialogs.c  every dialog
 */
#ifndef APP_VIEW_H
#define APP_VIEW_H

#include "app/app.h"
#include "ui/kit.h"
#include "ui/ribbon.h"

/* One frame of the whole window */
void view_build(app *a);
/* After kit_end: the keys and text no widget took go to the app; 1 if it used any */
int view_keys(app *a);
/* Free the views' caches (the brush previews, the friends' picture) */
void view_free(void);

/* ---- shared between the view files ---- */

typedef struct
{
    rect file_tab;      /* where the File tab is (the File menu hangs from it) */
    rect view;          /* the picture area, for the toasts and the door notices */
    int canvas_btns;    /* buttons held that went down on the canvas */
    int canvas_hover;   /* the app was last told the pointer is over the canvas */
} view_state;
extern view_state V;

void view_ribbon(app *a, rect full);       /* the tab strip and (unless minimized) the ribbon */
void view_canvas(app *a, rect full);       /* the picture area, rulers included */
void view_canvas_input(app *a);             /* this frame's pointer events over the picture go to the app */
/* The picture to show, with friends' drawing in progress set for the next gfx_canvas */
const img *view_picture(app *a);
void view_free_previews(void);
void view_full_screen(app *a, rect full);
void view_dialogs(app *a);

/* The Home tab's groups the Text tab shares */
void view_clipboard_group(app *a);
void view_colors_group(app *a);
/* The rotate and flip items (the Rotate menu, and the right-click menu's) */
void view_rotate_items(app *a);

color view_rgba(rgba c);                    /* an opaque colour for the screen */
/* "Save (Ctrl+S)" */
const char *view_tip(char *buf, size_t cap, const char *what, int mods, int key);

#endif
