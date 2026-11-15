//! Drawing egui's triangles into the window's pixels on the CPU, instead of on the GPU.
//!
//! It makes the pixels egui's own GPU renderer (egui-wgpu) makes: vertex colors and
//! textures are premultiplied and in gamma space, a pixel's color is the vertex color
//! times the texture, and it's blended as `out = src + dst * (1 - src.a)`. A pixel is
//! covered when its center is inside the triangle; on an edge two triangles share,
//! exactly one of them covers it (the "top-left" rule GPUs use), so seams between the
//! triangles of a translucent shape aren't blended twice.
//!
//! The window is split into bands of rows, drawn on separate threads.

use std::collections::HashMap;

use egui::epaint::textures::{TextureFilter, TextureWrapMode, TexturesDelta};
use egui::epaint::{ClippedPrimitive, ImageData, Primitive, TextureId, Vertex};

/// Positions are snapped to 1/256 of a pixel, so covering is exact (integer) arithmetic.
const SUB: f32 = 256.0;

/// Changed areas closer than this many pixels (in added area) are redrawn as one.
const MERGE_SLACK: i64 = 64 * 64;
/// At most this many separate areas are redrawn per frame.
const MAX_AREAS: usize = 12;

struct Texture {
    w: usize,
    h: usize,
    /// Premultiplied RGBA, as egui gives it.
    px: Vec<[u8; 4]>,
    mag: TextureFilter,
    min: TextureFilter,
    wrap: TextureWrapMode,
}

#[derive(Default)]
pub struct Renderer {
    textures: HashMap<TextureId, Texture>,
    /// What last frame's primitives were, for finding what changed
    last: Vec<Seen>,
}

/// What a primitive was, to compare with the next frame's.
struct Seen {
    /// Everything that decides its pixels (but its texture's pixels)
    hash: u64,
    /// Its clip rectangle and texture
    group: u64,
    /// The pixels it can touch
    bounds: Rect,
    /// Each triangle: a hash of its corners, and the pixels it can touch
    triangles: Vec<(u64, Rect)>,
}

impl Seen {
    fn of(p: &ClippedPrimitive, ppp: f32, screen: Rect) -> Self {
        let bounds = bounds(p, ppp, screen);
        let mut group = Hasher::new();
        group.clip_and_texture(p);
        let mut triangles = Vec::new();
        if let Primitive::Mesh(m) = &p.primitive
            && !bounds.is_empty()
        {
            let v = &m.vertices;
            triangles.reserve(m.indices.len() / 3);
            for t in m.indices.chunks_exact(3) {
                let (Some(a), Some(b), Some(c)) = (v.get(t[0] as usize), v.get(t[1] as usize), v.get(t[2] as usize)) else { continue };
                let mut h = Hasher::new();
                [a, b, c].iter().for_each(|v| h.vertex(v));
                let r = Rect::around(
                    (a.pos.x.min(b.pos.x).min(c.pos.x) * ppp, a.pos.y.min(b.pos.y).min(c.pos.y) * ppp),
                    (a.pos.x.max(b.pos.x).max(c.pos.x) * ppp, a.pos.y.max(b.pos.y).max(c.pos.y) * ppp),
                );
                triangles.push((h.0, r.intersect(bounds)));
            }
        }
        Self { hash: hash(p), group: group.0, bounds, triangles }
    }
}

/// The triangles of `now` that `was` didn't have, and those it had that `now` hasn't.
fn changed_triangles(was: &Seen, now: &Seen, out: &mut Vec<Rect>) {
    let mut left: HashMap<u64, usize> = HashMap::new();
    was.triangles.iter().for_each(|(h, _)| *left.entry(*h).or_default() += 1);
    for &(h, r) in &now.triangles {
        match left.get_mut(&h) {
            Some(n) if *n > 0 => *n -= 1,
            _ => out.push(r),
        }
    }
    let mut kept: HashMap<u64, usize> = HashMap::new();
    now.triangles.iter().for_each(|(h, _)| *kept.entry(*h).or_default() += 1);
    for &(h, r) in &was.triangles {
        match kept.get_mut(&h) {
            Some(n) if *n > 0 => *n -= 1,
            _ => out.push(r),
        }
    }
}

/// FNV-style hashing of 32-bit words
struct Hasher(u64);

impl Hasher {
    fn new() -> Self {
        Self(0xcbf2_9ce4_8422_2325)
    }

    fn add(&mut self, w: u32) {
        self.0 = (self.0 ^ w as u64).wrapping_mul(0x0000_0100_0000_01b3).rotate_left(5);
    }

    fn clip_and_texture(&mut self, p: &ClippedPrimitive) {
        let r = p.clip_rect;
        [r.min.x, r.min.y, r.max.x, r.max.y].iter().for_each(|v| self.add(v.to_bits()));
        if let Primitive::Mesh(m) = &p.primitive {
            let (kind, id) = match m.texture_id {
                TextureId::Managed(id) => (1, id),
                TextureId::User(id) => (2, id),
            };
            self.add(kind);
            self.add(id as u32);
            self.add((id >> 32) as u32);
        }
    }

    fn vertex(&mut self, v: &Vertex) {
        [v.pos.x, v.pos.y, v.uv.x, v.uv.y].iter().for_each(|f| self.add(f.to_bits()));
        self.add(u32::from_le_bytes(v.color.to_array()));
    }
}

