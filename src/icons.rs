//! Classic Paint-style colour icons, drawn as vectors on a 20×20 grid so the same
//! artwork serves 16px (small) and 32px (large) ribbon buttons.

use std::f32::consts::{PI, TAU};

use eframe::egui::{
    Color32, ColorImage, Context, FontId, Id, Painter, Pos2, Rect, Shape, Stroke, StrokeKind, TextureHandle, TextureOptions, Vec2,
    ecolor::Hsva, pos2, vec2,
};
use image::RgbaImage;

use crate::raster::{self, IRect, Mask};
use crate::shapes::ShapeKind;
use crate::theme;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Icon {
    Paste,
    Cut,
    Copy,
    Select,
    FreeSelect,
    SelectAll,
    Delete,
    Crop,
    Resize,
    Rotate,
    RotateLeft,
    Rotate180,
    FlipVertical,
    FlipHorizontal,
    Pencil,
    Fill,
    Text,
    Eraser,
    Picker,
    Magnifier,
    Brushes,
    Size,
    EditColors,
    /// Shape outline: its colour, or `None` for no outline.
    Outline(Option<Color32>),
    /// Shape fill: its colour, or `None` for no fill.
    FillShape(Option<Color32>),
    ZoomIn,
    ZoomOut,
    Zoom100,
    FullScreen,
    New,
    Open,
    Save,
    SaveAs,
    Properties,
    About,
    Exit,
    Undo,
    Redo,
    Help,
    Collapse,
    Dropdown,
    ScrollUp,
    ScrollDown,
    GalleryMore,
    Check,
    CursorPos,
    SelectionSize,
    CanvasSize,
    FileSize,
    Opaque,
    Transparent,
    Bold,
    Italic,
    Underline,
    Strike,
    FileType(FileKind),
    Shape(ShapeKind),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FileKind {
    Png,
    Jpeg,
    Bmp,
    Gif,
    Other,
}

const fn hex(rgb: u32) -> Color32 {
    Color32::from_rgb((rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8)
}

const INK: Color32 = hex(0x2B2B2B);
const BLUE: Color32 = hex(0x2B78D4);
const DARK_BLUE: Color32 = hex(0x1D5AA3);
const LIGHT_BLUE: Color32 = hex(0xC4E0F7);
const YELLOW: Color32 = hex(0xF7C948);
const YELLOW_DARK: Color32 = hex(0xB98A10);
const FOLDER: Color32 = hex(0xF4C55B);
const FOLDER_BACK: Color32 = hex(0xDDA33A);
const PINK: Color32 = hex(0xF29CB0);
const WOOD: Color32 = hex(0xF3D9A6);
const BROWN: Color32 = hex(0xC08B57);
const BROWN_DARK: Color32 = hex(0x7C5230);
const SILVER: Color32 = hex(0xC3C8CE);
const SILVER_DARK: Color32 = hex(0x7D838A);
const GREEN: Color32 = hex(0x2E9B45);
const RED: Color32 = hex(0xD64541);
const PAPER: Color32 = Color32::WHITE;
const PAPER_LINE: Color32 = hex(0x9AA4AE);

/// Draws on a 20×20 grid mapped onto a square rect.
struct Pen<'a> {
    painter: &'a Painter,
    origin: Pos2,
    unit: f32,
    disabled: bool,
    /// Physical pixels per point, for snapping to the pixel grid.
    ppp: f32,
}

impl Pen<'_> {
    fn p(&self, (x, y): (f32, f32)) -> Pos2 {
        self.origin + vec2(x, y) * self.unit
    }

    /// Disabled icons are drawn faded and grey, like Windows does.
    fn c(&self, col: Color32) -> Color32 {
        if !self.disabled {
            return col;
        }
        let g = (0.3 * col.r() as f32 + 0.59 * col.g() as f32 + 0.11 * col.b() as f32) as u8;
        let g = g / 2 + 110;
        Color32::from_rgba_unmultiplied(g, g, g, (col.a() as f32 * 0.55) as u8)
    }

    fn line(&self, pts: &[(f32, f32)], color: Color32, width: f32) {
        let v: Vec<Pos2> = pts.iter().map(|&q| self.p(q)).collect();
        self.path(&v, color, width);
    }

    /// A polyline drawn as one joined stroke (no gaps or blobs at the joints).
    /// Purely horizontal/vertical paths are snapped to the pixel grid so they're crisp;
    /// open curved or diagonal strokes get round ends.
    fn path(&self, pts: &[Pos2], color: Color32, width: f32) {
        if pts.len() < 2 {
            return;
        }
        let color = self.c(color);
        let rectilinear = pts.windows(2).all(|s| (s[0].x - s[1].x).abs() < 1e-3 || (s[0].y - s[1].y).abs() < 1e-3);
        let (pts, w): (Vec<Pos2>, f32) = if rectilinear {
            let w = (width * self.unit * self.ppp).round().max(1.0) / self.ppp;
            let snap = |v: f32| self.snap(v - w / 2.0) + w / 2.0;
            (pts.iter().map(|q| pos2(snap(q.x), snap(q.y))).collect(), w)
        } else {
            (pts.to_vec(), width * self.unit)
        };
        let closed = pts.len() > 2 && (pts[0] - pts[pts.len() - 1]).length() < 1e-3;
        if closed {
            self.painter.add(Shape::closed_line(pts[..pts.len() - 1].to_vec(), Stroke::new(w, color)));
        } else {
            self.painter.add(Shape::line(pts.clone(), Stroke::new(w, color)));
            if !rectilinear && w * self.ppp > 1.6 {
                for q in [pts[0], pts[pts.len() - 1]] {
                    self.painter.circle_filled(q, w * 0.5, color);
                }
            }
        }
    }

    fn closed(&self, pts: &[(f32, f32)], color: Color32, width: f32) {
        let mut v: Vec<Pos2> = pts.iter().map(|&q| self.p(q)).collect();
        v.push(v[0]);
        self.path(&v, color, width);
    }

    /// Convex polygon, optionally outlined.
    fn poly(&self, pts: &[(f32, f32)], fill: Color32, outline: Option<Color32>) {
        let v: Vec<Pos2> = pts.iter().map(|&q| self.p(q)).collect();
        self.painter.add(Shape::convex_polygon(v, self.c(fill), Stroke::NONE));
        if let Some(o) = outline {
            self.closed(pts, o, 1.0);
        }
    }

    fn rect(&self, (x0, y0): (f32, f32), (x1, y1): (f32, f32), fill: Color32, outline: Option<Color32>) {
        self.crisp(&[(x0, y0), (x1, y0), (x1, y1), (x0, y1)], fill, outline);
    }

    /// The rect between two grid points, with its edges on pixel boundaries.
    fn snapped_rect(&self, a: (f32, f32), b: (f32, f32)) -> Rect {
        let (a, b) = (self.p(a), self.p(b));
        Rect::from_min_max(pos2(self.snap(a.x), self.snap(a.y)), pos2(self.snap(b.x), self.snap(b.y)))
    }

    /// Outline width: one physical pixel per point, so 1px at 1x and 2px on Retina.
    fn hairline(&self) -> f32 {
        self.ppp.round().max(1.0) / self.ppp
    }

    fn snap(&self, v: f32) -> f32 {
        (v * self.ppp).round() / self.ppp
    }

    /// A convex polygon on the pixel grid: the fill's edges land on pixel boundaries
    /// and the outline is one joined stroke just inside them, so corners are sharp
    /// and closed.
    fn crisp(&self, pts: &[(f32, f32)], fill: Color32, outline: Option<Color32>) {
        let b: Vec<Pos2> = pts.iter().map(|&q| self.p(q)).map(|q| pos2(self.snap(q.x), self.snap(q.y))).collect();
        self.painter.add(Shape::convex_polygon(b.clone(), self.c(fill), Stroke::NONE));
        if let Some(o) = outline {
            let n = b.len() as f32;
            let c = b.iter().fold(Vec2::ZERO, |acc, q| acc + q.to_vec2()) / n;
            let half = self.hairline() / 2.0;
            let inset: Vec<Pos2> = b.iter().map(|q| pos2(q.x + half * (c.x - q.x).signum(), q.y + half * (c.y - q.y).signum())).collect();
            self.painter.add(Shape::closed_line(inset, Stroke::new(self.hairline(), self.c(o))));
        }
    }

    /// A horizontal hairline (ruled lines on paper), centred on a pixel row.
    fn rule(&self, (x0, x1): (f32, f32), y: f32, color: Color32) {
        let w = self.hairline();
        let y = self.snap(self.p((0.0, y)).y - w / 2.0) + w / 2.0;
        let (a, b) = (self.snap(self.p((x0, 0.0)).x), self.snap(self.p((x1, 0.0)).x));
        self.painter.hline(eframe::egui::Rangef::new(a, b), y, Stroke::new(w, self.c(color)));
    }

    fn disc(&self, c: (f32, f32), r: f32, fill: Color32) {
        self.painter.circle_filled(self.p(c), r * self.unit, self.c(fill));
    }

    fn ring(&self, c: (f32, f32), r: f32, color: Color32, width: f32) {
        self.painter.circle_stroke(self.p(c), r * self.unit, Stroke::new(width * self.unit, self.c(color)));
    }

    fn dashed(&self, pts: &[(f32, f32)], color: Color32) {
        let mut v: Vec<Pos2> = pts.iter().map(|&q| self.p(q)).collect();
        // Dashed rectangles sit on pixel centres so the dashes are crisp.
        if pts.len() == 4 {
            let w = self.hairline();
            v = v.into_iter().map(|q| pos2(self.snap(q.x - w / 2.0) + w / 2.0, self.snap(q.y - w / 2.0) + w / 2.0)).collect();
        }
        v.push(v[0]);
        let width = if pts.len() == 4 { self.hairline() } else { (1.1 * self.unit).max(1.0) };
        self.painter.extend(Shape::dashed_line(&v, Stroke::new(width, self.c(color)), 2.0 * self.unit, 1.5 * self.unit));
    }

    fn arc(&self, c: (f32, f32), r: f32, from: f32, to: f32) -> Vec<Pos2> {
        let n = ((to - from).abs() / 0.15).ceil().max(2.0) as usize;
        (0..=n)
            .map(|i| {
                let t = from + (to - from) * i as f32 / n as f32;
                self.p((c.0 + r * t.cos(), c.1 + r * t.sin()))
            })
            .collect()
    }

    fn arrow_head(&self, tip: Pos2, dir: Vec2, size: f32, color: Color32) {
        let d = dir.normalized() * size * self.unit;
        let n = vec2(-d.y, d.x) * 0.8;
        self.painter.add(Shape::convex_polygon(vec![tip + d * 0.55, tip - d * 0.55 + n, tip - d * 0.55 - n], self.c(color), Stroke::NONE));
    }

    fn text(&self, c: (f32, f32), s: &str, font: FontId, color: Color32) {
        let font = FontId::new(font.size * self.unit, font.family);
        let galley = self.painter.layout_no_wrap(s.to_owned(), font, self.c(color));
        let pos = self.p(c) - galley.size() * 0.5;
        self.painter.galley(pos, galley, self.c(color));
    }

    /// A rod from `a` to `b` built from segments: (start, end, half width, fill).
    /// A negative half width tapers from a point to that width.
    fn rod(&self, a: (f32, f32), b: (f32, f32), parts: &[(f32, f32, f32, Color32)]) {
        let (dx, dy) = (b.0 - a.0, b.1 - a.1);
        let len = (dx * dx + dy * dy).sqrt();
        let n = (-dy / len, dx / len);
        let at = |s: f32, off: f32| (a.0 + dx * s + n.0 * off, a.1 + dy * s + n.1 * off);
        for &(s0, s1, half, fill) in parts {
            let (h0, h1) = if half < 0.0 { (0.0, -half) } else { (half, half) };
            let pts = [at(s0, h0), at(s1, h1), at(s1, -h1), at(s0, -h0)];
            self.poly(&pts, fill, Some(INK));
        }
    }

    /// The red diagonal that marks "none" (no fill, no outline).
    fn no_entry_slash(&self, (x0, y0): (f32, f32), (x1, y1): (f32, f32)) {
        self.line(&[(x0 + 0.8, y1 - 0.8), (x1 - 0.8, y0 + 0.8)], RED, 1.6);
    }

    fn page(&self, (x0, y0): (f32, f32), (x1, y1): (f32, f32)) {
        let fold = 3.5;
        self.crisp(&[(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)], PAPER, Some(INK));
        self.crisp(&[(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)], hex(0xDDE3EA), Some(INK));
    }

    fn floppy(&self) {
        let body = [(2.5, 2.5), (17.5, 2.5), (17.5, 17.5), (2.5, 17.5)];
        self.crisp(&body, BLUE, None);
        // The label stops short of the bottom edge, with blue below it like a real disk.
        self.rect((5.0, 10.0), (15.0, 15.5), PAPER, None);
        self.rule((6.5, 13.5), 12.0, PAPER_LINE);
        self.rule((6.5, 13.5), 13.9, PAPER_LINE);
        self.rect((6.0, 2.5), (14.0, 7.0), SILVER, None);
        self.rect((11.0, 3.3), (12.8, 6.2), DARK_BLUE, None);
        // Outline last, so nothing covers it and the disk reads as closed on any background.
        self.crisp(&body, Color32::TRANSPARENT, Some(DARK_BLUE));
    }

    fn magnifier(&self) {
        // Handle first, starting under the rim: a metal collar, then the wooden grip.
        // The lens and rim go on top, so nothing pokes into the glass.
        let (c, r) = ((8.5, 8.5), 5.6);
        let along = |d: f32| (c.0 + d * std::f32::consts::FRAC_1_SQRT_2, c.1 + d * std::f32::consts::FRAC_1_SQRT_2);
        let (start, collar, end) = (r + 0.4, r + 2.0, r + 7.0);
        let split = (collar - start) / (end - start);
        self.rod(along(start), along(end), &[(0.0, split, 1.15, SILVER), (split, 1.0, 1.6, BROWN)]);
        self.disc(c, r, hex(0xD9ECFB));
        self.path(&self.arc(c, r - 1.6, PI * 1.1, PI * 1.45), PAPER, 1.0);
        self.ring(c, r, INK, 1.4);
    }

    fn arrow_pointer(&self, (x, y): (f32, f32)) {
        let pts = [(x, y), (x, y + 9.0), (x + 2.2, y + 6.9), (x + 3.8, y + 10.2), (x + 5.2, y + 9.5), (x + 3.6, y + 6.2), (x + 6.6, y + 6.0)];
        self.poly(&[pts[0], pts[1], pts[6]], PAPER, None);
        self.poly(&[pts[2], pts[3], pts[4], pts[5]], PAPER, None);
        self.closed(&pts, INK, 1.0);
    }

    fn picture(&self, (x0, y0): (f32, f32), (x1, y1): (f32, f32)) {
        self.rect((x0, y0), (x1, y1), LIGHT_BLUE, Some(INK));
        let w = x1 - x0;
        let h = y1 - y0;
        self.poly(&[(x0 + 0.5, y1 - 0.5), (x0 + w * 0.4, y0 + h * 0.45), (x0 + w * 0.7, y1 - 0.5)], GREEN, None);
        self.disc((x0 + w * 0.75, y0 + h * 0.3), h * 0.12, YELLOW);
    }
}

