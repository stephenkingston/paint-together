//! The app icon: a macOS-style rounded tile holding an artist's palette and a brush.
//! Drawn with the canvas rasteriser in 512-unit coordinates; small sizes are
//! rendered at 512 px and downsampled.

use std::f32::consts::TAU;

use eframe::egui::{Pos2, Vec2, pos2, vec2};
use image::{RgbaImage, imageops};

use crate::raster::{self, IRect, Mask, Rgba8};

const BASE: u32 = 512;

const RED: Rgba8 = hex(0xE5483D);
const YELLOW: Rgba8 = hex(0xF6C232);
const GREEN: Rgba8 = hex(0x2FB37A);
const BLUE: Rgba8 = hex(0x2F80ED);
const PURPLE: Rgba8 = hex(0x9B51E0);

const fn hex(rgb: u32) -> Rgba8 {
    [(rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8, 255]
}

const fn hexa(rgb: u32, a: u8) -> Rgba8 {
    [(rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8, a]
}

pub fn render(size: u32) -> RgbaImage {
    if size >= BASE {
        draw(size)
    } else {
        imageops::resize(&draw(BASE), size, size, imageops::FilterType::Lanczos3)
    }
}

fn mix(a: Rgba8, b: Rgba8, t: f32) -> Rgba8 {
    let t = t.clamp(0.0, 1.0);
    let mut out = [0u8; 4];
    for i in 0..4 {
        out[i] = (a[i] as f32 + (b[i] as f32 - a[i] as f32) * t).round() as u8;
    }
    out
}

/// Drawing surface addressed in 512-unit coordinates whatever its pixel size.
struct Canvas {
    img: RgbaImage,
    k: f32,
}

impl Canvas {
    fn composite(&mut self, mask: &Mask, r: IRect, color: &dyn Fn(f32, f32) -> Rgba8) {
        for y in r.y0..r.y1 {
            for x in r.x0..r.x1 {
                let cov = mask.get(x, y);
                if cov <= 0.0 {
                    continue;
                }
                let c = color((x as f32 + 0.5) / self.k, (y as f32 + 0.5) / self.k);
                let px = self.img.get_pixel_mut(x as u32, y as u32);
                px.0 = raster::over(px.0, c, cov * c[3] as f32 / 255.0);
            }
        }
    }

    fn scaled(&self, pts: &[Pos2]) -> Vec<Pos2> {
        pts.iter().map(|p| pos2(p.x * self.k, p.y * self.k)).collect()
    }

    fn fill(&mut self, pts: &[Pos2], color: impl Fn(f32, f32) -> Rgba8) {
        let mut m = Mask::new(self.img.width(), self.img.height());
        let r = m.fill_polygon(&self.scaled(pts), 1.0);
        self.composite(&m, r, &color);
    }

    fn stroke(&mut self, pts: &[Pos2], width: f32, color: Rgba8) {
        let mut m = Mask::new(self.img.width(), self.img.height());
        let r = m.stroke_path(&self.scaled(pts), true, width * self.k * 0.5);
        self.composite(&m, r, &|_, _| color);
    }
}

fn superellipse(c: Pos2, a: f32, n: f32) -> Vec<Pos2> {
    (0..720)
        .map(|i| {
            let (s, co) = (i as f32 / 720.0 * TAU).sin_cos();
            c + vec2(a * co.signum() * co.abs().powf(2.0 / n), a * s.signum() * s.abs().powf(2.0 / n))
        })
        .collect()
}

fn ellipse(c: Pos2, rx: f32, ry: f32, rot: f32) -> Vec<Pos2> {
    (0..180)
        .map(|i| {
            let t = i as f32 / 180.0 * TAU;
            c + rotate(vec2(rx * t.cos(), ry * t.sin()), rot)
        })
        .collect()
}

fn rotate(v: Vec2, a: f32) -> Vec2 {
    let (s, c) = a.sin_cos();
    vec2(v.x * c - v.y * s, v.x * s + v.y * c)
}

fn shifted(pts: &[Pos2], d: Vec2) -> Vec<Pos2> {
    pts.iter().map(|p| *p + d).collect()
}

fn draw(size: u32) -> RgbaImage {
    let mut cv = Canvas { img: RgbaImage::new(size, size), k: size as f32 / BASE as f32 };

    // Tile with a soft drop shadow.
    let tile_c = pos2(256.0, 248.0);
    let a = 206.0;
    for i in 0..14 {
        let k = i as f32;
        cv.fill(&superellipse(tile_c + vec2(0.0, 7.0 + k * 0.6), a + k * 1.4, 5.0), |_, _| [0, 0, 0, 5]);
    }
    let top = tile_c.y - a;
    let background = move |_x: f32, y: f32| mix(hex(0xFFFFFF), hex(0xE2E8F2), ((y - top) / (2.0 * a)).powf(1.3));
    cv.fill(&superellipse(tile_c, a, 5.0), background);
    cv.stroke(&superellipse(tile_c, a - 0.75, 5.0), 1.5, [0, 0, 0, 24]);

    // Palette: an oval with a notch beside the thumb hole.
    let pc = pos2(236.0, 276.0);
    let rot = -14f32.to_radians();
    let to = move |u: f32, v: f32| pc + rotate(vec2(u * 150.0, v * 116.0), rot);
    let notch = 200f32.to_radians();
    let outline: Vec<Pos2> = (0..360)
        .map(|i| {
            let t = i as f32 / 360.0 * TAU;
            let dt = (t - notch + TAU / 2.0).rem_euclid(TAU) - TAU / 2.0;
            let k = 1.0 - 0.27 * (-(dt * dt) / (2.0 * 0.19 * 0.19)).exp();
            to(t.cos() * k, t.sin() * k)
        })
        .collect();
    for (d, alpha) in [(vec2(6.0, 18.0), 14), (vec2(4.0, 12.0), 18), (vec2(2.0, 6.0), 24)] {
        cv.fill(&shifted(&outline, d), move |_, _| [60, 40, 20, alpha]);
    }
    cv.fill(&outline, |x, y| mix(hex(0xFCE9C6), hex(0xD99E52), ((x - 90.0) + (y - 160.0)) / 500.0));
    cv.stroke(&outline, 3.0, hexa(0xA86F33, 150));
    let hole_c = to(-0.42, 0.3);
    cv.fill(&ellipse(hole_c, 25.0, 25.0, 0.0), background);
    cv.fill(&ellipse(hole_c + vec2(0.0, -3.0), 25.0, 22.0, 0.0), |_, _| [90, 60, 30, 26]);
    cv.stroke(&ellipse(hole_c, 25.0, 25.0, 0.0), 3.0, hexa(0xA86F33, 170));

    // Paint dabs with a shadow, a shaded body and a glint.
    for (u, v, color) in [(-0.04, -0.56, RED), (0.38, -0.5, YELLOW), (0.66, -0.06, GREEN), (0.5, 0.43, BLUE), (0.08, 0.56, PURPLE)] {
        let c = to(u, v);
        cv.fill(&ellipse(c + vec2(2.0, 6.0), 31.0, 29.0, 0.0), |_, _| [70, 45, 20, 50]);
        let light = mix(color, hex(0xFFFFFF), 0.28);
        let dark = mix(color, hex(0x000000), 0.22);
        cv.fill(&ellipse(c, 31.0, 29.0, 0.0), move |x, y| {
            let d = ((x - (c.x - 11.0)).powi(2) + (y - (c.y - 12.0)).powi(2)).sqrt() / 55.0;
            mix(light, dark, d)
        });
        cv.fill(&ellipse(c + vec2(-11.0, -12.0), 10.0, 6.5, -0.6), |_, _| [255, 255, 255, 170]);
    }

    draw_brush(&mut cv);
    cv.img
}

fn draw_brush(cv: &mut Canvas) {
    let end = pos2(436.0, 70.0);
    let tip = pos2(250.0, 266.0);
    let len = (tip - end).length();
    let d = (tip - end) / len;
    let n = vec2(-d.y, d.x);
    let at = move |s: f32, w: f32| end + d * (len * s) + n * w;
    // (position along the brush, width)
    let profile = [
        (0.0, 12.0),
        (0.08, 14.0),
        (0.55, 21.0),
        (0.555, 23.0),
        (0.70, 23.0),
        (0.705, 20.0),
        (0.80, 26.0),
        (0.92, 16.0),
        (1.0, 0.0),
    ];
    let outline = |from: f32, to_: f32| -> Vec<Pos2> {
        let part: Vec<(f32, f32)> = profile.iter().copied().filter(|&(s, _)| s >= from - 1e-4 && s <= to_ + 1e-4).collect();
        let mut pts: Vec<Pos2> = part.iter().map(|&(s, w)| at(s, -w / 2.0)).collect();
        pts.extend(part.iter().rev().map(|&(s, w)| at(s, w / 2.0)));
        pts
    };
    let across = move |x: f32, y: f32| (pos2(x, y) - end).dot(n);
    let along = move |x: f32, y: f32| (pos2(x, y) - end).dot(d) / len;

    // Shadow cast on the palette and tile.
    cv.fill(&shifted(&outline(0.0, 1.0), vec2(9.0, 13.0)), |_, _| [40, 30, 20, 55]);
    // Lacquered handle with a rounded end.
    let handle = move |x: f32, y: f32| mix(hex(0x4A68B0), hex(0x14203D), (across(x, y) / 11.0 + 1.0) / 2.0);
    cv.fill(&ellipse(at(0.0, 0.0), 6.0, 6.0, 0.0), handle);
    cv.fill(&outline(0.0, 0.555), handle);
    cv.stroke(&[at(0.06, -3.0), at(0.5, -6.0)], 2.0, [255, 255, 255, 60]);
    // Metal ferrule with crimp lines.
    cv.fill(&outline(0.555, 0.70), move |x, y| mix(hex(0xF4F6F9), hex(0x8691A0), (across(x, y) / 11.5 + 1.0) / 2.0));
    for s in [0.6, 0.645] {
        cv.stroke(&[at(s, -11.0), at(s, 11.0)], 1.6, hexa(0x5A6472, 150));
    }
    // Bristles, dipped in red paint.
    cv.fill(&outline(0.705, 1.0), move |x, y| {
        let paint = ((along(x, y) - 0.76) / 0.08).clamp(0.0, 1.0);
        let base = mix(hex(0x5A3E2B), RED, paint);
        mix(mix(base, hex(0xFFFFFF), 0.18), mix(base, hex(0x000000), 0.25), (across(x, y) / 12.0 + 1.0) / 2.0)
    });
    cv.fill(&ellipse(at(0.86, -4.0), 7.0, 3.0, d.y.atan2(d.x)), |_, _| [255, 255, 255, 110]);
}
