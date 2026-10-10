//! Pixel operations on the canvas: coverage masks, compositing, fills and transforms.
//!
//! Drawing tools never paint straight onto the canvas. They rasterize coverage
//! (0..=1 per pixel) into a [`Mask`], keeping the maximum per pixel, and the mask
//! is then composited over a snapshot taken when the operation started. That keeps
//! anti-aliased strokes free of the dark "beads" you get from re-blending
//! overlapping dabs, and lets shapes be re-rasterized on every mouse move.

use eframe::egui::{Pos2, Vec2, pos2, vec2};
use image::{Rgba, RgbaImage, imageops};

pub type Rgba8 = [u8; 4];

/// Integer pixel rectangle, half-open: `[x0, x1) × [y0, y1)`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IRect {
    pub x0: i32,
    pub y0: i32,
    pub x1: i32,
    pub y1: i32,
}

impl IRect {
    pub const EMPTY: IRect = IRect { x0: i32::MAX, y0: i32::MAX, x1: i32::MIN, y1: i32::MIN };

    pub const fn new(x0: i32, y0: i32, x1: i32, y1: i32) -> Self {
        Self { x0, y0, x1, y1 }
    }

    pub fn of_image(img: &RgbaImage) -> Self {
        Self::new(0, 0, img.width() as i32, img.height() as i32)
    }

    /// Pixels covering the box spanned by two points, grown by `pad`.
    pub fn around(a: Pos2, b: Pos2, pad: f32) -> Self {
        Self::new(
            (a.x.min(b.x) - pad).floor() as i32,
            (a.y.min(b.y) - pad).floor() as i32,
            (a.x.max(b.x) + pad).ceil() as i32 + 1,
            (a.y.max(b.y) + pad).ceil() as i32 + 1,
        )
    }

    pub fn is_empty(&self) -> bool {
        self.x0 >= self.x1 || self.y0 >= self.y1
    }

    pub fn width(&self) -> i32 {
        (self.x1 - self.x0).max(0)
    }

    pub fn height(&self) -> i32 {
        (self.y1 - self.y0).max(0)
    }

    pub fn union(self, o: Self) -> Self {
        if self.is_empty() {
            return o;
        }
        if o.is_empty() {
            return self;
        }
        Self::new(self.x0.min(o.x0), self.y0.min(o.y0), self.x1.max(o.x1), self.y1.max(o.y1))
    }

    pub fn intersect(self, o: Self) -> Self {
        Self::new(self.x0.max(o.x0), self.y0.max(o.y0), self.x1.min(o.x1), self.y1.min(o.y1))
    }

    pub fn contains_rect(&self, o: IRect) -> bool {
        o.x0 >= self.x0 && o.y0 >= self.y0 && o.x1 <= self.x1 && o.y1 <= self.y1
    }

    pub fn contains(&self, p: Pos2) -> bool {
        p.x >= self.x0 as f32 && p.x < self.x1 as f32 && p.y >= self.y0 as f32 && p.y < self.y1 as f32
    }
}

/// Per-pixel coverage for one drawing operation.
pub struct Mask {
    w: i32,
    h: i32,
    data: Vec<f32>,
}

impl Mask {
    pub fn new(w: u32, h: u32) -> Self {
        Self { w: w as i32, h: h as i32, data: vec![0.0; w as usize * h as usize] }
    }

    pub fn bounds(&self) -> IRect {
        IRect::new(0, 0, self.w, self.h)
    }

    #[inline]
    pub fn get(&self, x: i32, y: i32) -> f32 {
        if x < 0 || y < 0 || x >= self.w || y >= self.h {
            return 0.0;
        }
        self.data[(y * self.w + x) as usize]
    }

    #[inline]
    pub fn max_at(&mut self, x: i32, y: i32, v: f32) {
        if x >= 0 && y >= 0 && x < self.w && y < self.h {
            let p = &mut self.data[(y * self.w + x) as usize];
            if v > *p {
                *p = v.min(1.0);
            }
        }
    }

