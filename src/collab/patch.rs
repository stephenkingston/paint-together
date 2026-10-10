//! Edits as they travel between kids: the pixels that changed, never the
//! commands that changed them. A brush, a font or a maths library that differs
//! between two computers therefore can't make their pictures drift apart.

use image::RgbaImage;

use crate::history::Change;
use crate::raster::IRect;

/// Some changed pixels inside a rectangle. Pixels outside the mask are left
/// alone, so two kids drawing side by side never wipe out each other's work.
#[derive(Clone, Debug, PartialEq, bincode::Encode, bincode::Decode)]
pub struct Patch {
    pub x: i32,
    pub y: i32,
    pub w: u32,
    pub h: u32,
    /// One bit per pixel of the rectangle, row by row: set where the pixel changed.
    pub mask: Vec<u8>,
    /// RGBA for each set bit, in the same order.
    pub pixels: Vec<u8>,
}

/// A whole picture (for edits that change its size, or replace it outright).
#[derive(Clone, Debug, PartialEq, bincode::Encode, bincode::Decode)]
pub struct Picture {
    pub w: u32,
    pub h: u32,
    pub rgba: Vec<u8>,
}

#[derive(Clone, Debug, PartialEq, bincode::Encode, bincode::Decode)]
pub enum Edit {
    Patch(Patch),
    Replace(Picture),
}

/// Which pixels an edit touched.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Region {
    Rect(IRect),
    /// The whole picture, possibly at a new size.
    Whole,
}

/// The largest picture anyone may share (in pixels per side).
pub const MAX_SIDE: u32 = 16_384;

impl Picture {
    /// Holds exactly the bytes its size needs, and a size Paint can open.
    pub fn is_valid(&self) -> bool {
        (1..=MAX_SIDE).contains(&self.w) && (1..=MAX_SIDE).contains(&self.h) && self.rgba.len() as u64 == self.w as u64 * self.h as u64 * 4
    }

    pub fn of(img: &RgbaImage) -> Self {
        Self { w: img.width(), h: img.height(), rgba: img.as_raw().clone() }
    }

    pub fn to_image(&self) -> Option<RgbaImage> {
        RgbaImage::from_raw(self.w, self.h, self.rgba.clone())
    }
}

impl Patch {
    /// Its mask and pixels agree with its size (anything from the network is checked).
    pub fn is_valid(&self) -> bool {
        let n = self.w as u64 * self.h as u64;
        self.w <= MAX_SIDE
            && self.h <= MAX_SIDE
            && self.x.unsigned_abs() <= MAX_SIDE
            && self.y.unsigned_abs() <= MAX_SIDE
            && self.mask.len() as u64 == n.div_ceil(8)
            && self.pixels.len() as u64 == 4 * self.mask.iter().map(|b| b.count_ones() as u64).sum::<u64>()
    }

    pub fn rect(&self) -> IRect {
        IRect::new(self.x, self.y, self.x + self.w as i32, self.y + self.h as i32)
    }

    fn bit(&self, i: usize) -> bool {
        self.mask[i / 8] & (1 << (i % 8)) != 0
    }

    /// Pixels inside `r` that differ between two same-sized images.
    pub fn diff(before: &RgbaImage, after: &RgbaImage, r: IRect) -> Option<Patch> {
        let r = r.intersect(IRect::of_image(before)).intersect(IRect::of_image(after));
        if r.is_empty() {
            return None;
        }
        Self::build(r, |x, y| {
            let a = after.get_pixel(x as u32, y as u32).0;
            (before.get_pixel(x as u32, y as u32).0 != a).then_some(a)
        })
    }

    /// A patch over `r` holding `pick(x, y)` wherever it returns a pixel.
    pub fn build(r: IRect, mut pick: impl FnMut(i32, i32) -> Option<[u8; 4]>) -> Option<Patch> {
        let (w, h) = (r.width() as u32, r.height() as u32);
        let mut mask = vec![0u8; (w as usize * h as usize).div_ceil(8)];
        let mut pixels = Vec::new();
        let mut i = 0;
        for y in r.y0..r.y1 {
            for x in r.x0..r.x1 {
                if let Some(px) = pick(x, y) {
                    mask[i / 8] |= 1 << (i % 8);
                    pixels.extend_from_slice(&px);
                }
                i += 1;
            }
        }
        (!pixels.is_empty()).then_some(Patch { x: r.x0, y: r.y0, w, h, mask, pixels })
    }