/// Pixels `x0..x1` × `y0..y1`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x0: i32,
    pub y0: i32,
    pub x1: i32,
    pub y1: i32,
}

impl Rect {
    pub const EMPTY: Self = Self { x0: 0, y0: 0, x1: 0, y1: 0 };

    pub fn of_size([w, h]: [usize; 2]) -> Self {
        Self { x0: 0, y0: 0, x1: w as i32, y1: h as i32 }
    }

    pub fn is_empty(&self) -> bool {
        self.x0 >= self.x1 || self.y0 >= self.y1
    }

    pub fn area(&self) -> i64 {
        if self.is_empty() { 0 } else { (self.x1 - self.x0) as i64 * (self.y1 - self.y0) as i64 }
    }

    pub fn union(self, o: Self) -> Self {
        if self.is_empty() {
            return o;
        }
        if o.is_empty() {
            return self;
        }
        Self { x0: self.x0.min(o.x0), y0: self.y0.min(o.y0), x1: self.x1.max(o.x1), y1: self.y1.max(o.y1) }
    }

    pub fn intersect(self, o: Self) -> Self {
        Self { x0: self.x0.max(o.x0), y0: self.y0.max(o.y0), x1: self.x1.min(o.x1), y1: self.y1.min(o.y1) }
    }

    /// The pixels touched by points `min..max` (in pixels), plus a pixel all round for smoothing and filtering.
    fn around(min: (f32, f32), max: (f32, f32)) -> Self {
        Self { x0: min.0.floor() as i32 - 1, y0: min.1.floor() as i32 - 1, x1: max.0.ceil() as i32 + 1, y1: max.1.ceil() as i32 + 1 }
    }
}

/// One band of the window: rows `y0..y1`, `px` holds just those rows.
struct Band<'a> {
    px: &'a mut [u32],
    width: usize,
    y0: i32,
    y1: i32,
}

impl Renderer {
    /// New and changed textures: call before [`Self::render`].
    pub fn set_textures(&mut self, delta: &TexturesDelta) {
        for (id, d) in delta.set.iter().flat_map(|(id, ds)| ds.iter().map(move |d| (id, d))) {
            let ImageData::Color(img) = &d.image;
            let (mag, min, wrap) = (d.options.magnification, d.options.minification, d.options.wrap_mode);
            let src: Vec<[u8; 4]> = img.pixels.iter().map(|c| c.to_array()).collect();
            match d.pos {
                None => {
                    self.textures.insert(*id, Texture { w: img.size[0], h: img.size[1], px: src, mag, min, wrap });
                }
                Some([x, y]) => {
                    let Some(t) = self.textures.get_mut(id) else { continue };
                    let (w, h) = (img.size[0], img.size[1]);
                    if x + w > t.w || y + h > t.h {
                        continue;
                    }
                    for row in 0..h {
                        t.px[(y + row) * t.w + x..][..w].copy_from_slice(&src[row * w..][..w]);
                    }
                    (t.mag, t.min, t.wrap) = (mag, min, wrap);
                }
            }
        }
    }

    /// Textures egui no longer uses: call after [`Self::render`].
    pub fn free_textures(&mut self, delta: &TexturesDelta) {
        for id in &delta.free {
            self.textures.remove(id);
        }
    }

    /// The parts of the window that look different from the last frame this was asked
    /// about: primitives that appeared, went or changed (for a changed mesh, just its
    /// triangles that changed), and the parts of pictures whose pixels changed (`delta`,
    /// this frame's texture changes; call after [`Self::set_textures`]). Returns the whole
    /// window if things merely changed order.
    pub fn damage(&mut self, prims: &[ClippedPrimitive], delta: &TexturesDelta, size: [usize; 2], ppp: f32) -> Vec<Rect> {
        let screen = Rect::of_size(size);
        let now: Vec<Seen> = prims.iter().map(|p| Seen::of(p, ppp, screen)).collect();
        let last = std::mem::replace(&mut self.last, now);
        let now = &self.last;

        // Primitives unchanged from the last frame
        let mut unchanged: HashMap<u64, usize> = HashMap::new();
        last.iter().for_each(|s| *unchanged.entry(s.hash).or_default() += 1);
        let mut same_now = vec![false; now.len()];
        for (i, s) in now.iter().enumerate() {
            if let Some(n) = unchanged.get_mut(&s.hash).filter(|n| **n > 0) {
                *n -= 1;
                same_now[i] = true;
            }
        }
        let mut in_now: HashMap<u64, usize> = HashMap::new();
        now.iter().zip(&same_now).filter(|(_, same)| **same).for_each(|(s, _)| *in_now.entry(s.hash).or_default() += 1);
        let same_last: Vec<bool> = last
            .iter()
            .map(|s| match in_now.get_mut(&s.hash) {
                Some(n) if *n > 0 => {
                    *n -= 1;
                    true
                }
                _ => false,
            })
            .collect();

        // A changed one is paired with one that went, with the same clip and texture: the
        // same mesh with something in it changed (egui puts neighbouring shapes in one mesh).
        let mut went: HashMap<u64, std::collections::VecDeque<usize>> = HashMap::new();
        for (i, s) in last.iter().enumerate().filter(|(i, _)| !same_last[*i]) {
            went.entry(s.group).or_default().push_back(i);
        }
        let mut pair_of_last = vec![None; last.len()];
        let mut rects = Vec::new();
        let mut order_now = Vec::new();
        for (i, s) in now.iter().enumerate() {
            if same_now[i] {
                order_now.push(s.hash);
            } else if let Some(j) = went.get_mut(&s.group).and_then(|q| q.pop_front()) {
                pair_of_last[j] = Some(i);
                order_now.push(!(i as u64));
                changed_triangles(&last[j], s, &mut rects);
            } else {
                rects.push(s.bounds);
            }
        }
        let mut order_last = Vec::new();
        for (j, s) in last.iter().enumerate() {
            if same_last[j] {
                order_last.push(s.hash);
            } else if let Some(i) = pair_of_last[j] {
                order_last.push(!(i as u64));
            } else {
                rects.push(s.bounds);
            }
        }
        if order_now != order_last {
            // The same things in another order: what's on top may have changed anywhere
            return vec![screen];
        }

        // Pictures whose pixels changed: where the primitives showing them show those pixels
        for (id, deltas) in &delta.set {
            let Some(tex) = self.textures.get(id) else { continue };
            for d in deltas {
                let texels = match d.pos {
                    None => Rect::of_size([tex.w, tex.h]),
                    Some([x, y]) => Rect { x0: x as i32, y0: y as i32, x1: (x + d.image.width()) as i32, y1: (y + d.image.height()) as i32 },
                };
                for (p, s) in prims.iter().zip(now) {
                    if let Primitive::Mesh(m) = &p.primitive
                        && m.texture_id == *id
                        && !s.bounds.is_empty()
                    {
                        showing(m, texels, [tex.w, tex.h], ppp, s.bounds, &mut rects);
                    }
                }
            }
        }
        merge(rects, screen)
    }