    pub fn clear(&mut self, r: IRect) {
        let r = r.intersect(self.bounds());
        if r.is_empty() {
            return;
        }
        for y in r.y0..r.y1 {
            let row = (y * self.w) as usize;
            self.data[row + r.x0 as usize..row + r.x1 as usize].fill(0.0);
        }
    }

    /// Visit every pixel within `reach` of the segment `a`–`b`. The callback gets
    /// the pixel, its distance to the segment and its signed distance across the
    /// segment's direction, and returns the coverage to write.
    pub fn sweep(&mut self, a: Pos2, b: Pos2, reach: f32, mut f: impl FnMut(i32, i32, f32, f32) -> f32) -> IRect {
        let r = IRect::around(a, b, reach + 1.0).intersect(self.bounds());
        if r.is_empty() {
            return r;
        }
        let ab = b - a;
        let len2 = ab.length_sq();
        let normal = if len2 > 0.0 { vec2(-ab.y, ab.x) / len2.sqrt() } else { Vec2::ZERO };
        for y in r.y0..r.y1 {
            for x in r.x0..r.x1 {
                let ap = pos2(x as f32 + 0.5, y as f32 + 0.5) - a;
                let t = if len2 > 0.0 { (ap.dot(ab) / len2).clamp(0.0, 1.0) } else { 0.0 };
                let d = (ap - ab * t).length();
                if d > reach + 1.0 {
                    continue;
                }
                let v = f(x, y, d, ap.dot(normal));
                if v > 0.0 {
                    self.max_at(x, y, v);
                }
            }
        }
        r
    }

    /// Anti-aliased segment with round caps.
    pub fn capsule(&mut self, a: Pos2, b: Pos2, radius: f32, weight: f32) -> IRect {
        self.sweep(a, b, radius, |_, _, d, _| (radius + 0.5 - d).clamp(0.0, 1.0) * weight)
    }

    /// Aliased line drawn with a `size`×`size` square pen (pencil, eraser).
    pub fn square_line(&mut self, a: Pos2, b: Pos2, size: i32) -> IRect {
        let size = size.max(1);
        let steps = (b - a).abs().max_elem().ceil().max(1.0) as i32;
        let mut out = IRect::EMPTY;
        let half = size as f32 * 0.5;
        for i in 0..=steps {
            let p = a + (b - a) * (i as f32 / steps as f32);
            let x0 = (p.x - half).round() as i32;
            let y0 = (p.y - half).round() as i32;
            for y in y0..y0 + size {
                for x in x0..x0 + size {
                    self.max_at(x, y, 1.0);
                }
            }
            out = out.union(IRect::new(x0, y0, x0 + size, y0 + size));
        }
        out.intersect(self.bounds())
    }

    /// Anti-aliased even-odd polygon fill: 4 vertical subsamples per pixel with
    /// exact horizontal span coverage.
    pub fn fill_polygon(&mut self, pts: &[Pos2], weight: f32) -> IRect {
        if pts.len() < 3 {
            return IRect::EMPTY;
        }
        let (mut lo, mut hi) = (pts[0], pts[0]);
        for p in pts {
            lo = lo.min(*p);
            hi = hi.max(*p);
        }
        let r = IRect::around(lo, hi, 0.0).intersect(self.bounds());
        if r.is_empty() {
            return r;
        }
        const SUB: usize = 4;
        let bw = r.width() as usize;
        let mut acc = vec![0f32; bw + 2];
        let mut xs: Vec<f32> = Vec::new();
        for y in r.y0..r.y1 {
            acc.fill(0.0);
            for s in 0..SUB {
                let sy = y as f32 + (s as f32 + 0.5) / SUB as f32;
                xs.clear();
                for i in 0..pts.len() {
                    let (p, q) = (pts[i], pts[(i + 1) % pts.len()]);
                    if (p.y <= sy) != (q.y <= sy) {
                        xs.push(p.x + (sy - p.y) * (q.x - p.x) / (q.y - p.y));
                    }
                }
                xs.sort_by(f32::total_cmp);
                for pair in xs.chunks_exact(2) {
                    let xa = (pair[0] - r.x0 as f32).max(0.0);
                    let xb = (pair[1] - r.x0 as f32).min(bw as f32);
                    if xb > xa {
                        add_span(&mut acc, xa, xb, 1.0 / SUB as f32);
                    }
                }
            }
            for (i, v) in acc[..bw].iter().enumerate() {
                if *v > 0.0 {
                    self.max_at(r.x0 + i as i32, y, v.min(1.0) * weight);
                }
            }
        }
        r
    }