pub fn paint(icon: Icon, painter: &Painter, rect: Rect, enabled: bool) {
    // Start on a whole physical pixel, so edges don't straddle two pixels and blur.
    let ppp = painter.ctx().pixels_per_point();
    let rect = Rect::from_min_size(((rect.min.to_vec2() * ppp).round() / ppp).to_pos2(), rect.size());
    let pen = Pen { painter, origin: rect.min, unit: rect.width() / 20.0, disabled: !enabled, ppp };
    match icon {
        Icon::Paste => {
            pen.rect((3.5, 2.5), (16.5, 19.0), BROWN, Some(BROWN_DARK));
            pen.rect((5.5, 5.0), (14.5, 17.5), PAPER, Some(PAPER_LINE));
            for y in [8.5, 11.0, 13.5] {
                pen.rule((7.0, 13.0), y, PAPER_LINE);
            }
            pen.rect((7.0, 1.0), (13.0, 4.4), SILVER, Some(SILVER_DARK));
            pen.rect((9.0, 1.6), (11.0, 2.6), SILVER_DARK, None);
        }
        Icon::Cut => {
            pen.line(&[(5.5, 1.5), (12.0, 12.5)], SILVER_DARK, 1.7);
            pen.line(&[(14.5, 1.5), (8.0, 12.5)], SILVER_DARK, 1.7);
            pen.disc((10.0, 8.8), 1.0, INK);
            pen.ring((6.5, 15.2), 2.7, BLUE, 1.7);
            pen.ring((13.5, 15.2), 2.7, BLUE, 1.7);
        }
        Icon::Copy => {
            pen.page((2.5, 1.5), (12.5, 13.5));
            pen.page((7.5, 6.0), (17.5, 18.5));
            for y in [10.5, 13.0, 15.5] {
                pen.rule((9.5, 15.0), y, BLUE);
            }
        }
        Icon::Select => {
            pen.dashed(&[(1.5, 2.5), (18.0, 2.5), (18.0, 14.5), (1.5, 14.5)], INK);
            pen.arrow_pointer((10.5, 8.5));
        }
        Icon::FreeSelect => {
            let pts: Vec<(f32, f32)> = (0..28)
                .map(|i| {
                    let t = i as f32 / 28.0 * TAU;
                    let r = 1.0 + 0.12 * (3.0 * t + 0.6).sin();
                    (9.5 + 7.8 * r * t.cos(), 8.0 + 5.6 * r * t.sin())
                })
                .collect();
            pen.dashed(&pts, INK);
            pen.arrow_pointer((10.5, 8.5));
        }
        Icon::SelectAll => {
            pen.rect((3.0, 3.5), (17.0, 16.5), LIGHT_BLUE, None);
            pen.dashed(&[(3.0, 3.5), (17.0, 3.5), (17.0, 16.5), (3.0, 16.5)], INK);
        }
        Icon::Delete => {
            pen.line(&[(4.5, 4.5), (15.5, 15.5)], RED, 2.6);
            pen.line(&[(15.5, 4.5), (4.5, 15.5)], RED, 2.6);
        }
        Icon::Crop => {
            pen.rect((6.0, 6.0), (14.0, 14.0), LIGHT_BLUE, None);
            pen.line(&[(6.0, 2.0), (6.0, 14.0), (18.0, 14.0)], INK, 1.6);
            pen.line(&[(2.0, 6.0), (14.0, 6.0), (14.0, 18.0)], INK, 1.6);
        }
        Icon::Resize => {
            pen.dashed(&[(2.5, 2.5), (17.5, 2.5), (17.5, 17.5), (2.5, 17.5)], SILVER_DARK);
            pen.picture((2.5, 9.5), (10.5, 17.5));
            pen.line(&[(9.5, 10.5), (16.0, 4.0)], INK, 1.3);
            pen.line(&[(11.5, 4.0), (16.0, 4.0), (16.0, 8.5)], INK, 1.3);
        }
        Icon::Rotate | Icon::RotateLeft => {
            let right = icon == Icon::Rotate;
            let (pic0, pic1) = if right { ((7.5, 8.0), (17.5, 17.5)) } else { ((2.5, 8.0), (12.5, 17.5)) };
            pen.picture(pic0, pic1);
            let (from, to) = if right { (PI * 1.0, PI * 1.62) } else { (PI * 2.0, PI * 1.38) };
            let pts = pen.arc((10.0, 10.0), 7.0, from, to);
            let end = *pts.last().unwrap();
            pen.path(&pts, BLUE, 1.8);
            pen.arrow_head(end, if right { vec2(1.0, 0.25) } else { vec2(-1.0, 0.25) }, 3.4, BLUE);
        }
        Icon::Rotate180 => {
            pen.picture((5.0, 6.5), (15.0, 15.5));
            let pts = pen.arc((10.0, 11.0), 8.0, PI * 0.95, PI * 2.05);
            let end = *pts.last().unwrap();
            pen.path(&pts, BLUE, 1.6);
            pen.arrow_head(end, vec2(0.1, 1.0), 3.2, BLUE);
        }
        Icon::FlipVertical => {
            pen.poly(&[(3.0, 8.5), (17.0, 8.5), (10.0, 2.0)], LIGHT_BLUE, Some(INK));
            pen.poly(&[(3.0, 11.5), (17.0, 11.5), (10.0, 18.0)], BLUE, Some(INK));
        }
        Icon::FlipHorizontal => {
            pen.poly(&[(8.5, 3.0), (8.5, 17.0), (2.0, 10.0)], LIGHT_BLUE, Some(INK));
            pen.poly(&[(11.5, 3.0), (11.5, 17.0), (18.0, 10.0)], BLUE, Some(INK));
        }
        Icon::Pencil => pen.rod(
            (2.5, 17.5),
            (17.5, 2.5),
            &[(0.0, 0.1, -0.7, INK), (0.1, 0.28, -2.3, WOOD), (0.28, 0.76, 2.3, YELLOW), (0.76, 0.85, 2.3, SILVER), (0.85, 1.0, 2.3, PINK)],
        ),
        Icon::Fill => {
            // A bucket tipped to the right, its mouth facing down-right.
            pen.poly(&[(7.2, 1.2), (17.4, 8.6), (14.1, 12.5), (4.4, 4.4)], hex(0xE9ECEF), None);
            pen.poly(&[(4.4, 4.4), (14.1, 12.5), (10.8, 16.4), (1.6, 7.6)], hex(0xC2C8CF), None);
            pen.poly(&[(17.4, 8.6), (10.8, 16.4), (9.9, 15.3), (16.3, 7.8)], BLUE, None);
            pen.closed(&[(7.2, 1.2), (17.4, 8.6), (10.8, 16.4), (1.6, 7.6)], INK, 1.0);
            pen.path(&pen.arc((6.6, 8.6), 5.6, std::f32::consts::PI * 1.08, std::f32::consts::PI * 1.72), INK, 1.0);
            // Paint pouring from the lip.
            pen.poly(&[(16.0, 9.8), (18.3, 9.2), (18.9, 15.6), (17.1, 15.6)], BLUE, None);
            pen.disc((17.9, 17.0), 2.0, BLUE);
        }
        Icon::Text => pen.text((10.0, 10.3), "A", theme::serif_bold(25.0), INK),
        Icon::Eraser => pen.rod((3.0, 16.0), (16.5, 2.5), &[(0.0, 0.5, 3.6, PINK), (0.5, 1.0, 3.6, hex(0x6FA8DC))]),
        Icon::Picker => {
            pen.rod((2.5, 17.5), (15.5, 4.5), &[(0.0, 0.08, -0.6, INK), (0.08, 0.62, 1.4, LIGHT_BLUE), (0.62, 0.72, 2.6, SILVER)]);
            pen.line(&[(13.2, 6.8), (15.8, 4.2)], INK, 4.4);
        }
        Icon::Magnifier => pen.magnifier(),
        Icon::Brushes => {
            pen.rod((18.0, 2.0), (9.0, 11.0), &[(0.0, 0.62, 1.3, BROWN), (0.62, 0.86, 1.8, SILVER)]);
            pen.poly(&[(8.2, 9.4), (10.6, 11.8), (8.0, 15.4), (4.4, 15.6)], INK, None);
            pen.poly(&[(4.4, 15.6), (8.0, 15.4), (3.0, 18.4)], BLUE, None);
        }
        Icon::Size => {
            for (y, w) in [(3.5, 1.0), (7.5, 1.8), (12.0, 2.8), (16.8, 4.0)] {
                pen.rect((2.5, y - w / 2.0), (17.5, y + w / 2.0), INK, None);
            }
        }
        Icon::EditColors => {
            let px = (rect.width() * ppp).round().max(1.0) as u32;
            let tex = cached_texture(painter.ctx(), "color-wheel", px, render_color_wheel);
            let tint = if enabled { Color32::WHITE } else { Color32::from_white_alpha(90) };
            let r = Rect::from_min_size(rect.min, Vec2::splat(px as f32 / ppp));
            painter.image(tex.id(), r, Rect::from_min_max(Pos2::ZERO, pos2(1.0, 1.0)), tint);
        }
        Icon::Outline(color) => match color {
            Some(c) => {
                // A thin square in the outline colour; very light colours get a grey edge
                // outside it so they stay visible.
                let outer = pen.snapped_rect((2.5, 3.0), (17.5, 15.0));
                let hair = pen.hairline();
                let luma = 0.299 * c.r() as f32 + 0.587 * c.g() as f32 + 0.114 * c.b() as f32;
                let line = if luma > 200.0 {
                    painter.rect_stroke(outer, 0.0, Stroke::new(hair, pen.c(SILVER_DARK)), StrokeKind::Inside);
                    outer.shrink(hair)
                } else {
                    outer
                };
                painter.rect_stroke(line, 0.0, Stroke::new(hair, pen.c(c)), StrokeKind::Inside);
            }
            None => {
                pen.rect((2.5, 3.0), (17.5, 15.0), PAPER, Some(SILVER_DARK));
                pen.no_entry_slash((2.5, 3.0), (17.5, 15.0));
            }
        },
        Icon::FillShape(color) => match color {
            Some(c) => pen.rect((2.5, 3.0), (17.5, 15.0), c, Some(INK)),
            None => {
                pen.rect((2.5, 3.0), (17.5, 15.0), PAPER, Some(INK));
                pen.no_entry_slash((2.5, 3.0), (17.5, 15.0));
            }
        },
        Icon::ZoomIn | Icon::ZoomOut => {
            pen.magnifier();
            pen.line(&[(6.0, 8.5), (11.0, 8.5)], INK, 1.5);
            if icon == Icon::ZoomIn {
                pen.line(&[(8.5, 6.0), (8.5, 11.0)], INK, 1.5);
            }
        }
        Icon::Zoom100 => {
            pen.magnifier();
            pen.text((8.5, 8.6), "100", FontId::proportional(4.6), INK);
        }
        Icon::FullScreen => {
            pen.rect((1.5, 2.5), (18.5, 14.0), INK, None);
            pen.rect((2.8, 3.8), (17.2, 12.7), LIGHT_BLUE, None);
            pen.poly(&[(2.8, 12.7), (8.0, 7.0), (12.0, 12.7)], GREEN, None);
            pen.rect((8.5, 14.0), (11.5, 16.5), SILVER_DARK, None);
            pen.rect((5.5, 16.5), (14.5, 18.0), INK, None);
        }
        Icon::New => {
            pen.page((4.0, 1.5), (16.5, 18.5));
            pen.disc((5.0, 4.0), 2.2, YELLOW);
        }
        Icon::Open => {
            pen.poly(&[(1.5, 4.0), (7.5, 4.0), (9.0, 5.5), (18.0, 5.5), (18.0, 16.5), (1.5, 16.5)], FOLDER_BACK, Some(YELLOW_DARK));
            pen.poly(&[(1.5, 16.5), (4.0, 8.5), (19.5, 8.5), (17.0, 16.5)], FOLDER, Some(YELLOW_DARK));
        }
        Icon::Save | Icon::FileSize => pen.floppy(),
        Icon::SaveAs => {
            pen.floppy();
            pen.rod((9.5, 18.5), (19.0, 9.0), &[(0.0, 0.18, -1.8, WOOD), (0.18, 0.8, 1.8, YELLOW), (0.8, 1.0, 1.8, PINK)]);
        }
        Icon::Properties => {
            pen.page((3.0, 1.5), (15.5, 18.5));
            for y in [6.5, 9.0, 11.5] {
                pen.rule((5.5, 12.5), y, PAPER_LINE);
            }
            pen.disc((14.5, 14.5), 4.2, GREEN);
            pen.line(&[(12.6, 14.6), (14.0, 16.0), (16.6, 13.2)], PAPER, 1.3);
        }
        Icon::About | Icon::Help => {
            pen.disc((10.0, 10.0), 8.5, BLUE);
            let glyph = if icon == Icon::About { "i" } else { "?" };
            pen.text((10.0, 10.2), glyph, theme::serif_bold(13.0), PAPER);
        }
        Icon::Exit => {
            pen.rect((3.0, 2.0), (12.0, 18.0), hex(0xE8EBEF), Some(INK));
            pen.disc((10.0, 10.5), 0.9, INK);
            pen.line(&[(9.5, 10.0), (17.0, 10.0)], RED, 1.8);
            pen.arrow_head(pen.p((18.0, 10.0)), vec2(1.0, 0.0), 3.6, RED);
        }
        Icon::Undo | Icon::Redo => {
            let undo = icon == Icon::Undo;
            let flip = |q: Pos2| if undo { q } else { pos2(rect.center().x * 2.0 - q.x, q.y) };
            let mut pts = vec![flip(pen.p((15.5, 15.5)))];
            pts.extend(pen.arc((10.0, 10.0), 5.5, 0.0, -PI).into_iter().map(flip));
            let end = *pts.last().unwrap();
            pen.path(&pts, DARK_BLUE, 1.9);
            pen.arrow_head(end + vec2(0.0, 1.3 * pen.unit), vec2(0.0, 1.0), 4.0, DARK_BLUE);
        }
        Icon::Collapse => pen.line(&[(6.0, 12.0), (10.0, 8.0), (14.0, 12.0)], INK, 1.3),
        Icon::Dropdown => pen.poly(&[(6.5, 8.5), (13.5, 8.5), (10.0, 12.5)], INK, None),
        Icon::ScrollUp => pen.poly(&[(6.5, 12.0), (13.5, 12.0), (10.0, 8.0)], INK, None),
        Icon::ScrollDown => pen.poly(&[(6.5, 8.0), (13.5, 8.0), (10.0, 12.0)], INK, None),
        Icon::GalleryMore => {
            pen.line(&[(6.5, 6.5), (13.5, 6.5)], INK, 1.2);
            pen.poly(&[(6.5, 9.5), (13.5, 9.5), (10.0, 13.5)], INK, None);
        }
        Icon::Check => pen.line(&[(4.5, 10.5), (8.2, 14.0), (15.5, 6.0)], INK, 1.7),
        Icon::CursorPos => {
            pen.line(&[(10.0, 1.5), (10.0, 18.5)], INK, 1.0);
            pen.line(&[(1.5, 10.0), (18.5, 10.0)], INK, 1.0);
            pen.ring((10.0, 10.0), 4.0, INK, 1.0);
        }
        Icon::SelectionSize => pen.dashed(&[(2.5, 4.0), (17.5, 4.0), (17.5, 16.0), (2.5, 16.0)], INK),
        Icon::CanvasSize => {
            pen.rect((2.5, 3.5), (15.0, 15.0), PAPER, Some(INK));
            pen.rect((14.0, 14.0), (18.0, 18.0), PAPER, Some(INK));
        }
        Icon::Opaque | Icon::Transparent => {
            if icon == Icon::Opaque {
                pen.rect((2.0, 2.0), (18.0, 18.0), PAPER, Some(SILVER_DARK));
            } else {
                for (i, (x, y)) in [(2.0, 2.0), (10.0, 2.0), (2.0, 10.0), (10.0, 10.0)].into_iter().enumerate() {
                    let fill = if i == 0 || i == 3 { hex(0xD4D7DB) } else { PAPER };
                    pen.rect((x, y), (x + 8.0, y + 8.0), fill, None);
                }
                pen.closed(&[(2.0, 2.0), (18.0, 2.0), (18.0, 18.0), (2.0, 18.0)], SILVER_DARK, 1.0);
            }
            pen.text((10.0, 10.5), "A", theme::serif_bold(15.0), BLUE);
        }
        Icon::Bold => pen.text((10.0, 10.0), "B", theme::serif_bold(17.0), INK),
        Icon::Italic => pen.text((10.0, 10.0), "I", FontId::new(17.0, eframe::egui::FontFamily::Name("serif".into())), INK),
        Icon::Underline => {
            pen.text((10.0, 8.5), "U", theme::serif(15.0), INK);
            pen.line(&[(5.0, 17.0), (15.0, 17.0)], INK, 1.2);
        }
        Icon::Strike => {
            pen.text((10.0, 10.0), "abc", theme::serif(12.0), INK);
            pen.line(&[(2.5, 10.8), (17.5, 10.8)], RED, 1.2);
        }
        Icon::FileType(kind) => {
            pen.page((3.5, 1.5), (16.5, 18.5));
            let (band, label) = match kind {
                FileKind::Png => (BLUE, "PNG"),
                FileKind::Jpeg => (hex(0xE07B22), "JPG"),
                FileKind::Bmp => (GREEN, "BMP"),
                FileKind::Gif => (hex(0x8E44AD), "GIF"),
                FileKind::Other => (SILVER_DARK, "•••"),
            };
            pen.rect((3.5, 10.0), (16.5, 15.5), band, None);
            pen.text((10.0, 12.8), label, FontId::proportional(4.6), PAPER);
        }
        Icon::Shape(kind) => {
            let px = (rect.width() * ppp).round().max(1.0) as u32;
            let tex = shape_texture(painter.ctx(), kind, px);
            let tint = if enabled { Color32::WHITE } else { Color32::from_white_alpha(90) };
            let r = Rect::from_min_size(rect.min, Vec2::splat(px as f32 / ppp));
            painter.image(tex.id(), r, Rect::from_min_max(Pos2::ZERO, pos2(1.0, 1.0)), tint);
        }
    }
}