    /// Draws `prims` over `clear` into `out` (`width` × `height` pixels, 0x00RRGGBB), only
    /// inside `areas` (the rest of `out` is left as it was), on up to `threads` threads.
    #[allow(clippy::too_many_arguments)]
    pub fn render(&self, out: &mut [u32], [width, height]: [usize; 2], ppp: f32, clear: u32, prims: &[ClippedPrimitive], areas: &[Rect], threads: usize) {
        if width == 0 || height == 0 || areas.is_empty() {
            return;
        }
        // Bands sized by the rows that are drawn, so every thread gets some
        let (top, bottom) = areas.iter().fold((height as i32, 0), |(t, b), r| (t.min(r.y0.max(0)), b.max(r.y1.min(height as i32))));
        if top >= bottom {
            return;
        }
        let drawn = (bottom - top) as usize;
        let threads = threads.clamp(1, 16).min(drawn.div_ceil(32));
        let rows = drawn.div_ceil(threads);
        let out = &mut out[top as usize * width..bottom as usize * width];
        if threads == 1 {
            self.render_band(&mut Band { px: out, width, y0: top, y1: bottom }, ppp, clear, prims, areas);
            return;
        }
        std::thread::scope(|s| {
            for (i, px) in out.chunks_mut(rows * width).enumerate() {
                let y0 = top + (i * rows) as i32;
                let y1 = y0 + (px.len() / width) as i32;
                s.spawn(move || self.render_band(&mut Band { px, width, y0, y1 }, ppp, clear, prims, areas));
            }
        });
    }

    fn render_band(&self, band: &mut Band, ppp: f32, clear: u32, prims: &[ClippedPrimitive], areas: &[Rect]) {
        let whole = Rect { x0: 0, y0: band.y0, x1: band.width as i32, y1: band.y1 };
        for area in areas {
            let area = area.intersect(whole);
            if area.is_empty() {
                continue;
            }
            for y in area.y0..area.y1 {
                let row = (y - band.y0) as usize * band.width;
                band.px[row + area.x0 as usize..row + area.x1 as usize].fill(clear);
            }
            for prim in prims {
                let Primitive::Mesh(mesh) = &prim.primitive else { continue };
                let Some(tex) = self.textures.get(&mesh.texture_id) else { continue };
                let clip = scissor(prim, ppp).intersect(area);
                if clip.is_empty() {
                    continue;
                }
                let v = &mesh.vertices;
                for t in mesh.indices.chunks_exact(3) {
                    let (Some(a), Some(b), Some(c)) = (v.get(t[0] as usize), v.get(t[1] as usize), v.get(t[2] as usize)) else { continue };
                    triangle(band, clip, tex, ppp, a, b, c);
                }
            }
        }
    }
}

/// A primitive's clip rectangle in pixels, rounded as egui-wgpu rounds its scissor rectangle.
fn scissor(p: &ClippedPrimitive, ppp: f32) -> Rect {
    let r = p.clip_rect;
    let px = |v: f32| ((v * ppp).round() as i64).clamp(i32::MIN as i64 / 2, i32::MAX as i64 / 2) as i32;
    Rect { x0: px(r.min.x), y0: px(r.min.y), x1: px(r.max.x), y1: px(r.max.y) }
}

/// The pixels a primitive can touch.
fn bounds(p: &ClippedPrimitive, ppp: f32, screen: Rect) -> Rect {
    let Primitive::Mesh(m) = &p.primitive else { return Rect::EMPTY };
    if m.vertices.is_empty() {
        return Rect::EMPTY;
    }
    let (mut min, mut max) = ((f32::MAX, f32::MAX), (f32::MIN, f32::MIN));
    for v in &m.vertices {
        min = (min.0.min(v.pos.x), min.1.min(v.pos.y));
        max = (max.0.max(v.pos.x), max.1.max(v.pos.y));
    }
    Rect::around((min.0 * ppp, min.1 * ppp), (max.0 * ppp, max.1 * ppp)).intersect(scissor(p, ppp)).intersect(screen)
}