    /// Stroke a polyline with round joins and caps.
    pub fn stroke_path(&mut self, pts: &[Pos2], closed: bool, radius: f32) -> IRect {
        let mut r = IRect::EMPTY;
        let n = pts.len();
        if n == 1 {
            return self.capsule(pts[0], pts[0], radius, 1.0);
        }
        let segs = if closed { n } else { n.saturating_sub(1) };
        for i in 0..segs {
            r = r.union(self.capsule(pts[i], pts[(i + 1) % n], radius, 1.0));
        }
        r
    }
}

fn add_span(acc: &mut [f32], xa: f32, xb: f32, w: f32) {
    let ia = xa.floor() as usize;
    let ib = xb.floor() as usize;
    if ia == ib {
        acc[ia] += (xb - xa) * w;
        return;
    }
    acc[ia] += (ia as f32 + 1.0 - xa) * w;
    for v in &mut acc[ia + 1..ib] {
        *v += w;
    }
    if ib < acc.len() {
        acc[ib] += (xb - ib as f32) * w;
    }
}

/// Source-over blend of `c` at alpha `a` onto `dst` (both unpremultiplied).
#[inline]
pub fn over(dst: Rgba8, c: Rgba8, a: f32) -> Rgba8 {
    if a <= 0.0 {
        return dst;
    }
    let a = a.min(1.0);
    let da = dst[3] as f32 / 255.0;
    let oa = a + da * (1.0 - a);
    if oa <= 0.0 {
        return [0, 0, 0, 0];
    }
    let mut out = [0u8; 4];
    for i in 0..3 {
        out[i] = ((c[i] as f32 * a + dst[i] as f32 * da * (1.0 - a)) / oa).round().clamp(0.0, 255.0) as u8;
    }
    out[3] = (oa * 255.0).round() as u8;
    out
}

/// Copy the pixels in `r` from `src` to `dst` (images of the same size).
pub fn copy_rect(dst: &mut RgbaImage, src: &RgbaImage, r: IRect) {
    let r = r.intersect(IRect::of_image(dst)).intersect(IRect::of_image(src));
    if r.is_empty() {
        return;
    }
    let (dw, sw) = (dst.width() as usize, src.width() as usize);
    let s: &[u8] = src;
    let d: &mut [u8] = dst;
    for y in r.y0 as usize..r.y1 as usize {
        let (x0, x1) = (r.x0 as usize, r.x1 as usize);
        d[(y * dw + x0) * 4..(y * dw + x1) * 4].copy_from_slice(&s[(y * sw + x0) * 4..(y * sw + x1) * 4]);
    }
}

/// Blend `color` onto `dst` through `mask` within `r`.
pub fn blend_mask(dst: &mut RgbaImage, mask: &Mask, color: Rgba8, opacity: f32, r: IRect) {
    let r = r.intersect(IRect::of_image(dst)).intersect(mask.bounds());
    if r.is_empty() {
        return;
    }
    let ca = color[3] as f32 / 255.0 * opacity;
    let w = dst.width() as usize;
    let buf: &mut [u8] = dst;
    for y in r.y0..r.y1 {
        for x in r.x0..r.x1 {
            let m = mask.get(x, y);
            if m <= 0.0 {
                continue;
            }
            let i = (y as usize * w + x as usize) * 4;
            let px = [buf[i], buf[i + 1], buf[i + 2], buf[i + 3]];
            buf[i..i + 4].copy_from_slice(&over(px, color, m * ca));
        }
    }
}

