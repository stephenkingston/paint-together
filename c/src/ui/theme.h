/*
 * Paint (C port) - the look of classic (Windows 7/10) Paint (Rust: theme.rs, and ribbon.rs's constants)
 *
 * A light "Scenic Ribbon" palette and square Windows-style controls. Every
 * size in the Rust code is in points; here a point is ui_scale / 100
 * device pixels, and S() turns the Rust numbers into pixels.
 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "ui/font.h"
#include "ui/gfx.h"

extern int ui_scale;    /* percent: host_scale() */
void theme_set_scale(int percent);

/* Points to device pixels, rounded */
static inline int S(float pt)
{
    float v = pt * (float)ui_scale / 100.0f;
    return (int)(v < 0 ? v - 0.5f : v + 0.5f);
}
static inline float SF(float pt)
{
    return pt * (float)ui_scale / 100.0f;
}
/* A hairline: one device pixel per point, so 1 px at 100% and 2 at 200% (and 150%) */
static inline int HAIR(void)
{
    int h = (ui_scale + 50) / 100;
    return h < 1 ? 1 : h;
}

/* Lines one point thick (HAIR() pixels): along x0..x1 with the top at y, or y0..y1 with the left at x */
static inline void hair_h(surface *s, int x0, int x1, int y, color c)
{
    gfx_fill(s, rect_make(x0, y, x1 - x0 + 1, HAIR()), c);
}
static inline void hair_v(surface *s, int x, int y0, int y1, color c)
{
    gfx_fill(s, rect_make(x, y0, HAIR(), y1 - y0 + 1), c);
}

/* The UI font at a size in points */
font *ui_font(float pt, int style);

/* ---- palette (Rust's CLASSIC) ---- */
#define PAL_CHROME          COLOR(0xFFFFFF)     /* title bar and tab strip */
#define PAL_RIBBON          COLOR(0xF5F6F7)     /* the ribbon body */
#define PAL_RIBBON_BORDER   COLOR(0xDADBDC)
#define PAL_GROUP_SEPARATOR COLOR(0xE2E3E4)
#define PAL_WORKSPACE       COLOR(0xC9D3E2)     /* around the canvas */
#define PAL_STATUS          COLOR(0xF0F0F0)
#define PAL_STATUS_BORDER   COLOR(0xD7D7D7)
#define PAL_CARD            COLOR(0xFFFFFF)
#define PAL_TEXT            COLOR(0x1E1E1E)
#define PAL_TEXT_WEAK       COLOR(0x6D6D6D)
#define PAL_HOVER           COLOR(0xE8EFF7)
#define PAL_HOVER_BORDER    COLOR(0xA4CEF9)
#define PAL_PRESSED         COLOR(0xC9E0F7)
#define PAL_PRESSED_BORDER  COLOR(0x62A2E4)
#define PAL_CHECKED         COLOR(0xCCE4F7)
#define PAL_CHECKED_BORDER  COLOR(0x66A7E8)
#define PAL_FILE_TAB        COLOR(0x1979CA)
#define PAL_FILE_TAB_HOVER  COLOR(0x2A8AD4)
#define PAL_ACCENT          COLOR(0x0078D7)
#define PAL_CONTEXTUAL      COLOR(0xF2C811)
#define PAL_CANVAS_SHADOW   COLOR_A(0x000000, 40)

/* egui's widget visuals as theme.rs sets them */
#define VIS_WINDOW_STROKE   COLOR(0xA0A0A0)
#define VIS_SELECTION       COLOR(0xCCE8FF)     /* selected text */
#define VIS_INACTIVE_BG     COLOR(0xFFFFFF)     /* text fields, check boxes */
#define VIS_INACTIVE_WEAK   COLOR(0xFDFDFD)     /* buttons */
#define VIS_INACTIVE_STROKE COLOR(0xABADB3)
#define VIS_HOVER_BG        COLOR(0xE5F1FB)
#define VIS_HOVER_STROKE    COLOR(0x7EB4EA)
#define VIS_ACTIVE_BG       COLOR(0xCCE4F7)
#define VIS_ACTIVE_STROKE   COLOR(0x569DE5)
#define DISABLED_TEXT       COLOR(0xA0A0A0)
#define CHECKED_HOVER       COLOR(0xB9D9F6)
#define DIALOG_FILL         COLOR(0xF0F0F0)
#define DIALOG_STROKE       COLOR(0x888888)
#define DIALOG_BUTTON       COLOR(0xE1E1E1)
#define DIALOG_BUTTON_EDGE  COLOR(0xADADAD)
#define DIALOG_HEADING      COLOR(0x003399)     /* group box titles, "Do you want to save" */
#define BACKDROP            COLOR_A(0x000000, 28)

/* ---- metrics, in points ---- */
#define CONTENT_H           66      /* a ribbon group's content: one large button, or three small ones */
#define SMALL_H             22
#define LABEL_PT            12.0f   /* ribbon labels */
#define HIGHLIGHT_RADIUS    3
#define TAB_FLARE           5       /* the selected tab's outward curves */
#define TAB_RADIUS          5
#define TITLE_BAR_H         30
#define TAB_STRIP_H         24
#define STATUS_H            24
#define FONT_SMALL          11.0f
#define FONT_BODY           12.5f
#define FONT_HEADING        16.0f
#define ITEM_SPACING_X      6
#define ITEM_SPACING_Y      5
#define BUTTON_PAD_X        8
#define BUTTON_PAD_Y        3
#define INTERACT_W          36
#define INTERACT_H          22
#define MENU_MARGIN         6
#define WINDOW_MARGIN       14
#define COMBO_HEIGHT        320
#define SCROLL_BAR_W        12

#endif