/// Everything about a primitive that decides its pixels, except its texture's pixels.
fn hash(p: &ClippedPrimitive) -> u64 {
    let mut h = Hasher::new();
    h.clip_and_texture(p);
    if let Primitive::Mesh(m) = &p.primitive {
        m.vertices.iter().for_each(|v| h.vertex(v));
        m.indices.iter().for_each(|&i| h.add(i));
    }
    h.0
}

/// Where mesh `m` (inside `bounds`) shows texels `texels` of its `size` texture: for each
/// triangle sampling them, those texels mapped onto the screen.
fn showing(m: &egui::epaint::Mesh, texels: Rect, size: [usize; 2], ppp: f32, bounds: Rect, out: &mut Vec<Rect>) {
    let (tw, th) = (size[0] as f32, size[1] as f32);
    // A texel more all round: filtering reads neighbours
    let (u0, v0, u1, v1) = ((texels.x0 - 1) as f32 / tw, (texels.y0 - 1) as f32 / th, (texels.x1 + 1) as f32 / tw, (texels.y1 + 1) as f32 / th);
    let v = &m.vertices;
    for t in m.indices.chunks_exact(3) {
        let (Some(a), Some(b), Some(c)) = (v.get(t[0] as usize), v.get(t[1] as usize), v.get(t[2] as usize)) else { continue };
        let (umin, umax) = (a.uv.x.min(b.uv.x).min(c.uv.x), a.uv.x.max(b.uv.x).max(c.uv.x));
        let (vmin, vmax) = (a.uv.y.min(b.uv.y).min(c.uv.y), a.uv.y.max(b.uv.y).max(c.uv.y));
        if umax < u0 || umin > u1 || vmax < v0 || vmin > v1 {
            continue;
        }
        let tri = Rect::around(
            (a.pos.x.min(b.pos.x).min(c.pos.x) * ppp, a.pos.y.min(b.pos.y).min(c.pos.y) * ppp),
            (a.pos.x.max(b.pos.x).max(c.pos.x) * ppp, a.pos.y.max(b.pos.y).max(c.pos.y) * ppp),
        )
        .intersect(bounds);
        // The texture is laid on the triangle by a linear map from texture to screen: where the texels land
        let (du1, du2) = ((b.uv.x - a.uv.x, b.uv.y - a.uv.y), (c.uv.x - a.uv.x, c.uv.y - a.uv.y));
        let det = du1.0 * du2.1 - du2.0 * du1.1;
        if det.abs() < 1e-12 {
            out.push(tri);
            continue;
        }
        let (dp1, dp2) = ((b.pos.x - a.pos.x, b.pos.y - a.pos.y), (c.pos.x - a.pos.x, c.pos.y - a.pos.y));
        let map = |u: f32, w: f32| {
            let (du, dv) = (u - a.uv.x, w - a.uv.y);
            let (s, t) = ((du * du2.1 - dv * du2.0) / det, (du1.0 * dv - du1.1 * du) / det);
            ((a.pos.x + s * dp1.0 + t * dp2.0) * ppp, (a.pos.y + s * dp1.1 + t * dp2.1) * ppp)
        };
        let corners = [map(u0, v0), map(u1, v0), map(u0, v1), map(u1, v1)];
        let min = corners.iter().fold((f32::MAX, f32::MAX), |m, c| (m.0.min(c.0), m.1.min(c.1)));
        let max = corners.iter().fold((f32::MIN, f32::MIN), |m, c| (m.0.max(c.0), m.1.max(c.1)));
        out.push(Rect::around(min, max).intersect(tri));
    }
}

/// A few rectangles covering `rects`: ones that overlap or nearly touch become one.
pub fn merge(rects: Vec<Rect>, screen: Rect) -> Vec<Rect> {
    let mut out: Vec<Rect> = Vec::new();
    for r in rects.into_iter().map(|r| r.intersect(screen)).filter(|r| !r.is_empty()) {
        out.push(r);
        // Fold the new one into others while that costs little
        let mut i = out.len() - 1;
        let mut j = 0;
        while j < out.len() {
            if j != i && out[i].union(out[j]).area() <= out[i].area() + out[j].area() + MERGE_SLACK {
                let u = out[i].union(out[j]);
                out.swap_remove(j.max(i));
                out.swap_remove(j.min(i));
                out.push(u);
                (i, j) = (out.len() - 1, 0);
            } else {
                j += 1;
            }
        }
    }
    while out.len() > MAX_AREAS {
        // Merge the pair that grows least
        let mut best = (i64::MAX, 0, 1);
        for i in 0..out.len() {
            for j in i + 1..out.len() {
                let grow = out[i].union(out[j]).area() - out[i].area() - out[j].area();
                if grow < best.0 {
                    best = (grow, i, j);
                }
            }
        }
        let u = out[best.1].union(out[best.2]);
        out.swap_remove(best.2);
        out[best.1] = u;
    }
    out
}

/// A vertex value spread across a triangle, as a function of the pixel. It's worked out from
/// the vertices alone and evaluated at absolute pixel coordinates, so a pixel gets the same
/// value however the window is split into bands or redrawn areas.
#[derive(Clone, Copy)]
struct Plane {
    /// At the center of pixel (0, 0)
    at: f64,
    dx: f64,
    dy: f64,
}