pub fn fill_rect(img: &mut RgbaImage, r: IRect, color: Rgba8) {
    let r = r.intersect(IRect::of_image(img));
    for y in r.y0.max(0)..r.y1 {
        for x in r.x0.max(0)..r.x1 {
            img.put_pixel(x as u32, y as u32, Rgba(color));
        }
    }
}

/// The pixels of `r` (clipped to the image) as a new image.
pub fn crop(img: &RgbaImage, r: IRect) -> RgbaImage {
    let r = r.intersect(IRect::of_image(img));
    if r.is_empty() {
        return RgbaImage::new(1, 1);
    }
    imageops::crop_imm(img, r.x0 as u32, r.y0 as u32, r.width() as u32, r.height() as u32).to_image()
}

/// Alpha-composite `src` onto `dst` with its top-left at (x, y). Pixels matching
/// `key` are skipped (Paint's "transparent selection").
pub fn blit_over(dst: &mut RgbaImage, src: &RgbaImage, x: i32, y: i32, key: Option<[u8; 3]>) -> IRect {
    let r = IRect::new(x, y, x + src.width() as i32, y + src.height() as i32).intersect(IRect::of_image(dst));
    if r.is_empty() {
        return r;
    }
    for py in r.y0..r.y1 {
        for px in r.x0..r.x1 {
            let s = src.get_pixel((px - x) as u32, (py - y) as u32).0;
            if s[3] == 0 || key.is_some_and(|k| k == [s[0], s[1], s[2]]) {
                continue;
            }
            let d = dst.get_pixel_mut(px as u32, py as u32);
            d.0 = if s[3] == 255 { s } else { over(d.0, s, s[3] as f32 / 255.0) };
        }
    }
    r
}

/// Scanline flood fill of the region connected to (x, y). Pixels whose channels
/// all lie within `tolerance` of the seed colour are replaced.
pub fn flood_fill(img: &mut RgbaImage, x: u32, y: u32, color: Rgba8, tolerance: u8) -> IRect {
    let (w, h) = img.dimensions();
    if x >= w || y >= h {
        return IRect::EMPTY;
    }
    let target = img.get_pixel(x, y).0;
    if target == color && tolerance == 0 {
        return IRect::EMPTY;
    }
    let matches = |p: Rgba8| p.iter().zip(target.iter()).all(|(a, b)| a.abs_diff(*b) <= tolerance);
    let mut visited = vec![false; w as usize * h as usize];
    let mut stack = vec![(x, y)];
    let mut out = IRect::EMPTY;
    while let Some((sx, sy)) = stack.pop() {
        let row = sy as usize * w as usize;
        if visited[row + sx as usize] || !matches(img.get_pixel(sx, sy).0) {
            continue;
        }
        let mut lx = sx;
        while lx > 0 && !visited[row + lx as usize - 1] && matches(img.get_pixel(lx - 1, sy).0) {
            lx -= 1;
        }
        let mut rx = sx;
        while rx + 1 < w && !visited[row + rx as usize + 1] && matches(img.get_pixel(rx + 1, sy).0) {
            rx += 1;
        }
        for px in lx..=rx {
            visited[row + px as usize] = true;
            img.put_pixel(px, sy, Rgba(color));
        }
        out = out.union(IRect::new(lx as i32, sy as i32, rx as i32 + 1, sy as i32 + 1));
        for ny in [sy.wrapping_sub(1), sy + 1] {
            if ny >= h {
                continue;
            }
            let nrow = ny as usize * w as usize;
            let mut px = lx;
            while px <= rx {
                if !visited[nrow + px as usize] && matches(img.get_pixel(px, ny).0) {
                    stack.push((px, ny));
                    while px <= rx && !visited[nrow + px as usize] && matches(img.get_pixel(px, ny).0) {
                        px += 1;
                    }
                } else {
                    px += 1;
                }
            }
        }
    }
    out.union(fill_antialiased_edges(img, &visited, target, color, tolerance))
}

