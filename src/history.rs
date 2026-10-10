//! Undo/redo. Most edits store only the before/after pixels of the rectangle
//! they touched; edits that change the canvas size store whole images.

use image::{RgbaImage, imageops};

use crate::raster::{self, IRect};

/// Total pixel bytes kept for undo before the oldest steps are dropped.
const BUDGET_BYTES: usize = 1 << 30;

pub enum Change {
    Patch { x: u32, y: u32, before: RgbaImage, after: RgbaImage },
    Replace { before: RgbaImage, after: RgbaImage },
}

impl Change {
    /// The difference between two same-sized images within `r`, if any.
    pub fn patch(before: &RgbaImage, after: &RgbaImage, r: IRect) -> Option<Change> {
        let r = r.intersect(IRect::of_image(before)).intersect(IRect::of_image(after));
        if r.is_empty() {
            return None;
        }
        let b = raster::crop(before, r);
        let a = raster::crop(after, r);
        (b != a).then_some(Change::Patch { x: r.x0 as u32, y: r.y0 as u32, before: b, after: a })
    }

    fn bytes(&self) -> usize {
        match self {
            Change::Patch { before, after, .. } | Change::Replace { before, after } => before.len() + after.len(),
        }
    }

    fn apply(&self, img: &mut RgbaImage, undo: bool) {
        match self {
            Change::Patch { x, y, before, after } => {
                imageops::replace(img, if undo { before } else { after }, *x as i64, *y as i64);
            }
            Change::Replace { before, after } => *img = if undo { before } else { after }.clone(),
        }
    }
}

struct Edit {
    id: u64,
    change: Change,
}

#[derive(Default)]
pub struct History {
    undo: Vec<Edit>,
    redo: Vec<Edit>,
    next_id: u64,
}

impl History {
    pub fn push(&mut self, change: Change) {
        self.redo.clear();
        self.next_id += 1;
        self.undo.push(Edit { id: self.next_id, change });
        while self.undo.len() > 1 && self.undo.iter().map(|e| e.change.bytes()).sum::<usize>() > BUDGET_BYTES {
            self.undo.remove(0);
        }
    }

    pub fn undo(&mut self, img: &mut RgbaImage) -> bool {
        let Some(edit) = self.undo.pop() else { return false };
        edit.change.apply(img, true);
        self.redo.push(edit);
        true
    }

    pub fn redo(&mut self, img: &mut RgbaImage) -> bool {
        let Some(edit) = self.redo.pop() else { return false };
        edit.change.apply(img, false);
        self.undo.push(edit);
        true
    }

    /// Undo (`back`) or redo one step, but only if `apply` performs it.
    pub fn step_with(&mut self, back: bool, apply: impl FnOnce(&Change) -> bool) -> bool {
        let (from, to) = if back { (&mut self.undo, &mut self.redo) } else { (&mut self.redo, &mut self.undo) };
        let Some(edit) = from.last() else {
            return false;
        };
        if !apply(&edit.change) {
            return false;
        }
        to.push(from.pop().expect("checked above"));
        true
    }

    /// Identifies the step `redo` would bring back.
    pub fn redo_id(&self) -> u64 {
        self.redo.last().map_or(0, |e| e.id)
    }

    pub fn can_undo(&self) -> bool {
        !self.undo.is_empty()
    }

    pub fn can_redo(&self) -> bool {
        !self.redo.is_empty()
    }

    /// Identifies the current document state, for "unsaved changes" tracking.
    pub fn state_id(&self) -> u64 {
        self.undo.last().map_or(0, |e| e.id)
    }

    pub fn clear(&mut self) {
        self.undo.clear();
        self.redo.clear();
    }
}
