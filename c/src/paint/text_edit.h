/*
 * Paint (C port) - caret, selection and undo for text typed straight onto the canvas (Rust: text_edit.rs)
 *
 * The text is a malloc'd, NUL-terminated UTF-8 string the caller owns; the
 * editing calls take its address and may replace it. Positions count
 * characters, not bytes.
 */
#ifndef PAINT_TEXT_EDIT_H
#define PAINT_TEXT_EDIT_H

#include "paint/text.h"

typedef struct
{
    char *text;
    int cursor;
} text_snapshot;

typedef struct
{
    int cursor;         /* character index of the caret */
    int anchor;         /* the other end of the selection (== cursor when nothing is selected) */
    int has_goal;       /* column to aim for when moving up and down */
    float goal_x;
    text_snapshot *undo, *redo;     /* the editor's own undo, at most 500 steps */
    int nundo, nredo, capundo, capredo;
} text_cursor;

void text_cursor_init(text_cursor *c);
void text_cursor_free(text_cursor *c);

static inline int text_cursor_start(const text_cursor *c)      /* the selection, start..end */
{
    return c->cursor < c->anchor ? c->cursor : c->anchor;
}
static inline int text_cursor_end(const text_cursor *c)
{
    return c->cursor > c->anchor ? c->cursor : c->anchor;
}
static inline int text_cursor_has_selection(const text_cursor *c)
{
    return c->cursor != c->anchor;
}
char *text_cursor_selected(const text_cursor *c, const char *text);        /* malloc'd copy */

/* Type s, replacing the selection */
void text_cursor_insert(text_cursor *c, char **text, const char *s);
void text_cursor_delete_selection(text_cursor *c, char **text);
/* Delete before the caret: a character, a word (Alt/Ctrl) or to the line start (Cmd) */
void text_cursor_backspace(text_cursor *c, char **text, const caret_layout *layout, int word, int line);
/* Delete after the caret: a character or a word */
void text_cursor_delete(text_cursor *c, char **text, int word);

void text_cursor_move_to(text_cursor *c, int index, int extend);
void text_cursor_left(text_cursor *c, const char *text, int extend, int word);
void text_cursor_right(text_cursor *c, const char *text, int extend, int word);
void text_cursor_line_start(text_cursor *c, const caret_layout *layout, int extend);
void text_cursor_line_end(text_cursor *c, const caret_layout *layout, int extend);
/* Move delta lines up (negative) or down, keeping the column */
void text_cursor_vertical(text_cursor *c, const char *text, const caret_layout *layout, int delta, int extend);
void text_cursor_select_all(text_cursor *c, const char *text);
void text_cursor_undo(text_cursor *c, char **text);
void text_cursor_redo(text_cursor *c, char **text);

#endif