/// Anti-aliased edges are blends of the old colour and the boundary's colour, so a plain
/// fill stops short of them and leaves a pale ring. Walk outwards from the filled area and
/// give each blended pixel the same mix of the new colour and the boundary colour. The walk
/// only continues while pixels get closer to the boundary colour, so it never crosses an
/// outline, and pixels that simply match the old colour (unconnected areas) are left alone.
fn fill_antialiased_edges(img: &mut RgbaImage, filled: &[bool], target: Rgba8, color: Rgba8, tolerance: u8) -> IRect {
    let (w, h) = img.dimensions();
    let at = |x: u32, y: u32| y as usize * w as usize + x as usize;
    let diff = |c: Rgba8| -> [f32; 4] { std::array::from_fn(|i| c[i] as f32 - target[i] as f32) };
    let dist2 = |c: Rgba8| diff(c).iter().map(|d| d * d).sum::<f32>();
    let neighbours4 = |x: u32, y: u32| {
        [(x.wrapping_sub(1), y), (x + 1, y), (x, y.wrapping_sub(1)), (x, y + 1)].into_iter().filter(move |&(nx, ny)| nx < w && ny < h)
    };
    let mut edge = vec![false; filled.len()];
    let mut stack: Vec<(u32, u32, f32)> = Vec::new();
    for y in 0..h {
        for x in 0..w {
            if filled[at(x, y)] {
                stack.extend(neighbours4(x, y).filter(|&(nx, ny)| !filled[at(nx, ny)]).map(|(nx, ny)| (nx, ny, 0.0)));
            }
        }
    }
    let min_dist = (tolerance as f32).max(8.0);
    let mut edits = Vec::new();
    while let Some((x, y, parent_t)) = stack.pop() {
        let i = at(x, y);
        if filled[i] || edge[i] {
            continue;
        }
        let n = img.get_pixel(x, y).0;
        if dist2(n).sqrt() <= min_dist {
            continue;
        }
        // The boundary colour: whichever unfilled pixel around here is least like the old colour.
        let mut boundary = n;
        for ny in y.saturating_sub(1)..=(y + 1).min(h - 1) {
            for nx in x.saturating_sub(1)..=(x + 1).min(w - 1) {
                let c = img.get_pixel(nx, ny).0;
                if !filled[at(nx, ny)] && !edge[at(nx, ny)] && dist2(c) > dist2(boundary) {
                    boundary = c;
                }
            }
        }
        let (dn, db) = (diff(n), diff(boundary));
        let len2 = db.iter().map(|d| d * d).sum::<f32>();
        let t = (dn.iter().zip(&db).map(|(a, b)| a * b).sum::<f32>() / len2).clamp(0.0, 1.0);
        if t >= 0.97 || t <= parent_t + 0.01 {
            continue;
        }
        edge[i] = true;
        edits.push((x, y, std::array::from_fn::<u8, 4, _>(|k| (color[k] as f32 * (1.0 - t) + boundary[k] as f32 * t).round() as u8)));
        stack.extend(neighbours4(x, y).map(|(nx, ny)| (nx, ny, t)));
    }
    let mut out = IRect::EMPTY;
    for (x, y, c) in edits {
        img.put_pixel(x, y, Rgba(c));
        out = out.union(IRect::new(x as i32, y as i32, x as i32 + 1, y as i32 + 1));
    }
    out
}

/// A `w`×`h` canvas holding `img` at the top-left, padded with `fill`.
pub fn resize_canvas(img: &RgbaImage, w: u32, h: u32, fill: Rgba8) -> RgbaImage {
    let mut out = RgbaImage::from_pixel(w.max(1), h.max(1), Rgba(fill));
    imageops::replace(&mut out, img, 0, 0);
    out
}

pub fn invert(img: &RgbaImage) -> RgbaImage {
    let mut out = img.clone();
    for p in out.pixels_mut() {
        p.0 = [255 - p.0[0], 255 - p.0[1], 255 - p.0[2], p.0[3]];
    }
    out
}
