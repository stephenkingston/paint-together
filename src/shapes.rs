//! The shape gallery. Every shape is a polygon fitted to the dragged box, so the
//! same outline drives rasterization and the toolbar icons.

use std::f32::consts::{FRAC_PI_2, FRAC_PI_4, TAU};

use eframe::egui::{Pos2, Rect, pos2, vec2};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ShapeKind {
    Line,
    Oval,
    Rectangle,
    RoundedRectangle,
    Triangle,
    RightTriangle,
    Diamond,
    Pentagon,
    Hexagon,
    RightArrow,
    LeftArrow,
    UpArrow,
    DownArrow,
    Star4,
    Star5,
    Star6,
    RoundedCallout,
    OvalCallout,
    CloudCallout,
    Heart,
    Lightning,
}

use ShapeKind::*;

const ARROW: [(f32, f32); 7] = [(0.0, 0.25), (0.6, 0.25), (0.6, 0.0), (1.0, 0.5), (0.6, 1.0), (0.6, 0.75), (0.0, 0.75)];
const LIGHTNING: [(f32, f32); 7] = [(0.35, 0.0), (0.78, 0.0), (0.56, 0.36), (0.86, 0.36), (0.22, 1.0), (0.4, 0.55), (0.12, 0.55)];

impl ShapeKind {
    /// In the order of Paint's shape gallery.
    pub const ALL: [ShapeKind; 21] = [
        Line, Oval, Rectangle, RoundedRectangle, Triangle, RightTriangle, Diamond, Pentagon, Hexagon, RightArrow,
        LeftArrow, UpArrow, DownArrow, Star4, Star5, Star6, RoundedCallout, OvalCallout, CloudCallout, Heart, Lightning,
    ];

    pub fn name(self) -> &'static str {
        match self {
            Line => "Line",
            Oval => "Oval",
            Rectangle => "Rectangle",
            RoundedRectangle => "Rounded rectangle",
            Triangle => "Triangle",
            RightTriangle => "Right triangle",
            Diamond => "Diamond",
            Pentagon => "Pentagon",
            Hexagon => "Hexagon",
            RightArrow => "Right arrow",
            LeftArrow => "Left arrow",
            UpArrow => "Up arrow",
            DownArrow => "Down arrow",
            Star4 => "Four-point star",
            Star5 => "Five-point star",
            Star6 => "Six-point star",
            RoundedCallout => "Rounded rectangular callout",
            OvalCallout => "Oval callout",
            CloudCallout => "Cloud callout",
            Heart => "Heart",
            Lightning => "Lightning",
        }
    }

    /// Closed outline fitted to the box spanned by `a` and `b`. For [`Line`] it is
    /// just the two end points.
    pub fn outline(self, a: Pos2, b: Pos2) -> Vec<Pos2> {
        let r = Rect::from_two_pos(a, b);
        let unit: Vec<(f32, f32)> = match self {
            Line => return vec![a, b],
            RoundedRectangle => return rounded_rect(r, None),
            RoundedCallout => {
                let body = Rect::from_min_max(r.min, pos2(r.max.x, r.min.y + r.height() * 0.78));
                let (x, w) = (r.min.x, r.width());
                let tail = [pos2(x + 0.40 * w, body.max.y), pos2(x + 0.15 * w, r.max.y), pos2(x + 0.26 * w, body.max.y)];
                return rounded_rect(body, Some(tail));
            }
            OvalCallout => oval_callout(),
            CloudCallout => (0..180)
                .map(|i| {
                    let t = i as f32 / 180.0 * TAU;
                    let m = 0.86 + 0.14 * (4.5 * t).sin().abs();
                    (0.5 + 0.5 * m * t.cos(), 0.36 + 0.36 * m * t.sin())
                })
                .collect(),
            Oval => {
                let n = ((r.width() + r.height()) * 0.5).clamp(32.0, 360.0) as usize;
                (0..n)
                    .map(|i| {
                        let t = i as f32 / n as f32 * TAU;
                        (0.5 + 0.5 * t.cos(), 0.5 + 0.5 * t.sin())
                    })
                    .collect()
            }
            Rectangle => vec![(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)],
            Triangle => vec![(0.5, 0.0), (1.0, 1.0), (0.0, 1.0)],
            RightTriangle => vec![(0.0, 0.0), (1.0, 1.0), (0.0, 1.0)],
            Diamond => vec![(0.5, 0.0), (1.0, 0.5), (0.5, 1.0), (0.0, 0.5)],
            Pentagon => normalize(star(5, 1.0)),
            Hexagon => vec![(0.25, 0.0), (0.75, 0.0), (1.0, 0.5), (0.75, 1.0), (0.25, 1.0), (0.0, 0.5)],
            RightArrow => ARROW.to_vec(),
            LeftArrow => ARROW.iter().map(|&(x, y)| (1.0 - x, y)).collect(),
            UpArrow => ARROW.iter().map(|&(x, y)| (y, 1.0 - x)).collect(),
            DownArrow => ARROW.iter().map(|&(x, y)| (y, x)).collect(),
            Star4 => normalize(star(4, 0.38)),
            Star5 => normalize(star(5, 0.4)),
            Star6 => normalize(star(6, 0.55)),
            Heart => normalize(heart()),
            Lightning => LIGHTNING.to_vec(),
        };
        unit.into_iter().map(|(u, v)| pos2(r.min.x + u * r.width(), r.min.y + v * r.height())).collect()
    }
}

