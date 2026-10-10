//! Text tool: system font discovery and glyph rasterization into a coverage mask.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use ab_glyph::{Font, FontVec, GlyphId, PxScale, ScaleFont, point};
use eframe::egui::{Pos2, pos2};

use crate::raster::{IRect, Mask};

#[derive(Clone, Debug, PartialEq)]
pub struct TextStyle {
    pub family: usize,
    pub size_pt: f32,
    pub bold: bool,
    pub italic: bool,
    pub underline: bool,
    pub strike: bool,
    pub opaque: bool,
}

impl TextStyle {
    /// Point size at 96 DPI, like Windows.
    pub fn px(&self) -> f32 {
        self.size_pt * 96.0 / 72.0
    }
}

pub struct FontFamily {
    pub name: String,
    regular: Option<PathBuf>,
    bold: Option<PathBuf>,
    italic: Option<PathBuf>,
    bold_italic: Option<PathBuf>,
}

#[derive(Clone)]
pub struct LoadedFont {
    font: Rc<FontVec>,
    /// The family has no real bold/italic face, so fake it.
    fake_bold: bool,
    fake_italic: bool,
}

pub struct FontLibrary {
    pub families: Vec<FontFamily>,
    cache: HashMap<PathBuf, Option<Rc<FontVec>>>,
}

impl FontLibrary {
    pub fn scan() -> Self {
        let mut dirs = vec![
            PathBuf::from("/System/Library/Fonts/Supplemental"),
            PathBuf::from("/System/Library/Fonts"),
            PathBuf::from("/Library/Fonts"),
        ];
        if let Some(home) = std::env::var_os("HOME") {
            dirs.push(Path::new(&home).join("Library/Fonts"));
        }
        let mut by_name: HashMap<String, FontFamily> = HashMap::new();
        for dir in dirs {
            let Ok(entries) = std::fs::read_dir(&dir) else { continue };
            for entry in entries.flatten() {
                let path = entry.path();
                let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase();
                if !matches!(ext.as_str(), "ttf" | "otf" | "ttc") {
                    continue;
                }
                let Some(stem) = path.file_stem().and_then(|s| s.to_str()) else { continue };
                if stem.starts_with('.') {
                    continue;
                }
                let (name, bold, italic) = split_style(stem);
                let fam = by_name.entry(name.to_string()).or_insert_with(|| FontFamily {
                    name: name.to_string(),
                    regular: None,
                    bold: None,
                    italic: None,
                    bold_italic: None,
                });
                let slot = match (bold, italic) {
                    (false, false) => &mut fam.regular,
                    (true, false) => &mut fam.bold,
                    (false, true) => &mut fam.italic,
                    (true, true) => &mut fam.bold_italic,
                };
                slot.get_or_insert(path);
            }
        }
        let mut families: Vec<FontFamily> = by_name.into_values().filter(|f| f.regular.is_some()).collect();
        families.sort_by_key(|f| f.name.to_lowercase());
        Self { families, cache: HashMap::new() }
    }

    /// Calibri is Paint's default; on a Mac fall back to the closest common face.
    pub fn default_family(&self) -> usize {
        ["Calibri", "Arial", "Helvetica", "Verdana"]
            .iter()
            .find_map(|want| self.families.iter().position(|f| f.name == *want))
            .unwrap_or(0)
    }

    pub fn load(&mut self, family: usize, bold: bool, italic: bool) -> Option<LoadedFont> {
        let fam = self.families.get(family)?;
        let exact = match (bold, italic) {
            (false, false) => fam.regular.clone(),
            (true, false) => fam.bold.clone(),
            (false, true) => fam.italic.clone(),
            (true, true) => fam.bold_italic.clone(),
        };
        let (path, fake_bold, fake_italic) = match exact {
            Some(p) => (p, false, false),
            None if bold && italic && fam.bold.is_some() => (fam.bold.clone()?, false, true),
            None if bold && italic && fam.italic.is_some() => (fam.italic.clone()?, true, false),
            None => (fam.regular.clone()?, bold, italic),
        };
        let font = self
            .cache
            .entry(path.clone())
            .or_insert_with(|| {
                let data = std::fs::read(&path).ok()?;
                FontVec::try_from_vec_and_index(data, 0).ok().map(Rc::new)
            })
            .clone()?;
        Some(LoadedFont { font, fake_bold, fake_italic })
    }
}

fn split_style(stem: &str) -> (&str, bool, bool) {
    for (suffix, b, i) in [
        (" Bold Italic", true, true),
        (" BoldItalic", true, true),
        (" Bold Oblique", true, true),
        (" Bold", true, false),
        (" Italic", false, true),
        (" Oblique", false, true),
    ] {
        if let Some(name) = stem.strip_suffix(suffix) {
            return (name, b, i);
        }
    }
    (stem, false, false)
}

pub struct TextLayout {
    pub width: f32,
    pub height: f32,
}

const ITALIC_SHEAR: f32 = 0.2;

pub fn measure(font: &LoadedFont, px: f32, text: &str) -> TextLayout {
    let scaled = font.font.as_scaled(PxScale::from(px));
    let line_h = scaled.ascent() - scaled.descent() + scaled.line_gap();
    let mut width: f32 = 0.0;
    let mut lines = 0;
    for line in text.split('\n') {
        lines += 1;
        width = width.max(line_width(&scaled, line));
    }
    if font.fake_italic {
        width += px * ITALIC_SHEAR;
    }
    if font.fake_bold {
        width += bold_offset(px) as f32;
    }
    TextLayout { width, height: lines as f32 * line_h }
}

