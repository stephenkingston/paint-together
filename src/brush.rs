//! Paint's brush types, each a way of laying coverage along a stroke segment.

use std::f32::consts::FRAC_1_SQRT_2;

use eframe::egui::{Pos2, vec2};

use crate::raster::{IRect, Mask};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BrushKind {
    Brush,
    Calligraphy1,
    Calligraphy2,
    Airbrush,
    OilBrush,
    Crayon,
    Marker,
    NaturalPencil,
    Watercolor,
}

use BrushKind::*;

impl BrushKind {
    pub const ALL: [BrushKind; 9] =
        [Brush, Calligraphy1, Calligraphy2, Airbrush, OilBrush, Crayon, Marker, NaturalPencil, Watercolor];

    pub fn name(self) -> &'static str {
        match self {
            Brush => "Brush",
            Calligraphy1 => "Calligraphy brush 1",
            Calligraphy2 => "Calligraphy brush 2",
            Airbrush => "Airbrush",
            OilBrush => "Oil brush",
            Crayon => "Crayon",
            Marker => "Marker",
            NaturalPencil => "Natural pencil",
            Watercolor => "Watercolor brush",
        }
    }

    /// Alpha applied to the whole stroke.
    pub fn opacity(self) -> f32 {
        match self {
            Marker => 0.5,
            Watercolor => 0.45,
            NaturalPencil => 0.9,
            _ => 1.0,
        }
    }

    /// Lay coverage for the stroke segment `a`–`b` into `mask`.
    pub fn segment(self, mask: &mut Mask, a: Pos2, b: Pos2, size: f32, rng: &mut u64) -> IRect {
        let r = (size * 0.5).max(0.5);
        match self {
            Brush | Marker => mask.capsule(a, b, r, 1.0),
            Calligraphy1 | Calligraphy2 => {
                let s = if self == Calligraphy1 { -1.0 } else { 1.0 };
                let nib = vec2(FRAC_1_SQRT_2, s * FRAC_1_SQRT_2) * r;
                let quad = mask.fill_polygon(&[a - nib, a + nib, b + nib, b - nib], 1.0);
                quad.union(mask.capsule(a - nib, a + nib, 0.7, 1.0)).union(mask.capsule(b - nib, b + nib, 0.7, 1.0))
            }
            Airbrush => {
                let n = (size * 1.5).clamp(4.0, 300.0) as usize;
                spray(mask, b, size, rng, n)
            }
            OilBrush => mask.sweep(a, b, r, |_, _, d, perp| {
                let edge = (r + 0.5 - d).clamp(0.0, 1.0);
                let bristle = hash1(((perp + r) * 0.8).floor() as i32);
                edge * (0.55 + 0.45 * bristle)
            }),
            Crayon => mask.sweep(a, b, r, |x, y, d, _| {
                let edge = (r + 0.5 - d).clamp(0.0, 1.0);
                edge * if hash2(x, y) > 0.38 { 0.95 } else { 0.2 }
            }),
            NaturalPencil => {
                let r = (size * 0.3).max(0.6);
                mask.sweep(a, b, r, |x, y, d, _| (r + 0.5 - d).clamp(0.0, 1.0) * (0.45 + 0.55 * hash2(x, y)))
            }
            Watercolor => mask.sweep(a, b, r, |x, y, d, _| {
                let soft = (1.0 - d / r).clamp(0.0, 1.0).powf(0.6);
                soft * (0.75 + 0.25 * hash2(x / 3, y / 3))
            }),
        }
    }
}

/// Scatter `n` single-pixel dots in a disc of diameter `size` around `c`.
pub fn spray(mask: &mut Mask, c: Pos2, size: f32, rng: &mut u64, n: usize) -> IRect {
    let radius = (size * 0.5).max(2.0);
    for _ in 0..n {
        let t = next(rng) * std::f32::consts::TAU;
        let d = next(rng).sqrt() * radius;
        mask.max_at((c.x + t.cos() * d).floor() as i32, (c.y + t.sin() * d).floor() as i32, 1.0);
    }
    IRect::around(c, c, radius + 1.0).intersect(mask.bounds())
}

/// xorshift64*, mapped to [0, 1).
fn next(state: &mut u64) -> f32 {
    *state ^= *state >> 12;
    *state ^= *state << 25;
    *state ^= *state >> 27;
    (state.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 40) as f32 / (1u64 << 24) as f32
}

fn hash1(i: i32) -> f32 {
    hash2(i, 0x5bd1)
}

/// Stable per-pixel noise in [0, 1].
fn hash2(x: i32, y: i32) -> f32 {
    let mut h = (x as u32).wrapping_mul(374_761_393) ^ (y as u32).wrapping_mul(668_265_263);
    h = (h ^ (h >> 13)).wrapping_mul(1_274_126_177);
    (h ^ (h >> 16)) as f32 / u32::MAX as f32
}