    /// Write the changed pixels that fall inside `clip` (and the image).
    pub fn apply_within(&self, img: &mut RgbaImage, clip: IRect) {
        let clip = clip.intersect(IRect::of_image(img)).intersect(self.rect());
        if clip.is_empty() {
            return;
        }
        let (w, h) = (self.w as i32, self.h as i32);
        let iw = img.width() as usize;
        let buf: &mut [u8] = img;
        let mut src = 0;
        for j in 0..h {
            let y = self.y + j;
            for i in 0..w {
                if !self.bit((j * w + i) as usize) {
                    continue;
                }
                let x = self.x + i;
                if x >= clip.x0 && x < clip.x1 && y >= clip.y0 && y < clip.y1 {
                    let d = (y as usize * iw + x as usize) * 4;
                    buf[d..d + 4].copy_from_slice(&self.pixels[src..src + 4]);
                }
                src += 4;
            }
        }
    }

    /// Each changed pixel with its new value.
    pub fn each(&self, mut f: impl FnMut(i32, i32, [u8; 4])) {
        let w = self.w as i32;
        let mut src = 0;
        for j in 0..self.h as i32 {
            for i in 0..w {
                if self.bit((j * w + i) as usize) {
                    let p = &self.pixels[src..src + 4];
                    f(self.x + i, self.y + j, [p[0], p[1], p[2], p[3]]);
                    src += 4;
                }
            }
        }
    }
}

impl Edit {
    pub fn is_valid(&self) -> bool {
        match self {
            Edit::Patch(p) => p.is_valid(),
            Edit::Replace(pic) => pic.is_valid(),
        }
    }

    /// The edit a finished local change sends to everyone else.
    pub fn from_change(change: &Change) -> Option<Edit> {
        match change {
            Change::Patch { x, y, before, after } => {
                let r = IRect::new(*x as i32, *y as i32, (*x + after.width()) as i32, (*y + after.height()) as i32);
                Patch::build(r, |px, py| {
                    let (i, j) = ((px - *x as i32) as u32, (py - *y as i32) as u32);
                    let a = after.get_pixel(i, j).0;
                    (before.get_pixel(i, j).0 != a).then_some(a)
                })
                .map(Edit::Patch)
            }
            Change::Replace { after, .. } => Some(Edit::Replace(Picture::of(after))),
        }
    }

    pub fn region(&self, img: &RgbaImage) -> Region {
        match self {
            Edit::Patch(p) => Region::Rect(p.rect().intersect(IRect::of_image(img))),
            Edit::Replace(_) => Region::Whole,
        }
    }

    pub fn apply(&self, img: &mut RgbaImage) {
        match self {
            Edit::Patch(p) => p.apply_within(img, IRect::of_image(img)),
            Edit::Replace(pic) => {
                if let Some(new) = pic.to_image() {
                    *img = new;
                }
            }
        }
    }
}

/// Undo (or redo) one of my own changes without disturbing anyone else's: a
/// pixel goes back only while the picture still shows what my change left there.
/// Whole-picture changes come back whole; the caller decides whether that's allowed.
pub fn revert(change: &Change, current: &RgbaImage, undo: bool) -> Option<Edit> {
    match change {
        Change::Patch { x, y, before, after } => {
            let (from, to) = if undo { (after, before) } else { (before, after) };
            let r = IRect::new(*x as i32, *y as i32, (*x + after.width()) as i32, (*y + after.height()) as i32)
                .intersect(IRect::of_image(current));
            if r.is_empty() {
                return None;
            }
            Patch::build(r, |px, py| {
                let (i, j) = ((px - *x as i32) as u32, (py - *y as i32) as u32);
                let (f, t) = (from.get_pixel(i, j).0, to.get_pixel(i, j).0);
                (f != t && current.get_pixel(px as u32, py as u32).0 == f).then_some(t)
            })
            .map(Edit::Patch)
        }
        Change::Replace { before, after } => Some(Edit::Replace(Picture::of(if undo { before } else { after }))),
    }
}
