/*
 * Paint (C port) - classic Paint-style colour icons (Rust: icons.rs)
 *
 * Every icon is vector art on a 20x20 grid, drawn at whatever pixel size
 * it is shown at (16 and 32 points, scaled) with the same pixel snapping as
 * the Rust. Drawn icons are kept in a small cache of tiles (the least
 * recently used one is reused), so a frame mostly just blends pixels.
 */
#ifndef UI_ICONS_H
#define UI_ICONS_H

#include "ui/gfx.h"

enum
{
    ICON_PASTE,
    ICON_CUT,
    ICON_COPY,
    ICON_SELECT,
    ICON_FREE_SELECT,
    ICON_SELECT_ALL,
    ICON_DELETE,
    ICON_CROP,
    ICON_RESIZE,
    ICON_ROTATE,
    ICON_ROTATE_LEFT,
    ICON_ROTATE_180,
    ICON_FLIP_VERTICAL,
    ICON_FLIP_HORIZONTAL,
    ICON_PENCIL,
    ICON_FILL,
    ICON_TEXT,
    ICON_ERASER,
    ICON_PICKER,
    ICON_MAGNIFIER,
    ICON_BRUSHES,
    ICON_SIZE,
    ICON_EDIT_COLORS,
    ICON_OUTLINE,       /* arg: the outline colour, or 0 for no outline */
    ICON_FILL_SHAPE,    /* arg: the fill colour, or 0 for no fill */
    ICON_ZOOM_IN,
    ICON_ZOOM_OUT,
    ICON_ZOOM_100,
    ICON_FULL_SCREEN,
    ICON_NEW,
    ICON_OPEN,
    ICON_SAVE,
    ICON_SAVE_AS,
    ICON_PROPERTIES,
    ICON_ABOUT,
    ICON_EXIT,
    ICON_TOGETHER,      /* drawing together: two kids */
    ICON_JOIN_ROOM,
    ICON_LEAVE_ROOM,
    ICON_UNDO,
    ICON_REDO,
    ICON_HELP,
    ICON_COLLAPSE,
    ICON_DROPDOWN,
    ICON_SCROLL_UP,
    ICON_SCROLL_DOWN,
    ICON_GALLERY_MORE,
    ICON_CHECK,
    ICON_CURSOR_POS,
    ICON_SELECTION_SIZE,
    ICON_CANVAS_SIZE,
    ICON_FILE_SIZE,
    ICON_OPAQUE,
    ICON_TRANSPARENT,
    ICON_BOLD,
    ICON_ITALIC,
    ICON_UNDERLINE,
    ICON_STRIKE,
    ICON_FILE_TYPE,     /* arg: FILE_KIND_* */
    ICON_SHAPE,         /* arg: the shape_kind (paint/shapes.h) */
    ICON_LOGO,          /* the app's own icon (paint/logo.h; logo_render for host_icon) */
    ICON_COUNT
};

enum
{
    FILE_KIND_PNG,
    FILE_KIND_JPEG,
    FILE_KIND_BMP,
    FILE_KIND_GIF,
    FILE_KIND_OTHER,
};

/* The icon in the px x px square at (x, y); disabled ones are faded grey */
void icon_draw(surface *dst, int icon, uint32_t arg, int x, int y, int px, int enabled);
/* Centred on (cx, cy), the square's corner rounded to a pixel (Rust's icons::square) */
void icon_at(surface *dst, int icon, uint32_t arg, float cx, float cy, int px, int enabled);
/* With a white outline round it, so it shows on any colour (the tool pointers) */
void icon_halo(surface *dst, int icon, float cx, float cy, int px);

#endif