/// Gallery icons are rasterised once per size with the canvas's own anti-aliasing
/// (round joins, exact coverage) and drawn 1:1 on the pixel grid.
fn shape_texture(ctx: &Context, kind: ShapeKind, px: u32) -> TextureHandle {
    cached_texture(ctx, ("shape-icon", kind), px, |px| render_shape(kind, px))
}

/// A texture rendered once per (key, pixel size) and kept in egui's memory.
pub fn cached_texture(ctx: &Context, key: impl std::hash::Hash + std::fmt::Debug, px: u32, render: impl FnOnce(u32) -> ColorImage) -> TextureHandle {
    let id = Id::new((key, px));
    if let Some(tex) = ctx.data(|d| d.get_temp::<TextureHandle>(id)) {
        return tex;
    }
    let tex = ctx.load_texture(format!("icon-{id:?}"), render(px), TextureOptions::NEAREST);
    ctx.data_mut(|d| d.insert_temp(id, tex.clone()));
    tex
}

/// Hue around the circle, saturation towards the rim, with a thin grey edge.
fn render_color_wheel(px: u32) -> ColorImage {
    let s = px as f32;
    let (c, r) = (s / 2.0, s * 0.43);
    let mut img = RgbaImage::new(px, px);
    const N: usize = 4;
    for y in 0..px {
        for x in 0..px {
            let (mut acc, mut hits) = ([0f32; 3], 0);
            for sy in 0..N {
                for sx in 0..N {
                    let dx = x as f32 + (sx as f32 + 0.5) / N as f32 - c;
                    let dy = y as f32 + (sy as f32 + 0.5) / N as f32 - c;
                    let d = (dx * dx + dy * dy).sqrt();
                    if d > r {
                        continue;
                    }
                    let hue = ((dy.atan2(dx) + PI / 2.0) / TAU).rem_euclid(1.0);
                    let rgb = Color32::from(Hsva::new(hue, (d / r).powf(0.8) * 0.95, 1.0, 1.0));
                    acc[0] += rgb.r() as f32;
                    acc[1] += rgb.g() as f32;
                    acc[2] += rgb.b() as f32;
                    hits += 1;
                }
            }
            if hits > 0 {
                let n = hits as f32;
                let a = (n / (N * N) as f32 * 255.0).round() as u8;
                img.put_pixel(x, y, image::Rgba([(acc[0] / n) as u8, (acc[1] / n) as u8, (acc[2] / n) as u8, a]));
            }
        }
    }
    let ring: Vec<Pos2> = (0..96).map(|i| {
        let t = i as f32 / 96.0 * TAU;
        pos2(c + (r - 0.4) * t.cos(), c + (r - 0.4) * t.sin())
    }).collect();
    let mut mask = Mask::new(px, px);
    let area = mask.stroke_path(&ring, true, (s / 64.0).max(0.5));
    raster::blend_mask(&mut img, &mask, [0x6E, 0x75, 0x7E, 0xFF], 0.85, area);
    ColorImage::from_rgba_unmultiplied([px as usize, px as usize], img.as_raw())
}

fn render_shape(kind: ShapeKind, px: u32) -> ColorImage {
    let s = px as f32 / 16.0;
    // Straight edges sit exactly on pixel rows/columns: 1px lines at 1x, 2px at 2x.
    let (lo, hi, radius) = (1.5 * s, 14.5 * s, 0.5 * s);
    let mut mask = Mask::new(px, px);
    if kind == ShapeKind::Line {
        mask.capsule(pos2(lo, lo), pos2(hi, hi), radius, 1.0);
    } else {
        for contour in kind.contours(pos2(lo, lo), pos2(hi, hi)) {
            mask.stroke_path(&contour, true, radius);
        }
    }
    let mut img = RgbaImage::new(px, px);
    let all = IRect::of_image(&img);
    raster::blend_mask(&mut img, &mask, [0x2B, 0x2B, 0x2B, 0xFF], 1.0, all);
    ColorImage::from_rgba_unmultiplied([px as usize, px as usize], img.as_raw())
}

/// A square rect of `side` centred on `c`.
pub fn square(c: Pos2, side: f32) -> Rect {
    Rect::from_center_size(c, Vec2::splat(side))
}
