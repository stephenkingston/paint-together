//! A shape that stays editable after it is drawn, as in Paint: until it is
//! committed it can be moved, resized from its handles and restyled, and is
//! re-rasterised from scratch each time.

use eframe::egui::{Pos2, Rect, pos2, vec2};
use image::RgbaImage;

use crate::raster::{self, IRect, Mask, Rgba8};
use crate::shapes::{self, ShapeKind};

/// How the shape is drawn; taken from the ribbon, so changing it there restyles the shape.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Style {
    pub outline: bool,
    pub fill: bool,
    pub size: f32,
    pub line_color: Rgba8,
    pub fill_color: Rgba8,
}

/// Handle directions around the box, clockwise from the top-left corner.
const HANDLES: [(i8, i8); 8] = [(-1, -1), (0, -1), (1, -1), (1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0)];

pub struct LiveShape {
    pub kind: ShapeKind,
    /// Corners of the box (or a line's end points), in canvas pixels.
    pub a: Pos2,
    pub b: Pos2,
    /// Drawn with the right button: Color 1 and Color 2 swap roles.
    pub swapped: bool,
    snapshot: RgbaImage,
    fill_mask: Mask,
    line_mask: Mask,
    drawn: IRect,
    touched: IRect,
    rendered: Option<(ShapeKind, Pos2, Pos2, Style)>,
}

impl LiveShape {
    pub fn new(kind: ShapeKind, at: Pos2, swapped: bool, snapshot: RgbaImage) -> Self {
        let (w, h) = snapshot.dimensions();
        Self {
            kind,
            a: at,
            b: at,
            swapped,
            snapshot,
            fill_mask: Mask::new(w, h),
            line_mask: Mask::new(w, h),
            drawn: IRect::EMPTY,
            touched: IRect::EMPTY,
            rendered: None,
        }
    }

    pub fn is_line(&self) -> bool {
        self.kind == ShapeKind::Line
    }

    /// Nothing was dragged out (a plain click).
    pub fn is_empty(&self) -> bool {
        let d = (self.b - self.a).abs();
        d.x < 1.0 && d.y < 1.0
    }

    pub fn rect(&self) -> Rect {
        Rect::from_two_pos(self.a, self.b)
    }

    pub fn snapshot(&self) -> &RgbaImage {
        &self.snapshot
    }

    /// Every pixel the shape has ever covered.
    pub fn touched(&self) -> IRect {
        self.touched
    }

    /// Redraw onto `img` if the geometry or style changed; returns the pixels to refresh.
    pub fn render(&mut self, img: &mut RgbaImage, style: Style) -> IRect {
        let key = (self.kind, self.a, self.b, style);
        if self.rendered == Some(key) {
            return IRect::EMPTY;
        }
        let is_line = self.is_line();
        let outline = style.outline || is_line;
        let fill = style.fill && !is_line;
        let radius = style.size * 0.5;
        self.fill_mask.clear(self.drawn);
        self.line_mask.clear(self.drawn);
        let mut r = IRect::EMPTY;
        if is_line {
            r = self.line_mask.capsule(self.a, self.b, radius, 1.0);
        } else {
            for pts in self.kind.contours(self.a, self.b) {
                if fill {
                    r = r.union(self.fill_mask.fill_polygon(&pts, 1.0));
                }
                if outline {
                    r = r.union(self.line_mask.stroke_path(&pts, true, radius));
                }
            }
        }
        let region = self.drawn.union(r);
        raster::copy_rect(img, &self.snapshot, region);
        if fill {
            raster::blend_mask(img, &self.fill_mask, style.fill_color, 1.0, r);
        }
        if outline {
            raster::blend_mask(img, &self.line_mask, style.line_color, 1.0, r);
        }
        self.drawn = r;
        self.touched = self.touched.union(region);
        self.rendered = Some(key);
        region
    }

