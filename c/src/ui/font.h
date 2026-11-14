/*
 * Paint (C port) - the fonts the window's own text is drawn with
 *
 * stb_truetype on every platform. The UI face is the desktop's: Segoe UI on
 * Windows; on Linux the first of a list of common sans faces found under the
 * usual font folders (Noto Sans, DejaVu Sans, Liberation Sans, Cantarell...).
 * FONT_SERIF is Times New Roman / a serif face (the Text tool's "A").
 * Glyphs are cached per face and pixel size. Text is UTF-8; missing glyphs
 * fall back to the other loaded faces, then to a box.
 *
 * (The Text tool's fonts, which draw onto the picture, are paint/text.h.)
 */
#ifndef UI_FONT_H
#define UI_FONT_H

#include "ui/gfx.h"

enum
{
    FONT_REGULAR = 0,
    FONT_BOLD = 1,
    FONT_SERIF = 2,     /* | FONT_BOLD for serif bold */
};

typedef struct font font;

int font_init(void);                        /* 0 ok, -1 no font found anywhere */
font *font_get(float px, int style);        /* cached; px is the em size in screen pixels */
int font_height(const font *f);             /* line height (ascent + descent + gap), rounded up */
int font_ascent(const font *f);             /* baseline below the top of the line */
int text_width(font *f, const char *utf8, int len);    /* len -1: to the NUL */
/* Draws with the top of the line at y; returns the x after the text */
int text_draw(surface *dst, font *f, int x, int y, const char *utf8, int len, color c);

enum
{
    ALIGN_LEFT = 0,
    ALIGN_CENTER = 1,
    ALIGN_RIGHT = 2,
};
/* One line inside r: aligned horizontally, centred vertically, cut with "..." if too wide */
void text_in(surface *dst, font *f, rect r, int align, const char *utf8, color c);
/* Word-wrapped lines inside a width; returns the height used (draws nothing when dst is NULL) */
int text_wrap(surface *dst, font *f, rect r, int align, const char *utf8, color c);

#endif