impl Plane {
    /// At the center of pixel (0, y): add `dx() * x` for pixel (x, y).
    fn row(&self, y: i32) -> f32 {
        (self.at + self.dy * y as f64) as f32
    }

    fn dx(&self) -> f32 {
        self.dx as f32
    }
}

fn triangle(band: &mut Band, clip: Rect, tex: &Texture, ppp: f32, a: &Vertex, b: &Vertex, c: &Vertex) {
    let fix = |v: &Vertex| ((v.pos.x * ppp * SUB).round() as i64, (v.pos.y * ppp * SUB).round() as i64);
    let (pa, mut pb, mut pc) = (fix(a), fix(b), fix(c));
    let (mut b, mut c) = (b, c);
    let area = (pb.0 - pa.0) * (pc.1 - pa.1) - (pb.1 - pa.1) * (pc.0 - pa.0);
    if area == 0 {
        return;
    }
    if area < 0 {
        // Corners in the order that makes the edge functions positive inside
        std::mem::swap(&mut pb, &mut pc);
        std::mem::swap(&mut b, &mut c);
    }
    // Pixels whose centers can be inside
    let sub = SUB as i64;
    let x0 = (pa.0.min(pb.0).min(pc.0).div_euclid(sub) as i32).max(clip.x0);
    let x1 = ((pa.0.max(pb.0).max(pc.0) + sub - 1).div_euclid(sub) as i32).min(clip.x1);
    let y0 = (pa.1.min(pb.1).min(pc.1).div_euclid(sub) as i32).max(clip.y0);
    let y1 = ((pa.1.max(pb.1).max(pc.1) + sub - 1).div_euclid(sub) as i32).min(clip.y1);
    if x0 >= x1 || y0 >= y1 {
        return;
    }

    // Edge functions, each positive inside: e_ab is zero on edge a→b and grows towards c.
    // At the center of pixel (x0, y0), and their steps per pixel.
    let (cx, cy) = (x0 as i64 * sub + sub / 2, y0 as i64 * sub + sub / 2);
    let edge = |p: (i64, i64), q: (i64, i64)| {
        let (dx, dy) = (q.0 - p.0, q.1 - p.1);
        let at = dx * (cy - p.1) - dy * (cx - p.0);
        // Top-left rule: a pixel center exactly on the edge is inside for one of the
        // two triangles sharing the edge (which walk it in opposite directions).
        let bias = if dy > 0 || (dy == 0 && dx < 0) { 0 } else { -1 };
        (at + bias, -dy * sub, dx * sub)
    };
    let (e_bc, e_ca, e_ab) = (edge(pb, pc), edge(pc, pa), edge(pa, pb));

    // Vertex values across the triangle (from the snapped positions, in pixels)
    let px = |p: (i64, i64)| (p.0 as f64 / SUB as f64, p.1 as f64 / SUB as f64);
    let (fa, fb, fc) = (px(pa), px(pb), px(pc));
    let det = (fb.0 - fa.0) * (fc.1 - fa.1) - (fb.1 - fa.1) * (fc.0 - fa.0);
    let plane = |va: f32, vb: f32, vc: f32| {
        let (db, dc) = ((vb - va) as f64, (vc - va) as f64);
        let dx = (db * (fc.1 - fa.1) - dc * (fb.1 - fa.1)) / det;
        let dy = (dc * (fb.0 - fa.0) - db * (fc.0 - fa.0)) / det;
        Plane { at: va as f64 + dx * (0.5 - fa.0) + dy * (0.5 - fa.1), dx, dy }
    };
    let same_uv = a.uv == b.uv && a.uv == c.uv;
    let same_color = a.color == b.color && a.color == c.color;

    let n = (x1 - x0) as i64;
    // The pixels of row y whose centers are inside: x0 + lo .. x0 + hi. Each edge
    // function is linear along the row, so where it turns negative is one division.
    let span = |y: i32| -> Option<(usize, usize)> {
        let k = (y - y0) as i64;
        let (mut lo, mut hi) = (0, n);
        for (at, step, down) in [e_bc, e_ca, e_ab] {
            let at = at + down * k;
            if step > 0 {
                lo = lo.max((-at + step - 1).div_euclid(step));
            } else if step < 0 {
                hi = hi.min(at.div_euclid(-step) + 1);
            } else if at < 0 {
                return None;
            }
        }
        (lo < hi).then_some((lo as usize, hi as usize))
    };
    let (width, band_y0) = (band.width, band.y0);
    let row_of = |y: i32| (y - band_y0) as usize * width + x0 as usize;

    if same_uv {
        let t = sample_nearest(tex, a.uv.x, a.uv.y);
        if same_color {
            // One color for the whole triangle (most of the UI)
            let s = mul(a.color.to_array(), t);
            if s == [0; 4] {
                return;
            }
            for y in y0..y1 {
                let Some((lo, hi)) = span(y) else { continue };
                let px = &mut band.px[row_of(y)..][lo..hi];
                if s[3] == 255 {
                    px.fill(pack(s));
                } else {
                    px.iter_mut().for_each(|p| *p = blend(*p, s));
                }
            }
        } else {
            // Edges of shapes, faded out over a pixel for smoothing: the color varies.
            let ch = |i: usize| plane(a.color[i] as f32, b.color[i] as f32, c.color[i] as f32);
            let col = [ch(0), ch(1), ch(2), ch(3)];
            let tf = t.map(|v| v as f32 / 255.0);
            let cdx = col.map(|p| p.dx());
            for y in y0..y1 {
                let Some((lo, hi)) = span(y) else { continue };
                let start = col.map(|p| p.row(y));
                for (j, p) in band.px[row_of(y)..][lo..hi].iter_mut().enumerate() {
                    let x = (x0 as usize + lo + j) as f32;
                    let s = [0, 1, 2, 3].map(|n| ((start[n] + cdx[n] * x) * tf[n]).round().clamp(0.0, 255.0) as u8);
                    *p = blend(*p, s);
                }
            }
        }
        return;
    }

    // Textured: pictures, icons, and text (from egui's font texture)
    let u = plane(a.uv.x, b.uv.x, c.uv.x);
    let v = plane(a.uv.y, b.uv.y, c.uv.y);
    // Texels per pixel decides between the texture's magnifying and minifying filter (as a GPU does, per triangle here)
    let scale = (u.dx.abs() * tex.w as f64).max(v.dx.abs() * tex.h as f64).max(u.dy.abs() * tex.w as f64).max(v.dy.abs() * tex.h as f64);
    let filter = if scale > 1.0 { tex.min } else { tex.mag };
    let col = if same_color { None } else { Some([0, 1, 2, 3].map(|i| plane(a.color[i] as f32, b.color[i] as f32, c.color[i] as f32))) };
    let tint = a.color.to_array();
    let white = same_color && tint == [255; 4];
    let (tw, th) = (tex.w as f32, tex.h as f32);
    let (udx, vdx) = (u.dx(), v.dx());
    let cdx = col.map(|col| col.map(|p| p.dx()));
    for y in y0..y1 {
        let Some((lo, hi)) = span(y) else { continue };
        let (ur, vr) = (u.row(y), v.row(y));
        let first = x0 as usize + lo;
        let px = &mut band.px[row_of(y)..][lo..hi];
        if white && filter == TextureFilter::Nearest && v.dx == 0.0 {
            // An upright picture at its own colors (the canvas, icons): one texture row per screen row
            let ty = wrap((vr * th).floor() as i64, tex.h, tex.wrap);
            let trow = &tex.px[ty * tex.w..][..tex.w];
            for (j, p) in px.iter_mut().enumerate() {
                let t = trow[wrap(((ur + udx * (first + j) as f32) * tw).floor() as i64, tex.w, tex.wrap)];
                *p = if t[3] == 255 { pack(t) } else { blend(*p, t) };
            }
            continue;
        }
        let start = col.map(|col| col.map(|p| p.row(y)));
        for (j, p) in px.iter_mut().enumerate() {
            let i = (first + j) as f32;
            let (uu, vv) = (ur + udx * i, vr + vdx * i);
            let t = match filter {
                TextureFilter::Nearest => sample_nearest(tex, uu, vv),
                TextureFilter::Linear => sample_linear(tex, uu, vv),
            };
            let s = if white {
                t
            } else if let (Some(cdx), Some(start)) = (&cdx, &start) {
                [0, 1, 2, 3].map(|n| ((start[n] + cdx[n] * i) * t[n] as f32 / 255.0).round().clamp(0.0, 255.0) as u8)
            } else {
                mul(tint, t)
            };
            *p = if s[3] == 255 { pack(s) } else { blend(*p, s) };
        }
    }
}