    /// The picture under the shape changed in `r` (a friend drew there): use
    /// theirs as the background and redraw on the next render.
    pub fn rebase(&mut self, base: &RgbaImage, r: IRect) {
        raster::copy_rect(&mut self.snapshot, base, r);
        self.rendered = None;
    }

    /// Put back what was under the shape; returns the pixels to refresh.
    pub fn erase(&self, img: &mut RgbaImage) -> IRect {
        raster::copy_rect(img, &self.snapshot, self.touched);
        self.touched
    }

    /// Handle positions in canvas pixels: the ends of a line, or eight around a box.
    pub fn handles(&self) -> Vec<(usize, Pos2)> {
        if self.is_line() {
            return vec![(0, self.a), (1, self.b)];
        }
        let r = self.rect();
        HANDLES
            .iter()
            .enumerate()
            .map(|(i, &(dx, dy))| (i, r.center() + vec2(dx as f32 * r.width() * 0.5, dy as f32 * r.height() * 0.5)))
            .collect()
    }

    pub fn handle_at(&self, p: Pos2, tol: f32) -> Option<usize> {
        self.handles().into_iter().find(|(_, h)| (p - *h).abs().max_elem() <= tol).map(|(i, _)| i)
    }

    /// Whether `p` grabs the shape's body (within its box, or near a line).
    pub fn contains(&self, p: Pos2, tol: f32) -> bool {
        if self.is_line() {
            let ab = self.b - self.a;
            let t = if ab.length_sq() > 0.0 { ((p - self.a).dot(ab) / ab.length_sq()).clamp(0.0, 1.0) } else { 0.0 };
            return (p - (self.a + ab * t)).length() <= tol;
        }
        self.rect().expand(tol).contains(p)
    }

    /// Drag handle `h` to `p`, starting from the geometry `(a, b)` it had when grabbed.
    /// Shift keeps lines at 45° steps and corner drags square.
    pub fn drag_handle(&mut self, h: usize, (a, b): (Pos2, Pos2), p: Pos2, shift: bool) {
        if self.is_line() {
            if h == 0 {
                self.a = shapes::constrain(ShapeKind::Line, b, p, shift);
            } else {
                self.b = shapes::constrain(ShapeKind::Line, a, p, shift);
            }
            return;
        }
        let r = Rect::from_two_pos(a, b);
        let (dx, dy) = HANDLES[h];
        let (mut x0, mut y0, mut x1, mut y1) = (r.min.x, r.min.y, r.max.x, r.max.y);
        if dx != 0 && dy != 0 && shift {
            let (ox, oy) = (if dx < 0 { x1 } else { x0 }, if dy < 0 { y1 } else { y0 });
            let corner = shapes::constrain(ShapeKind::Rectangle, pos2(ox, oy), p, true);
            (x0, y0, x1, y1) = (ox, oy, corner.x, corner.y);
        } else {
            if dx < 0 {
                x0 = p.x;
            } else if dx > 0 {
                x1 = p.x;
            }
            if dy < 0 {
                y0 = p.y;
            } else if dy > 0 {
                y1 = p.y;
            }
        }
        // Dragging past the opposite edge just turns the box inside out.
        self.a = pos2(x0, y0);
        self.b = pos2(x1, y1);
    }

    pub fn translate(&mut self, (a, b): (Pos2, Pos2), d: eframe::egui::Vec2) {
        self.a = a + d;
        self.b = b + d;
    }
}

/// Mouse cursor for a handle of a box (corners diagonal, edges straight).
pub fn handle_cursor(is_line: bool, h: usize) -> eframe::egui::CursorIcon {
    use eframe::egui::CursorIcon;
    if is_line {
        return CursorIcon::Crosshair;
    }
    match h {
        0 | 4 => CursorIcon::ResizeNwSe,
        2 | 6 => CursorIcon::ResizeNeSw,
        1 | 5 => CursorIcon::ResizeVertical,
        _ => CursorIcon::ResizeHorizontal,
    }
}
