use eframe::egui::pos2;
use image::{Rgba, RgbaImage};

use crate::brush::BrushKind;
use crate::history::{Change, History};
use crate::raster::{self, IRect, Mask};
use crate::shapes::{self, ShapeKind};
use crate::text::{self, FontLibrary};

fn coverage_sum(m: &Mask, r: IRect) -> f32 {
    let mut s = 0.0;
    for y in r.y0..r.y1 {
        for x in r.x0..r.x1 {
            s += m.get(x, y);
        }
    }
    s
}

#[test]
fn polygon_fill_covers_its_area() {
    let mut m = Mask::new(100, 100);
    // 30×20 rectangle at a fractional offset, plus a triangle of area 200.
    let r = m.fill_polygon(&[pos2(10.5, 10.25), pos2(40.5, 10.25), pos2(40.5, 30.25), pos2(10.5, 30.25)], 1.0);
    assert!((coverage_sum(&m, r) - 600.0).abs() < 1.0, "rect area {}", coverage_sum(&m, r));
    let mut m = Mask::new(100, 100);
    let r = m.fill_polygon(&[pos2(50.0, 50.0), pos2(70.0, 50.0), pos2(50.0, 70.0)], 1.0);
    assert!((coverage_sum(&m, r) - 200.0).abs() < 2.0, "triangle area {}", coverage_sum(&m, r));
}

#[test]
fn capsule_is_antialiased_and_bounded() {
    let mut m = Mask::new(64, 64);
    let r = m.capsule(pos2(10.0, 32.3), pos2(50.0, 32.3), 4.0, 1.0);
    assert_eq!(m.get(30, 31), 1.0);
    assert_eq!(m.get(30, 20), 0.0);
    let edge = m.get(30, 36);
    assert!(edge > 0.0 && edge < 1.0, "edge coverage {edge}");
    // Roughly length × width + a disc for the caps.
    let area = coverage_sum(&m, r);
    let expected = 40.0 * 8.0 + std::f32::consts::PI * 16.0;
    assert!((area - expected).abs() < expected * 0.06, "area {area} vs {expected}");
}

#[test]
fn square_line_is_continuous() {
    let mut m = Mask::new(40, 40);
    m.square_line(pos2(2.0, 2.0), pos2(30.0, 17.0), 1);
    for x in 2..30 {
        assert!((0..40).any(|y| m.get(x, y) == 1.0), "gap at x={x}");
    }
}

#[test]
fn flood_fill_respects_boundaries() {
    let mut img = RgbaImage::from_pixel(20, 20, Rgba([255, 255, 255, 255]));
    for i in 0..20 {
        img.put_pixel(10, i, Rgba([0, 0, 0, 255]));
    }
    let r = raster::flood_fill(&mut img, 2, 2, [255, 0, 0, 255], 0);
    assert_eq!(r, IRect::new(0, 0, 10, 20));
    assert_eq!(img.get_pixel(9, 19).0, [255, 0, 0, 255]);
    assert_eq!(img.get_pixel(10, 5).0, [0, 0, 0, 255]);
    assert_eq!(img.get_pixel(11, 5).0, [255, 255, 255, 255]);
    // Filling with the same colour is a no-op.
    assert!(raster::flood_fill(&mut img, 2, 2, [255, 0, 0, 255], 0).is_empty());
}

#[test]
fn flood_fill_tolerance_terminates_when_fill_matches_target() {
    let mut img = RgbaImage::from_pixel(8, 8, Rgba([200, 200, 200, 255]));
    let r = raster::flood_fill(&mut img, 0, 0, [205, 205, 205, 255], 20);
    assert_eq!(r, IRect::new(0, 0, 8, 8));
}

#[test]
fn blend_over_mixes_colours() {
    assert_eq!(raster::over([255, 255, 255, 255], [0, 0, 0, 255], 1.0), [0, 0, 0, 255]);
    assert_eq!(raster::over([255, 255, 255, 255], [0, 0, 0, 255], 0.0), [255, 255, 255, 255]);
    let half = raster::over([255, 255, 255, 255], [0, 0, 0, 255], 0.5);
    assert!((126..=129).contains(&half[0]) && half[3] == 255, "{half:?}");
    // Painting onto transparency keeps the paint colour.
    assert_eq!(raster::over([0, 0, 0, 0], [10, 20, 30, 255], 0.5), [10, 20, 30, 128]);
}

