/*
 * Paint (C port) - the on-canvas text editor (Rust: text_edit.rs)
 */
#include <stdlib.h>
#include <string.h>
#include "paint/text_edit.h"

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

void text_cursor_init(text_cursor *c)
{
    memset(c, 0, sizeof *c);
}

static void clear_snaps(text_snapshot *v, int *n)
{
    for (int i = 0; i < *n; i++)
        free(v[i].text);
    *n = 0;
}

void text_cursor_free(text_cursor *c)
{
    clear_snaps(c->undo, &c->nundo);
    clear_snaps(c->redo, &c->nredo);
    free(c->undo);
    free(c->redo);
    memset(c, 0, sizeof *c);
}

static void push_snap(text_snapshot **v, int *n, int *cap, char *text, int cursor)
{
    if (*n == *cap)
    {
        *cap = *cap ? *cap * 2 : 16;
        *v = realloc(*v, (size_t)*cap * sizeof(text_snapshot));
    }
    (*v)[(*n)++] = (text_snapshot){ text, cursor };
}

/* Letters and digits in any script count as word characters, and '_'. Outside
 * ASCII this is an approximation of Rust's char::is_alphanumeric: punctuation,
 * symbol and emoji blocks are not words, everything else is. */
static int is_word(int c)
{
    if (c < 0x80)
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    if (c < 0xC0)
        return c == 0xAA || c == 0xB2 || c == 0xB3 || c == 0xB5 || c == 0xB9 || c == 0xBA || (c >= 0xBC && c <= 0xBE);
    if (c == 0xD7 || c == 0xF7)
        return 0;
    if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x2190 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFE30 && c <= 0xFE4F)
        || (c >= 0xFF00 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || (c >= 0x1F000 && c <= 0x1FAFF))
        return 0;
    return 1;
}

/* The text's characters as code points; *n gets how many */
static int *chars_of(const char *text, int *n)
{
    int count = utf8_count(text), len;
    int *v = malloc(((size_t)count + 1) * sizeof(int));
    for (int i = 0; i < count; i++)
    {
        v[i] = utf8_decode(text, &len);
        text += len;
    }
    *n = count;
    return v;
}

/* Start of the word before i (skipping spaces first), like Alt+Left */
static int word_start(const char *text, int i)
{
    int n, j = i;
    int *ch = chars_of(text, &n);
    while (j > 0 && !is_word(ch[j - 1]))
        j--;
    while (j > 0 && is_word(ch[j - 1]))
        j--;
    free(ch);
    return j;
}

/* End of the word after i, like Alt+Right */
static int word_end(const char *text, int i)
{
    int n, j = i;
    int *ch = chars_of(text, &n);
    while (j < n && !is_word(ch[j]))
        j++;
    while (j < n && is_word(ch[j]))
        j++;
    free(ch);
    return j;
}

char *text_cursor_selected(const text_cursor *c, const char *text)
{
    size_t a = utf8_byte(text, text_cursor_start(c)), b = utf8_byte(text, text_cursor_end(c));
    char *s = malloc(b - a + 1);
    memcpy(s, text + a, b - a);
    s[b - a] = 0;
    return s;
}

static void remember(text_cursor *c, const char *text)
{
    push_snap(&c->undo, &c->nundo, &c->capundo, dup_str(text), c->cursor);
    if (c->nundo > 500)
    {
        free(c->undo[0].text);
        memmove(c->undo, c->undo + 1, (size_t)(c->nundo - 1) * sizeof(text_snapshot));
        c->nundo--;
    }
    clear_snaps(c->redo, &c->nredo);
}

static void remove_range(text_cursor *c, char **text, int start, int end)
{
    size_t a = utf8_byte(*text, start), b = utf8_byte(*text, end);
    memmove(*text + a, *text + b, strlen(*text + b) + 1);
    c->cursor = c->anchor = start;
    c->has_goal = 0;
}

void text_cursor_insert(text_cursor *c, char **text, const char *s)
{
    if (!*s && !text_cursor_has_selection(c))
        return;
    remember(c, *text);
    remove_range(c, text, text_cursor_start(c), text_cursor_end(c));
    size_t at = utf8_byte(*text, c->cursor), len = strlen(*text), add = strlen(s);
    char *t = realloc(*text, len + add + 1);
    if (!t)
        return;
    memmove(t + at + add, t + at, len - at + 1);
    memcpy(t + at, s, add);
    *text = t;
    c->cursor += utf8_count(s);
    c->anchor = c->cursor;
}

