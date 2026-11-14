/*
 * Paint (C port) - the Text tool: system fonts, layout, and glyphs into a coverage mask (Rust: text.rs)
 *
 * The font list is made the first time any fonts_* call needs it, by reading
 * the name tables of the font files in the system's font folders (only each
 * file's table directory, 'name', 'OS/2' and 'post', not the outlines): on
 * Linux the fontconfig folders, on macOS /System/Library/Fonts, /Library/Fonts
 * and ~/Library/Fonts, on Windows the system and per-user Fonts folders. It
 * keeps a family name and, per style, a file path and face index. A face's
 * file is read when the Text tool first uses it and dropped when another face
 * is loaded. Families without a bold or italic face get a synthesized one.
 * Call these from the UI thread only.
 *
 * Text is UTF-8; positions in it (carets, selections) count characters
 * (Unicode scalar values), as the Rust's char indices do. Lines are split at
 * '\n'.
 */
#ifndef PAINT_TEXT_H
#define PAINT_TEXT_H

#include "paint/raster.h"

typedef struct
{
    int family;         /* index into the font list */
    float size_pt;
    int bold, italic, underline, strike;
    int opaque;         /* fill the box behind the text with Color 2 */
} text_style;

/* Point size at 96 DPI, like Windows */
static inline float text_style_px(const text_style *s)
{
    return s->size_pt * 96.0f / 72.0f;
}

/* ---- the font list ---- */

int fonts_count(void);                      /* families, sorted by name ignoring case; may be 0 */
const char *fonts_name(int family);         /* "" if out of range */
int fonts_find(const char *name);           /* exact family name, or -1 */
/* Calibri is Paint's default; otherwise the closest common face on the system (0 if none) */
int fonts_default_family(void);

typedef struct font_face font_face;
typedef struct
{
    font_face *face;
    int fake_bold, fake_italic;     /* the family has no real bold/italic face, so fake it */
} paint_font;

/* The face for a family and style, real or synthesized. 0 ok, -1 if it can't be
 * read. The face stays valid until fonts_load is called for a different face. */
int fonts_load(int family, int bold, int italic, paint_font *out);
void fonts_release(void);                   /* drop the loaded face's file */

/* ---- layout and drawing ---- */

typedef struct
{
    float width, height;
} text_size;

text_size ptext_measure(const paint_font *f, float px, const char *text);
/* Rasterize text with its top-left at origin into m; returns the touched pixels */
irect ptext_render(mask *m, const paint_font *f, float px, const char *text, pt origin, int underline, int strike);

/* Where the caret can sit: for each line, the x offset before every character
 * and after the last one, plus the line height. Offsets are from the text's origin. */
typedef struct
{
    int nlines;
    int *start;         /* nlines + 1 entries: line l's offsets are xs[start[l] .. start[l + 1]) */
    float *xs;
    float line_h;
} caret_layout;

void caret_layout_init(caret_layout *c);        /* one empty line, 16 px high (the Rust's Default) */
void caret_layout_build(caret_layout *c, const paint_font *f, float px, const char *text);   /* replaces the old */
void caret_layout_free(caret_layout *c);
static inline int caret_line_len(const caret_layout *c, int line)  /* caret positions on the line: characters + 1 */
{
    return c->start[line + 1] - c->start[line];
}
/* Line and column of a character index (newlines count as characters) */
void caret_line_col(const caret_layout *c, int index, int *line, int *col);
int caret_index(const caret_layout *c, int line, int col);
/* Top-left of the caret at index, relative to the origin */
pt caret_pos(const caret_layout *c, int index);
/* Nearest caret position on line to x */
int caret_col_at(const caret_layout *c, int line, float x);
/* Nearest caret index to a point relative to the origin */
int caret_index_at(const caret_layout *c, float x, float y);

/* UTF-8 helpers the text editor shares */
int utf8_decode(const char *s, int *len);               /* the code point at s and its byte length (bad bytes: U+FFFD, 1) */
int utf8_count(const char *s);                          /* characters in a NUL-terminated string */
size_t utf8_byte(const char *s, int index);             /* byte offset of character index (clamped to the end) */

#endif
