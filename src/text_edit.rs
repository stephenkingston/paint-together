//! Caret, selection and undo for text typed straight onto the canvas.

use std::ops::Range;

use crate::text::CaretLayout;

#[derive(Clone, Debug, Default)]
pub struct TextCursor {
    /// Character index of the caret.
    pub cursor: usize,
    /// The other end of the selection (equal to `cursor` when nothing is selected).
    pub anchor: usize,
    /// Column to aim for when moving up and down.
    goal_x: Option<f32>,
    undo: Vec<(String, usize)>,
    redo: Vec<(String, usize)>,
}

fn byte(text: &str, index: usize) -> usize {
    text.char_indices().nth(index).map_or(text.len(), |(b, _)| b)
}

fn is_word(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

/// Start of the word before `i` (skipping spaces first), like ⌥←.
fn word_start(text: &str, i: usize) -> usize {
    let chars: Vec<char> = text.chars().collect();
    let mut j = i;
    while j > 0 && !is_word(chars[j - 1]) {
        j -= 1;
    }
    while j > 0 && is_word(chars[j - 1]) {
        j -= 1;
    }
    j
}

/// End of the word after `i`, like ⌥→.
fn word_end(text: &str, i: usize) -> usize {
    let chars: Vec<char> = text.chars().collect();
    let mut j = i;
    while j < chars.len() && !is_word(chars[j]) {
        j += 1;
    }
    while j < chars.len() && is_word(chars[j]) {
        j += 1;
    }
    j
}

impl TextCursor {
    pub fn range(&self) -> Range<usize> {
        self.cursor.min(self.anchor)..self.cursor.max(self.anchor)
    }

    pub fn has_selection(&self) -> bool {
        self.cursor != self.anchor
    }

    pub fn selected<'a>(&self, text: &'a str) -> &'a str {
        let r = self.range();
        &text[byte(text, r.start)..byte(text, r.end)]
    }

    fn remember(&mut self, text: &str) {
        self.undo.push((text.to_owned(), self.cursor));
        if self.undo.len() > 500 {
            self.undo.remove(0);
        }
        self.redo.clear();
    }

    fn remove(&mut self, text: &mut String, r: Range<usize>) {
        text.replace_range(byte(text, r.start)..byte(text, r.end), "");
        self.cursor = r.start;
        self.anchor = r.start;
        self.goal_x = None;
    }

    /// Type `s`, replacing the selection.
    pub fn insert(&mut self, text: &mut String, s: &str) {
        if s.is_empty() && !self.has_selection() {
            return;
        }
        self.remember(text);
        let r = self.range();
        self.remove(text, r);
        text.insert_str(byte(text, self.cursor), s);
        self.cursor += s.chars().count();
        self.anchor = self.cursor;
    }

    pub fn delete_selection(&mut self, text: &mut String) {
        if self.has_selection() {
            self.remember(text);
            let r = self.range();
            self.remove(text, r);
        }
    }

    /// Delete before the caret: a character, a word (⌥) or to the line start (⌘).
    pub fn backspace(&mut self, text: &mut String, layout: &CaretLayout, word: bool, line: bool) {
        if self.has_selection() {
            return self.delete_selection(text);
        }
        let start = if line {
            let (l, _) = layout.line_col(self.cursor);
            layout.index(l, 0)
        } else if word {
            word_start(text, self.cursor)
        } else {
            self.cursor.saturating_sub(1)
        };
        if start < self.cursor {
            self.remember(text);
            self.remove(text, start..self.cursor);
        }
    }

    /// Delete after the caret: a character or a word (⌥).
    pub fn delete(&mut self, text: &mut String, word: bool) {
        if self.has_selection() {
            return self.delete_selection(text);
        }
        let len = text.chars().count();
        let end = if word { word_end(text, self.cursor) } else { (self.cursor + 1).min(len) };
        if end > self.cursor {
            self.remember(text);
            self.remove(text, self.cursor..end);
        }
    }

    pub fn move_to(&mut self, index: usize, extend: bool) {
        self.cursor = index;
        if !extend {
            self.anchor = index;
        }
        self.goal_x = None;
    }

    pub fn left(&mut self, text: &str, extend: bool, word: bool) {
        if self.has_selection() && !extend {
            return self.move_to(self.range().start, false);
        }
        let to = if word { word_start(text, self.cursor) } else { self.cursor.saturating_sub(1) };
        self.move_to(to, extend);
    }

    pub fn right(&mut self, text: &str, extend: bool, word: bool) {
        if self.has_selection() && !extend {
            return self.move_to(self.range().end, false);
        }
        let len = text.chars().count();
        let to = if word { word_end(text, self.cursor) } else { (self.cursor + 1).min(len) };
        self.move_to(to, extend);
    }

    pub fn line_start(&mut self, layout: &CaretLayout, extend: bool) {
        let (l, _) = layout.line_col(self.cursor);
        self.move_to(layout.index(l, 0), extend);
    }

    pub fn line_end(&mut self, layout: &CaretLayout, extend: bool) {
        let (l, _) = layout.line_col(self.cursor);
        self.move_to(layout.index(l, layout.lines[l].len() - 1), extend);
    }

    /// Move `delta` lines up (negative) or down, keeping the column.
    pub fn vertical(&mut self, text: &str, layout: &CaretLayout, delta: isize, extend: bool) {
        let (l, _) = layout.line_col(self.cursor);
        let x = self.goal_x.unwrap_or_else(|| layout.caret(self.cursor).x);
        let target = l as isize + delta;
        let to = if target < 0 {
            0
        } else if target as usize >= layout.lines.len() {
            text.chars().count()
        } else {
            layout.index(target as usize, layout.col_at(target as usize, x))
        };
        self.move_to(to, extend);
        self.goal_x = Some(x);
    }

    pub fn select_all(&mut self, text: &str) {
        self.anchor = 0;
        self.cursor = text.chars().count();
    }

    pub fn undo(&mut self, text: &mut String) {
        if let Some((t, c)) = self.undo.pop() {
            self.redo.push((std::mem::replace(text, t), self.cursor));
            self.move_to(c, false);
        }
    }

    pub fn redo(&mut self, text: &mut String) {
        if let Some((t, c)) = self.redo.pop() {
            self.undo.push((std::mem::replace(text, t), self.cursor));
            self.move_to(c, false);
        }
    }
}