#[test]
fn history_round_trips_patches_and_replacements() {
    let original = RgbaImage::from_pixel(16, 16, Rgba([255, 255, 255, 255]));
    let mut img = original.clone();
    let mut h = History::default();

    raster::fill_rect(&mut img, IRect::new(2, 3, 6, 9), [0, 0, 255, 255]);
    let after_patch = img.clone();
    h.push(Change::patch(&original, &img, IRect::new(0, 0, 16, 16)).expect("pixels changed"));

    let before_rotate = img.clone();
    img = image::imageops::rotate90(&img);
    h.push(Change::Replace { before: before_rotate, after: img.clone() });

    assert!(h.undo(&mut img));
    assert_eq!(img, after_patch);
    assert!(h.undo(&mut img));
    assert_eq!(img, original);
    assert!(!h.undo(&mut img));
    assert!(h.redo(&mut img));
    assert_eq!(img, after_patch);
    // Identical before/after is not recorded.
    assert!(Change::patch(&original, &original, IRect::new(0, 0, 16, 16)).is_none());
}

#[test]
fn shapes_fit_their_box() {
    for kind in ShapeKind::ALL {
        let contours = kind.contours(pos2(10.0, 20.0), pos2(110.0, 70.0));
        assert!(contours[0].len() >= 2, "{kind:?}");
        for p in contours.iter().flatten() {
            assert!(p.x >= 9.99 && p.x <= 110.01 && p.y >= 19.99 && p.y <= 70.01, "{kind:?} point {p:?} outside box");
        }
    }
    let sq = shapes::constrain(ShapeKind::Rectangle, pos2(0.0, 0.0), pos2(30.0, -10.0), true);
    assert_eq!(sq, pos2(30.0, -30.0));
    let line = shapes::constrain(ShapeKind::Line, pos2(0.0, 0.0), pos2(50.0, 3.0), true);
    assert!(line.y.abs() < 1e-3 && (line.x - 50.0).abs() < 1e-3);
}

#[test]
fn every_brush_lays_paint() {
    let mut rng = 0x1234_5678_9abc_def0;
    for kind in BrushKind::ALL {
        let mut m = Mask::new(80, 80);
        let r = kind.segment(&mut m, pos2(20.0, 40.0), pos2(60.0, 40.0), 12.0, &mut rng);
        assert!(!r.is_empty() && coverage_sum(&m, r) > 10.0, "{kind:?}");
    }
}

#[test]
fn resize_canvas_pads_with_fill() {
    let img = RgbaImage::from_pixel(4, 4, Rgba([1, 2, 3, 255]));
    let out = raster::resize_canvas(&img, 6, 3, [9, 9, 9, 255]);
    assert_eq!(out.dimensions(), (6, 3));
    assert_eq!(out.get_pixel(3, 2).0, [1, 2, 3, 255]);
    assert_eq!(out.get_pixel(5, 0).0, [9, 9, 9, 255]);
}

#[test]
fn text_renders_with_a_system_font() {
    let mut lib = FontLibrary::scan();
    assert!(!lib.families.is_empty(), "no fonts found");
    let fam = lib.default_family();
    let font = lib.load(fam, false, false).expect("default font loads");
    let layout = text::measure(&font, 32.0, "Hello\nWorld");
    assert!(layout.width > 60.0 && layout.height > 60.0, "{}x{}", layout.width, layout.height);
    let mut m = Mask::new(300, 120);
    let r = text::render(&mut m, &font, 32.0, "Hello", pos2(10.0, 10.0), true, true);
    assert!(!r.is_empty() && coverage_sum(&m, r) > 100.0);
    // Bold and italic resolve to real faces or fall back to synthesized ones.
    assert!(lib.load(fam, true, true).is_some());
}