fn wrap(i: i64, n: usize, mode: TextureWrapMode) -> usize {
    let n = n as i64;
    (match mode {
        TextureWrapMode::ClampToEdge => i.clamp(0, n - 1),
        TextureWrapMode::Repeat if n & (n - 1) == 0 => i & (n - 1),
        TextureWrapMode::Repeat => i.rem_euclid(n),
        TextureWrapMode::MirroredRepeat => {
            let m = i.rem_euclid(2 * n);
            if m >= n { 2 * n - 1 - m } else { m }
        }
    }) as usize
}

fn sample_nearest(t: &Texture, u: f32, v: f32) -> [u8; 4] {
    let x = wrap((u * t.w as f32).floor() as i64, t.w, t.wrap);
    let y = wrap((v * t.h as f32).floor() as i64, t.h, t.wrap);
    t.px[y * t.w + x]
}

fn sample_linear(t: &Texture, u: f32, v: f32) -> [u8; 4] {
    let (fx, fy) = (u * t.w as f32 - 0.5, v * t.h as f32 - 0.5);
    let (x, y) = (fx.floor(), fy.floor());
    let (ax, ay) = (((fx - x) * 256.0) as u32, ((fy - y) * 256.0) as u32);
    let (x, y) = (x as i64, y as i64);
    let (x0, x1) = (wrap(x, t.w, t.wrap), wrap(x + 1, t.w, t.wrap));
    let (y0, y1) = (wrap(y, t.h, t.wrap) * t.w, wrap(y + 1, t.h, t.wrap) * t.w);
    let (p00, p10, p01, p11) = (t.px[y0 + x0], t.px[y0 + x1], t.px[y1 + x0], t.px[y1 + x1]);
    let mut out = [0u8; 4];
    for n in 0..4 {
        let top = p00[n] as u32 * (256 - ax) + p10[n] as u32 * ax;
        let bot = p01[n] as u32 * (256 - ax) + p11[n] as u32 * ax;
        out[n] = ((top * (256 - ay) + bot * ay + (1 << 15)) >> 16) as u8;
    }
    out
}