void text_cursor_delete_selection(text_cursor *c, char **text)
{
    if (text_cursor_has_selection(c))
    {
        remember(c, *text);
        remove_range(c, text, text_cursor_start(c), text_cursor_end(c));
    }
}

void text_cursor_backspace(text_cursor *c, char **text, const caret_layout *layout, int word, int line)
{
    if (text_cursor_has_selection(c))
    {
        text_cursor_delete_selection(c, text);
        return;
    }
    int start;
    if (line)
    {
        int l, col;
        caret_line_col(layout, c->cursor, &l, &col);
        start = caret_index(layout, l, 0);
    }
    else if (word)
        start = word_start(*text, c->cursor);
    else
        start = c->cursor > 0 ? c->cursor - 1 : 0;
    if (start < c->cursor)
    {
        remember(c, *text);
        remove_range(c, text, start, c->cursor);
    }
}

void text_cursor_delete(text_cursor *c, char **text, int word)
{
    if (text_cursor_has_selection(c))
    {
        text_cursor_delete_selection(c, text);
        return;
    }
    int len = utf8_count(*text);
    int end = word ? word_end(*text, c->cursor) : (c->cursor + 1 < len ? c->cursor + 1 : len);
    if (end > c->cursor)
    {
        remember(c, *text);
        remove_range(c, text, c->cursor, end);
    }
}

void text_cursor_move_to(text_cursor *c, int index, int extend)
{
    c->cursor = index;
    if (!extend)
        c->anchor = index;
    c->has_goal = 0;
}

void text_cursor_left(text_cursor *c, const char *text, int extend, int word)
{
    if (text_cursor_has_selection(c) && !extend)
    {
        text_cursor_move_to(c, text_cursor_start(c), 0);
        return;
    }
    text_cursor_move_to(c, word ? word_start(text, c->cursor) : (c->cursor > 0 ? c->cursor - 1 : 0), extend);
}

void text_cursor_right(text_cursor *c, const char *text, int extend, int word)
{
    if (text_cursor_has_selection(c) && !extend)
    {
        text_cursor_move_to(c, text_cursor_end(c), 0);
        return;
    }
    int len = utf8_count(text);
    text_cursor_move_to(c, word ? word_end(text, c->cursor) : (c->cursor + 1 < len ? c->cursor + 1 : len), extend);
}

void text_cursor_line_start(text_cursor *c, const caret_layout *layout, int extend)
{
    int l, col;
    caret_line_col(layout, c->cursor, &l, &col);
    text_cursor_move_to(c, caret_index(layout, l, 0), extend);
}

void text_cursor_line_end(text_cursor *c, const caret_layout *layout, int extend)
{
    int l, col;
    caret_line_col(layout, c->cursor, &l, &col);
    text_cursor_move_to(c, caret_index(layout, l, caret_line_len(layout, l) - 1), extend);
}

void text_cursor_vertical(text_cursor *c, const char *text, const caret_layout *layout, int delta, int extend)
{
    int l, col;
    caret_line_col(layout, c->cursor, &l, &col);
    float x = c->has_goal ? c->goal_x : caret_pos(layout, c->cursor).x;
    int target = l + delta, to;
    if (target < 0)
        to = 0;
    else if (target >= layout->nlines)
        to = utf8_count(text);
    else
        to = caret_index(layout, target, caret_col_at(layout, target, x));
    text_cursor_move_to(c, to, extend);
    c->has_goal = 1;
    c->goal_x = x;
}

void text_cursor_select_all(text_cursor *c, const char *text)
{
    c->anchor = 0;
    c->cursor = utf8_count(text);
}

void text_cursor_undo(text_cursor *c, char **text)
{
    if (!c->nundo)
        return;
    text_snapshot s = c->undo[--c->nundo];
    push_snap(&c->redo, &c->nredo, &c->capredo, *text, c->cursor);
    *text = s.text;
    text_cursor_move_to(c, s.cursor, 0);
}

void text_cursor_redo(text_cursor *c, char **text)
{
    if (!c->nredo)
        return;
    text_snapshot s = c->redo[--c->nredo];
    push_snap(&c->undo, &c->nundo, &c->capundo, *text, c->cursor);
    *text = s.text;
    text_cursor_move_to(c, s.cursor, 0);
}