fn line_width<F: Font>(scaled: &impl ScaleFont<F>, line: &str) -> f32 {
    let mut x = 0.0;
    let mut prev: Option<GlyphId> = None;
    for ch in line.chars() {
        let id = scaled.glyph_id(ch);
        if let Some(p) = prev {
            x += scaled.kern(p, id);
        }
        x += scaled.h_advance(id);
        prev = Some(id);
    }
    x
}

fn bold_offset(px: f32) -> i32 {
    (px / 28.0).ceil().max(1.0) as i32
}

/// Rasterize `text` with its top-left at `origin`. Returns the touched pixels.
pub fn render(mask: &mut Mask, font: &LoadedFont, px: f32, text: &str, origin: Pos2, underline: bool, strike: bool) -> IRect {
    let scaled = font.font.as_scaled(PxScale::from(px));
    let ascent = scaled.ascent();
    let line_h = ascent - scaled.descent() + scaled.line_gap();
    let bold = if font.fake_bold { bold_offset(px) } else { 0 };
    let mut out = IRect::EMPTY;
    for (li, line) in text.split('\n').enumerate() {
        let baseline = origin.y + ascent + li as f32 * line_h;
        let mut x = origin.x;
        let mut prev: Option<GlyphId> = None;
        for ch in line.chars() {
            let id = scaled.glyph_id(ch);
            if let Some(p) = prev {
                x += scaled.kern(p, id);
            }
            let glyph = id.with_scale_and_position(px, point(x, baseline));
            x += scaled.h_advance(id);
            prev = Some(id);
            let Some(outlined) = font.font.outline_glyph(glyph) else { continue };
            let b = outlined.px_bounds();
            let (bx, by) = (b.min.x as i32, b.min.y as i32);
            let shear_of = |py: i32| {
                if font.fake_italic { ((baseline - py as f32) * ITALIC_SHEAR).round() as i32 } else { 0 }
            };
            outlined.draw(|gx, gy, c| {
                let py = by + gy as i32;
                let px_ = bx + gx as i32 + shear_of(py);
                for k in 0..=bold {
                    mask.max_at(px_ + k, py, c);
                }
            });
            let top = shear_of(by);
            let bottom = shear_of(b.max.y as i32);
            out = out.union(IRect::new(
                bx + top.min(bottom) - 1,
                by - 1,
                b.max.x.ceil() as i32 + top.max(bottom) + bold + 1,
                b.max.y.ceil() as i32 + 1,
            ));
        }
        let thick = (px / 16.0).max(1.0);
        let mut rule = |y: f32| mask.fill_polygon(&[pos2(origin.x, y), pos2(x, y), pos2(x, y + thick), pos2(origin.x, y + thick)], 1.0);
        if underline && x > origin.x {
            out = out.union(rule(baseline + (px * 0.1).max(1.0)));
        }
        if strike && x > origin.x {
            out = out.union(rule(baseline - px * 0.3));
        }
    }
    out.intersect(mask.bounds())
}

/// Where the caret can sit: for each line, the x offset before every character
/// and after the last one, plus the line height. Offsets are from the text's origin.
#[derive(Clone, Debug)]
pub struct CaretLayout {
    pub lines: Vec<Vec<f32>>,
    pub line_h: f32,
}

impl Default for CaretLayout {
    fn default() -> Self {
        Self { lines: vec![vec![0.0]], line_h: 16.0 }
    }
}

impl CaretLayout {
    /// Line and column of a character index (newlines count as characters).
    pub fn line_col(&self, index: usize) -> (usize, usize) {
        let mut i = index;
        for (l, xs) in self.lines.iter().enumerate() {
            let len = xs.len() - 1;
            if i <= len {
                return (l, i);
            }
            i -= len + 1;
        }
        let last = self.lines.len() - 1;
        (last, self.lines[last].len() - 1)
    }

    pub fn index(&self, line: usize, col: usize) -> usize {
        self.lines[..line].iter().map(|xs| xs.len()).sum::<usize>() + col
    }

    /// Top-left of the caret at `index`, relative to the origin.
    pub fn caret(&self, index: usize) -> eframe::egui::Vec2 {
        let (l, c) = self.line_col(index);
        eframe::egui::vec2(self.lines[l][c], l as f32 * self.line_h)
    }

    /// Nearest caret position on `line` to `x`.
    pub fn col_at(&self, line: usize, x: f32) -> usize {
        let xs = &self.lines[line];
        (0..xs.len()).min_by(|&a, &b| (xs[a] - x).abs().total_cmp(&(xs[b] - x).abs())).unwrap_or(0)
    }

    /// Nearest caret index to a point relative to the origin.
    pub fn index_at(&self, p: eframe::egui::Vec2) -> usize {
        let line = ((p.y / self.line_h).floor().max(0.0) as usize).min(self.lines.len() - 1);
        self.index(line, self.col_at(line, p.x))
    }
}

pub fn caret_layout(font: &LoadedFont, px: f32, text: &str) -> CaretLayout {
    let scaled = font.font.as_scaled(PxScale::from(px));
    let line_h = scaled.ascent() - scaled.descent() + scaled.line_gap();
    let lines = text
        .split('\n')
        .map(|line| {
            let mut xs = vec![0.0];
            let mut x = 0.0;
            let mut prev: Option<GlyphId> = None;
            for ch in line.chars() {
                let id = scaled.glyph_id(ch);
                if let Some(p) = prev {
                    x += scaled.kern(p, id);
                }
                x += scaled.h_advance(id);
                xs.push(x);
                prev = Some(id);
            }
            xs
        })
        .collect();
    CaretLayout { lines, line_h }
}