/// Vertex color times texture color (both premultiplied, 0–255)
fn mul(c: [u8; 4], t: [u8; 4]) -> [u8; 4] {
    [0, 1, 2, 3].map(|n| ((c[n] as u32 * t[n] as u32 + 127) / 255) as u8)
}

fn pack(s: [u8; 4]) -> u32 {
    (s[0] as u32) << 16 | (s[1] as u32) << 8 | s[2] as u32
}

/// `src + dst * (1 - src.a)`, src premultiplied
fn blend(dst: u32, s: [u8; 4]) -> u32 {
    let k = 255 - s[3] as u32;
    let ch = |shift: u32, sc: u8| {
        let d = (dst >> shift) & 255;
        (sc as u32 + (d * k + 127) / 255).min(255) << shift
    };
    ch(16, s[0]) | ch(8, s[1]) | ch(0, s[2])
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui::epaint::{ClippedPrimitive, Color32, ImageDelta, Mesh, Pos2, pos2};
    use egui::TextureOptions;

    fn renderer() -> Renderer {
        let mut r = Renderer::default();
        let mut d = TexturesDelta::default();
        let white = egui::ColorImage::new([1, 1], vec![Color32::WHITE]);
        d.set.insert(TextureId::default(), [ImageDelta::full(white, TextureOptions::LINEAR)].into_iter().collect());
        r.set_textures(&d);
        r
    }

    fn draw(mesh: Mesh, w: usize, h: usize, threads: usize) -> Vec<u32> {
        let mut out = vec![0; w * h];
        let prim = ClippedPrimitive { clip_rect: egui::Rect::from_min_max(Pos2::ZERO, pos2(w as f32, h as f32)), primitive: Primitive::Mesh(mesh) };
        renderer().render(&mut out, [w, h], 1.0, 0, &[prim], &[Rect::of_size([w, h])], threads);
        out
    }

    #[test]
    fn rectangle_covers_exactly_its_pixels() {
        let mut m = Mesh::default();
        m.add_colored_rect(egui::Rect::from_min_max(pos2(2.0, 1.0), pos2(6.0, 4.0)), Color32::RED);
        let out = draw(m, 8, 6, 1);
        for y in 0..6 {
            for x in 0..8 {
                let inside = (2..6).contains(&x) && (1..4).contains(&y);
                assert_eq!(out[y * 8 + x], if inside { 0xFF0000 } else { 0 }, "pixel {x},{y}");
            }
        }
    }

    #[test]
    fn translucent_triangles_sharing_an_edge_blend_once() {
        // Two triangles of a half-transparent square: the diagonal must not be darker.
        let mut m = Mesh::default();
        m.add_colored_rect(egui::Rect::from_min_max(pos2(0.0, 0.0), pos2(16.0, 16.0)), Color32::from_white_alpha(128));
        let out = draw(m, 16, 16, 1);
        assert!(out.iter().all(|&p| p == out[0]), "seam on the diagonal");
        assert_eq!(out[0] & 255, 128);
    }

    #[test]
    fn threads_draw_the_same_pixels() {
        let mut m = Mesh::default();
        m.colored_vertex(pos2(3.3, 1.7), Color32::RED);
        m.colored_vertex(pos2(90.1, 40.2), Color32::from_rgba_premultiplied(0, 100, 0, 100));
        m.colored_vertex(pos2(20.5, 97.9), Color32::BLUE);
        m.add_triangle(0, 1, 2);
        assert_eq!(draw(m.clone(), 100, 100, 1), draw(m, 100, 100, 7));
    }

    /// The pixels a triangle covers, one pixel at a time: the rule the renderer's row spans must match.
    fn covered(p: [(f32, f32); 3], w: usize, h: usize) -> Vec<bool> {
        let f = |q: (f32, f32)| ((q.0 * SUB).round() as i64, (q.1 * SUB).round() as i64);
        let (a, mut b, mut c) = (f(p[0]), f(p[1]), f(p[2]));
        let area = (b.0 - a.0) * (c.1 - a.1) - (b.1 - a.1) * (c.0 - a.0);
        if area < 0 {
            std::mem::swap(&mut b, &mut c);
        }
        let inside = |p: (i64, i64), q: (i64, i64), x: i64, y: i64| {
            let (dx, dy) = (q.0 - p.0, q.1 - p.1);
            let e = dx * (y - p.1) - dy * (x - p.0);
            e > 0 || (e == 0 && (dy > 0 || (dy == 0 && dx < 0)))
        };
        (0..w * h)
            .map(|i| {
                let (x, y) = ((i % w) as i64 * 256 + 128, (i / w) as i64 * 256 + 128);
                area != 0 && inside(b, c, x, y) && inside(c, a, x, y) && inside(a, b, x, y)
            })
            .collect()
    }

    #[test]
    fn row_spans_cover_the_same_pixels_as_testing_each_pixel() {
        let mut seed = 12345u64;
        let mut rnd = |range: f32| {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            (seed >> 40) as f32 / (1u64 << 24) as f32 * range - 4.0
        };
        let (w, h) = (40, 30);
        for n in 0..3000 {
            // Whole and half pixels often, to hit pixel centers exactly on edges
            let mut q = || {
                let (x, y) = (rnd(48.0), rnd(38.0));
                if n % 3 == 0 { ((x * 2.0).round() / 2.0, (y * 2.0).round() / 2.0) } else { (x, y) }
            };
            let p = [q(), q(), q()];
            let mut m = Mesh::default();
            for &(x, y) in &p {
                m.colored_vertex(pos2(x, y), Color32::WHITE);
            }
            m.add_triangle(0, 1, 2);
            let got: Vec<bool> = draw(m, w, h, 1).iter().map(|&px| px != 0).collect();
            assert_eq!(got, covered(p, w, h), "triangle {p:?}");
        }
    }

    /// A picture's pixels, as egui hands them over.
    fn picture(w: usize, h: usize, seed: u8) -> egui::ColorImage {
        let px = (0..w * h).map(|i| Color32::from_rgb(((i % w) as u8 * 3) ^ seed, (i / w) as u8 * 5, seed.wrapping_mul(7))).collect();
        egui::ColorImage::new([w, h], px)
    }

    fn quad(rect: Rect, tex: TextureId, color: Color32) -> ClippedPrimitive {
        let mut m = Mesh::with_texture(tex);
        let r = egui::Rect::from_min_max(pos2(rect.x0 as f32, rect.y0 as f32), pos2(rect.x1 as f32, rect.y1 as f32));
        m.add_rect_with_uv(r, egui::Rect::from_min_max(Pos2::ZERO, pos2(1.0, 1.0)), color);
        ClippedPrimitive { clip_rect: egui::Rect::EVERYTHING, primitive: Primitive::Mesh(m) }
    }

    #[test]
    fn redrawing_only_what_changed_gives_the_same_pixels() {
        let (w, h) = (120, 90);
        let size = [w, h];
        let pic = TextureId::Managed(7);
        let mut r = renderer();
        let mut first = TexturesDelta::default();
        first.set.insert(pic, [ImageDelta::full(picture(40, 30, 1), TextureOptions::NEAREST)].into_iter().collect());
        r.set_textures(&first);
        let scene = |shape_at: i32| {
            vec![
                quad(Rect { x0: 0, y0: 0, x1: 120, y1: 90 }, TextureId::default(), Color32::from_rgb(200, 210, 220)),
                // The picture, shown at twice its size
                quad(Rect { x0: 10, y0: 10, x1: 90, y1: 70 }, pic, Color32::WHITE),
                quad(Rect { x0: shape_at, y0: 50, x1: shape_at + 12, y1: 62 }, TextureId::default(), Color32::from_rgba_premultiplied(0, 0, 120, 160)),
            ]
        };
        let a = scene(20);
        let mut screen = vec![0u32; w * h];
        let areas = r.damage(&a, &first, size, 1.0);
        r.render(&mut screen, size, 1.0, 0, &a, &areas, 1);

        // The shape moves and a few of the picture's pixels change
        let b = scene(60);
        let mut change = TexturesDelta::default();
        change.set.insert(pic, [ImageDelta::partial([5, 4], picture(3, 2, 9), TextureOptions::NEAREST)].into_iter().collect());
        r.set_textures(&change);
        let areas = r.damage(&b, &change, size, 1.0);
        let changed: i64 = areas.iter().map(Rect::area).sum();
        assert!(changed < (w * h / 4) as i64, "redraws {changed} of {} pixels: {areas:?}", w * h);
        r.render(&mut screen, size, 1.0, 0, &b, &areas, 3);

        let mut whole = vec![0u32; w * h];
        r.render(&mut whole, size, 1.0, 0, &b, &[Rect::of_size(size)], 1);
        assert!(screen == whole, "differs from drawing everything");

        // Nothing changed: nothing to redraw
        assert!(r.damage(&b, &TexturesDelta::default(), size, 1.0).is_empty());
    }

    #[test]
    fn a_big_picture_samples_the_same_texels_on_any_number_of_threads() {
        let (w, h) = (1000, 640);
        let pic = TextureId::Managed(33);
        let mut r = renderer();
        let (tw, th) = (1152, 648);
        let px = (0..tw * th).map(|i| Color32::from_rgb((i % 251) as u8, (i / tw % 251) as u8, (i % 7) as u8 * 30)).collect();
        let mut d = TexturesDelta::default();
        d.set.insert(pic, [ImageDelta::full(egui::ColorImage::new([tw, th], px), TextureOptions::NEAREST)].into_iter().collect());
        r.set_textures(&d);
        let mut m = Mesh::with_texture(pic);
        m.add_rect_with_uv(egui::Rect::from_min_max(pos2(-234.0, 45.0), pos2(1494.0, 1017.0)), egui::Rect::from_min_max(Pos2::ZERO, pos2(1.0, 1.0)), Color32::WHITE);
        let prims = [ClippedPrimitive { clip_rect: egui::Rect::from_min_max(pos2(0.0, 146.0), pos2(984.0, 600.0)), primitive: Primitive::Mesh(m) }];
        let one = { let mut b = vec![0u32; w * h]; r.render(&mut b, [w, h], 1.0, 0, &prims, &[Rect::of_size([w, h])], 1); b };
        for t in [2, 3, 4, 8] {
            let mut b = vec![0u32; w * h];
            r.render(&mut b, [w, h], 1.0, 0, &prims, &[Rect::of_size([w, h])], t);
            if let Some(i) = (0..b.len()).find(|&i| b[i] != one[i]) {
                panic!("{t} threads: pixel {},{} is {:06x}, one thread {:06x}", i % w, i / w, b[i], one[i]);
            }
        }
    }
}