impl ShapeKind {
    /// Every closed outline of the shape: the main one plus any detached parts.
    pub fn contours(self, a: Pos2, b: Pos2) -> Vec<Vec<Pos2>> {
        let mut out = vec![self.outline(a, b)];
        if self == CloudCallout {
            let r = Rect::from_two_pos(a, b);
            for (cx, cy, rx, ry) in [(0.22, 0.83, 0.07, 0.06), (0.1, 0.95, 0.045, 0.04)] {
                out.push(
                    (0..24)
                        .map(|i| {
                            let t = i as f32 / 24.0 * TAU;
                            pos2(r.min.x + (cx + rx * t.cos()) * r.width(), r.min.y + (cy + ry * t.sin()) * r.height())
                        })
                        .collect(),
                );
            }
        }
        out
    }
}

/// With Shift held: lines snap to 45° steps, everything else becomes square.
pub fn constrain(kind: ShapeKind, a: Pos2, b: Pos2, shift: bool) -> Pos2 {
    if !shift {
        return b;
    }
    let d = b - a;
    if kind == Line {
        let angle = (d.y.atan2(d.x) / FRAC_PI_4).round() * FRAC_PI_4;
        let dir = vec2(angle.cos(), angle.sin());
        return a + dir * d.dot(dir);
    }
    let side = d.x.abs().max(d.y.abs());
    a + vec2(side.copysign(d.x), side.copysign(d.y))
}

/// Regular star with `n` points (inner radius ratio `inner`); `inner == 1` gives
/// a regular polygon with `n` corners.
fn star(n: usize, inner: f32) -> Vec<(f32, f32)> {
    if inner >= 1.0 {
        return (0..n)
            .map(|k| {
                let t = -FRAC_PI_2 + k as f32 * TAU / n as f32;
                (t.cos(), t.sin())
            })
            .collect();
    }
    (0..2 * n)
        .map(|k| {
            let t = -FRAC_PI_2 + k as f32 * TAU / (2 * n) as f32;
            let r = if k % 2 == 0 { 1.0 } else { inner };
            (r * t.cos(), r * t.sin())
        })
        .collect()
}

fn heart() -> Vec<(f32, f32)> {
    (0..120)
        .map(|i| {
            let t = i as f32 / 120.0 * TAU;
            let x = 16.0 * t.sin().powi(3);
            let y = -(13.0 * t.cos() - 5.0 * (2.0 * t).cos() - 2.0 * (3.0 * t).cos() - (4.0 * t).cos());
            (x, y)
        })
        .collect()
}

/// Rescale points so their bounding box is the unit square.
fn normalize(pts: Vec<(f32, f32)>) -> Vec<(f32, f32)> {
    let (mut x0, mut y0, mut x1, mut y1) = (f32::MAX, f32::MAX, f32::MIN, f32::MIN);
    for &(x, y) in &pts {
        x0 = x0.min(x);
        y0 = y0.min(y);
        x1 = x1.max(x);
        y1 = y1.max(y);
    }
    let (w, h) = ((x1 - x0).max(1e-6), (y1 - y0).max(1e-6));
    pts.into_iter().map(|(x, y)| ((x - x0) / w, (y - y0) / h)).collect()
}

fn rounded_rect(r: Rect, tail: Option<[Pos2; 3]>) -> Vec<Pos2> {
    let rad = r.width().min(r.height()) * 0.18;
    let corners = [
        (pos2(r.max.x - rad, r.min.y + rad), -FRAC_PI_2),
        (pos2(r.max.x - rad, r.max.y - rad), 0.0),
        (pos2(r.min.x + rad, r.max.y - rad), FRAC_PI_2),
        (pos2(r.min.x + rad, r.min.y + rad), 2.0 * FRAC_PI_2),
    ];
    let mut out = Vec::with_capacity(4 * 11 + 3);
    for (i, (c, start)) in corners.into_iter().enumerate() {
        if i == 2 {
            // The bottom edge runs right to left here; a callout's tail joins it.
            out.extend(tail.into_iter().flatten());
        }
        for k in 0..=10 {
            let t = start + k as f32 / 10.0 * FRAC_PI_2;
            out.push(c + vec2(t.cos(), t.sin()) * rad);
        }
    }
    out
}

/// An ellipse in the top of the box with a tail to the bottom-left corner.
fn oval_callout() -> Vec<(f32, f32)> {
    let (t0, t1) = (0.58 * std::f32::consts::PI, 0.72 * std::f32::consts::PI);
    let at = |t: f32| (0.5 + 0.5 * t.cos(), 0.39 + 0.39 * t.sin());
    let mut out = Vec::new();
    let mut tail_added = false;
    for i in 0..120 {
        let t = i as f32 / 120.0 * TAU;
        if t > t0 && t < t1 {
            continue;
        }
        if t >= t1 && !tail_added {
            out.extend([at(t0), (0.12, 1.0), at(t1)]);
            tail_added = true;
        }
        out.push(at(t));
    }
    out
}
