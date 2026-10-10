//! Controls of the Windows "Scenic Ribbon" that classic Paint is built from:
//! large and small buttons, split buttons, groups, tabs, menu rows and swatches.

use eframe::egui::{Align2, Color32, CornerRadius, FontId, Mesh, Painter, Pos2, Rect, Response, Sense, Stroke, StrokeKind, Ui, Vec2, pos2, vec2};

use crate::icons::{self, Icon};
use crate::theme::CLASSIC as P;

/// Height of a group's content area (one large button, or three small ones).
pub const CONTENT_H: f32 = 66.0;
const SMALL_H: f32 = 22.0;
const LABEL: f32 = 12.0;
const DISABLED_TEXT: Color32 = Color32::from_rgb(0xA0, 0xA0, 0xA0);
const CHECKED_HOVER: Color32 = Color32::from_rgb(0xB9, 0xD9, 0xF6);

/// Corner radius of hover, pressed and checked highlights.
pub const HIGHLIGHT_RADIUS: f32 = 3.0;
const TOP_HALF: CornerRadius = CornerRadius { nw: 3, ne: 3, sw: 0, se: 0 };
const BOTTOM_HALF: CornerRadius = CornerRadius { nw: 0, ne: 0, sw: 3, se: 3 };

/// Fill and border for a control in the given state (nothing when idle).
fn frame(painter: &Painter, rect: Rect, hovered: bool, pressed: bool, checked: bool) {
    let (fill, border) = if pressed {
        (P.pressed, P.pressed_border)
    } else if checked && hovered {
        (CHECKED_HOVER, P.pressed_border)
    } else if checked {
        (P.checked, P.checked_border)
    } else if hovered {
        (P.hover, P.hover_border)
    } else {
        return;
    };
    painter.rect_filled(rect, HIGHLIGHT_RADIUS, fill);
    painter.rect_stroke(rect, HIGHLIGHT_RADIUS, Stroke::new(1.0, border), StrokeKind::Inside);
}

fn state(resp: &Response, enabled: bool) -> (bool, bool) {
    (enabled && resp.hovered(), enabled && resp.is_pointer_button_down_on())
}

fn text_color(enabled: bool) -> Color32 {
    if enabled { P.text } else { DISABLED_TEXT }
}

fn sense(enabled: bool) -> Sense {
    if enabled { Sense::click() } else { Sense::hover() }
}

/// Width needed for a big button's label, with room for a ▾.
fn label_width(ui: &Ui, label: &str, dropdown: bool) -> f32 {
    let font = FontId::proportional(LABEL);
    let lines: Vec<&str> = label.split('\n').collect();
    let mut w: f32 = 0.0;
    for (i, line) in lines.iter().enumerate() {
        let mut lw = ui.painter().layout_no_wrap((*line).to_owned(), font.clone(), P.text).size().x;
        if dropdown && i == lines.len() - 1 {
            lw += 11.0;
        }
        w = w.max(lw);
    }
    (w + 10.0).max(44.0)
}

/// Lay out a big button's label lines from `y`, ending with ▾ for dropdowns.
fn draw_label(painter: &Painter, center_x: f32, mut y: f32, label: &str, dropdown: bool, enabled: bool) {
    let font = FontId::proportional(LABEL);
    let color = text_color(enabled);
    let mut lines: Vec<&str> = label.split('\n').collect();
    if dropdown && lines.len() == 1 {
        lines.push("");
    }
    let n = lines.len();
    for (i, line) in lines.into_iter().enumerate() {
        let galley = painter.layout_no_wrap(line.to_owned(), font.clone(), color);
        let arrow = dropdown && i == n - 1;
        let gap = if arrow && galley.size().x > 0.0 { 2.0 } else { 0.0 };
        let total = galley.size().x + gap + if arrow { 9.0 } else { 0.0 };
        let x0 = center_x - total / 2.0;
        let text_w = galley.size().x;
        painter.galley(pos2(x0, y), galley, color);
        if arrow {
            icons::paint(Icon::Dropdown, painter, icons::square(pos2(x0 + text_w + gap + 4.5, y + 7.5), 12.0), enabled);
        }
        y += 13.0;
    }
}

/// A large button: 32px icon over a label (use '\n' for two lines).
pub fn big_button(ui: &mut Ui, icon: Icon, label: &str, dropdown: bool, checked: bool, enabled: bool) -> Response {
    big_button_with(ui, label, dropdown, checked, enabled, |painter, rect, enabled| icons::paint(icon, painter, rect, enabled))
}

/// A large button whose 32px picture is drawn by `draw`.
pub fn big_button_with(
    ui: &mut Ui,
    label: &str,
    dropdown: bool,
    checked: bool,
    enabled: bool,
    draw: impl FnOnce(&Painter, Rect, bool),
) -> Response {
    let w = label_width(ui, label, dropdown);
    let (rect, resp) = ui.allocate_exact_size(vec2(w, CONTENT_H), sense(enabled));
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        let (hovered, pressed) = state(&resp, enabled);
        frame(painter, rect, hovered, pressed, checked);
        draw(painter, icons::square(pos2(rect.center().x, rect.min.y + 21.0), 32.0), enabled);
        draw_label(painter, rect.center().x, rect.min.y + 39.0, label, dropdown, enabled);
    }
    resp
}

/// A large split button (like Paste and Select): the icon part acts, the label
/// part opens a menu. Returns (icon part, menu part).
pub fn big_split_button(ui: &mut Ui, icon: Icon, label: &str, checked: bool, enabled: bool) -> (Response, Response) {
    let w = label_width(ui, label, true);
    let (rect, _) = ui.allocate_exact_size(vec2(w, CONTENT_H), Sense::hover());
    let top = Rect::from_min_max(rect.min, pos2(rect.max.x, rect.min.y + 38.0));
    let bottom = Rect::from_min_max(pos2(rect.min.x, top.max.y), rect.max);
    let id = ui.make_persistent_id(("split", label));
    let main = ui.interact(top, id.with("main"), sense(enabled));
    let menu = ui.interact(bottom, id.with("menu"), sense(enabled));
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        let (main_h, main_p) = state(&main, enabled);
        let (menu_h, menu_p) = state(&menu, enabled);
        if checked {
            painter.rect_filled(top, TOP_HALF, if main_h { CHECKED_HOVER } else { P.checked });
        }
        if main_h || menu_h {
            let lighter = Color32::from_rgb(0xF2, 0xF7, 0xFC);
            if !checked {
                painter.rect_filled(top, TOP_HALF, if main_p { P.pressed } else if main_h { P.hover } else { lighter });
            }
            painter.rect_filled(bottom, BOTTOM_HALF, if menu_p { P.pressed } else if menu_h { P.hover } else { lighter });
            painter.hline(rect.x_range(), top.max.y, Stroke::new(1.0, P.hover_border));
        }
        if main_h || menu_h || checked {
            let border = if checked { P.checked_border } else { P.hover_border };
            painter.rect_stroke(rect, HIGHLIGHT_RADIUS, Stroke::new(1.0, border), StrokeKind::Inside);
        }
        icons::paint(icon, painter, icons::square(top.center() + vec2(0.0, 1.0), 32.0), enabled);
        draw_label(painter, rect.center().x, bottom.min.y + 1.0, label, true, enabled);
    }
    (main, menu)
}

/// A small button: 16px icon, label, optional ▾.
pub fn small_button(ui: &mut Ui, icon: Option<Icon>, label: &str, dropdown: bool, checked: bool, enabled: bool) -> Response {
    let color = text_color(enabled);
    let galley = ui.painter().layout_no_wrap(label.to_owned(), FontId::proportional(LABEL), color);
    let icon_w = if icon.is_some() { 20.0 } else { 0.0 };
    let w = 4.0 + icon_w + galley.size().x + if dropdown { 14.0 } else { 6.0 };
    let (rect, resp) = ui.allocate_exact_size(vec2(w, SMALL_H), sense(enabled));
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        let (hovered, pressed) = state(&resp, enabled);
        frame(painter, rect, hovered, pressed, checked);
        if let Some(icon) = icon {
            icons::paint(icon, painter, icons::square(rect.left_center() + vec2(12.0, 0.0), 16.0), enabled);
        }
        let text_x = rect.min.x + 4.0 + icon_w;
        let text_w = galley.size().x;
        painter.galley(pos2(text_x, rect.center().y - galley.size().y / 2.0), galley, color);
        if dropdown {
            icons::paint(Icon::Dropdown, painter, icons::square(pos2(text_x + text_w + 7.0, rect.center().y), 12.0), enabled);
        }
    }
    resp
}

/// A square 16px icon button (tools, text style toggles).
pub fn icon_button(ui: &mut Ui, icon: Icon, checked: bool, enabled: bool, tooltip: &str) -> Response {
    let (rect, resp) = ui.allocate_exact_size(vec2(23.0, SMALL_H), sense(enabled));
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        let (hovered, pressed) = state(&resp, enabled);
        frame(painter, rect, hovered, pressed, checked);
        icons::paint(icon, painter, icons::square(rect.center(), 16.0), enabled);
    }
    resp.on_hover_text(tooltip)
}

/// A ribbon group: content centred in the content area, caption underneath,
/// separator line after. `content_h` is the content's height.
pub fn group(ui: &mut Ui, caption: Option<&str>, content_h: f32, add: impl FnOnce(&mut Ui)) {
    // At least as wide as the caption, so it never gets cut off.
    let caption_w =
        caption.map_or(0.0, |c| ui.painter().layout_no_wrap(c.to_owned(), FontId::proportional(LABEL), P.text_weak).size().x + 12.0);
    ui.vertical(|ui| {
        ui.spacing_mut().item_spacing.y = 0.0;
        let row = ui.allocate_ui_with_layout(
            vec2(ui.available_width(), CONTENT_H),
            eframe::egui::Layout::top_down(eframe::egui::Align::Min),
            |ui| {
                ui.set_min_height(CONTENT_H);
                ui.set_min_width(caption_w);
                ui.add_space(((CONTENT_H - content_h) * 0.5).max(0.0));
                ui.horizontal_top(|ui| {
                    ui.spacing_mut().item_spacing.x = 2.0;
                    add(ui);
                });
            },
        );
        let (rect, _) = ui.allocate_exact_size(vec2(row.response.rect.width(), 17.0), Sense::hover());
        if let Some(caption) = caption {
            ui.painter().text(rect.center() + vec2(0.0, 1.0), Align2::CENTER_CENTER, caption, FontId::proportional(LABEL), P.text_weak);
        }
    });
    let (rect, _) = ui.allocate_exact_size(vec2(9.0, CONTENT_H + 17.0), Sense::hover());
    ui.painter().vline(rect.center().x, eframe::egui::Rangef::new(rect.top() + 2.0, rect.bottom() - 2.0), Stroke::new(1.0, P.group_separator));
}

/// A tab on the ribbon's tab strip.
pub fn tab(ui: &mut Ui, label: &str, selected: bool, contextual: bool) -> Response {
    let galley = ui.painter().layout_no_wrap(label.to_owned(), FontId::proportional(12.5), P.text);
    let (rect, resp) = ui.allocate_exact_size(vec2(galley.size().x + 26.0, 24.0), Sense::click());
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        if contextual {
            let tint = Color32::from_rgb(0xFF, 0xF6, 0xD5);
            painter.rect_filled(rect, TAB_CORNERS, if selected { P.ribbon } else { tint });
            // The coloured band follows the rounded top edge exactly.
            let band = Rect::from_min_size(rect.min, vec2(rect.width(), 3.0));
            painter.with_clip_rect(band).rect_filled(rect, TAB_CORNERS, P.contextual);
        } else if selected {
            painter.rect_filled(rect, TAB_CORNERS, P.ribbon);
        } else if resp.hovered() {
            // Full height, so the highlight meets the line under the tabs.
            painter.rect_filled(Rect::from_min_max(rect.min + vec2(0.0, 2.0), rect.max), TAB_CORNERS, P.hover);
        }
        if selected {
            selected_tab_outline(painter, rect);
        }
        let color = if selected || resp.hovered() { P.text } else { Color32::from_rgb(0x44, 0x44, 0x44) };
        painter.galley(rect.center() - galley.size() / 2.0 + vec2(0.0, 1.0), galley, color);
    }
    resp
}

/// Tabs are rounded at the top only.
const TAB_CORNERS: CornerRadius = CornerRadius { nw: 5, ne: 5, sw: 0, se: 0 };

/// Radius of the outward curves where the selected tab meets the ribbon.
pub const TAB_FLARE: f32 = 5.0;

/// The selected tab's border: rounded top corners, open at the bottom, flaring
/// outwards into the line along the ribbon's top edge (like browser tabs).
fn selected_tab_outline(painter: &Painter, rect: Rect) {
    use std::f32::consts::FRAC_PI_2;
    let (r, top_r) = (TAB_FLARE, TAB_CORNERS.nw as f32);
    // Outline on pixel centres so the straight parts are crisp.
    let (x0, x1, y0, base) = (rect.min.x + 0.5, rect.max.x - 0.5, rect.min.y + 0.5, rect.max.y - 0.5);
    let arc = |c: Pos2, rad: f32, from: f32, to: f32| -> Vec<Pos2> {
        (0..=8).map(|i| c + Vec2::angled(from + (to - from) * i as f32 / 8.0) * rad).collect::<Vec<_>>()
    };
    // Fill the two flares (the area between the tab, the baseline and each curve).
    for (corner, centre, from, to) in [
        (pos2(rect.min.x, rect.max.y), pos2(x0 - r, base - r), 0.0, FRAC_PI_2),
        (pos2(rect.max.x, rect.max.y), pos2(x1 + r, base - r), 2.0 * FRAC_PI_2, FRAC_PI_2),
    ] {
        let mut mesh = Mesh::default();
        mesh.colored_vertex(corner, P.ribbon);
        for p in arc(centre, r, from, to) {
            mesh.colored_vertex(p, P.ribbon);
        }
        for i in 1..9u32 {
            mesh.add_triangle(0, i, i + 1);
        }
        painter.add(mesh);
    }
    let mut pts = arc(pos2(x0 - r, base - r), r, FRAC_PI_2, 0.0);
    pts.extend(arc(pos2(x0 + top_r, y0 + top_r), top_r, 2.0 * FRAC_PI_2, 3.0 * FRAC_PI_2));
    pts.extend(arc(pos2(x1 - top_r, y0 + top_r), top_r, 3.0 * FRAC_PI_2, 4.0 * FRAC_PI_2));
    pts.extend(arc(pos2(x1 + r, base - r), r, 2.0 * FRAC_PI_2, FRAC_PI_2));
    painter.add(eframe::egui::Shape::line(pts, Stroke::new(1.0, P.ribbon_border)));
}

/// The blue "File" tab.
pub fn file_tab(ui: &mut Ui) -> Response {
    let galley = ui.painter().layout_no_wrap("File".to_owned(), FontId::proportional(12.5), Color32::WHITE);
    let (rect, resp) = ui.allocate_exact_size(vec2(56.0, 24.0), Sense::click());
    if ui.is_rect_visible(rect) {
        let fill = if resp.hovered() { P.file_tab_hover } else { P.file_tab };
        ui.painter().rect_filled(rect, TAB_CORNERS, fill);
        ui.painter().galley(rect.center() - galley.size() / 2.0 + vec2(0.0, 1.0), galley, Color32::WHITE);
    }
    resp
}

/// A grey section header inside a ribbon menu.
pub fn menu_header(ui: &mut Ui, text: &str) {
    let (rect, _) = ui.allocate_exact_size(vec2(ui.available_width(), 22.0), Sense::hover());
    let painter = ui.painter();
    painter.rect_filled(rect, 0.0, Color32::from_rgb(0xEE, 0xEF, 0xF1));
    painter.hline(rect.x_range(), rect.max.y - 0.5, Stroke::new(1.0, P.ribbon_border));
    painter.text(rect.left_center() + vec2(8.0, 0.0), Align2::LEFT_CENTER, text, FontId::proportional(12.0), P.text);
}

/// A row in a ribbon menu: icon gutter (or check mark), label.
pub fn menu_item(ui: &mut Ui, icon: Option<Icon>, label: &str, checked: bool, enabled: bool) -> Response {
    let (rect, resp) = ui.allocate_exact_size(vec2(ui.available_width().max(160.0), 26.0), sense(enabled));
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        let (hovered, pressed) = state(&resp, enabled);
        frame(painter, rect.shrink(1.0), hovered, pressed, false);
        let gutter = Rect::from_min_size(rect.min + vec2(3.0, 3.0), vec2(20.0, 20.0));
        if checked {
            painter.rect_filled(gutter, HIGHLIGHT_RADIUS, P.checked);
            painter.rect_stroke(gutter, HIGHLIGHT_RADIUS, Stroke::new(1.0, P.checked_border), StrokeKind::Inside);
        }
        match (icon, checked) {
            (Some(icon), _) => icons::paint(icon, painter, icons::square(gutter.center(), 16.0), enabled),
            (None, true) => icons::paint(Icon::Check, painter, icons::square(gutter.center(), 16.0), enabled),
            (None, false) => {}
        }
        painter.text(rect.left_center() + vec2(32.0, 0.0), Align2::LEFT_CENTER, label, FontId::proportional(12.5), text_color(enabled));
    }
    resp
}

/// A large row in the File menu: 32px icon and a label.
pub fn file_menu_item(ui: &mut Ui, icon: Icon, label: &str, arrow: bool, highlighted: bool) -> Response {
    let (rect, resp) = ui.allocate_exact_size(vec2(ui.available_width(), 44.0), Sense::click());
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        frame(painter, rect, resp.hovered() || highlighted, resp.is_pointer_button_down_on(), false);
        icons::paint(icon, painter, icons::square(rect.left_center() + vec2(24.0, 0.0), 32.0), true);
        painter.text(rect.left_center() + vec2(50.0, 0.0), Align2::LEFT_CENTER, label, FontId::proportional(13.0), P.text);
        if arrow {
            let c = rect.right_center() - vec2(12.0, 0.0);
            ui.painter().add(eframe::egui::Shape::convex_polygon(
                vec![c + vec2(-3.0, -5.0), c + vec2(3.0, 0.0), c + vec2(-3.0, 5.0)],
                P.text,
                Stroke::NONE,
            ));
        }
    }
    resp
}

/// A square palette swatch with Paint's grey-and-white double border.
pub fn swatch(ui: &mut Ui, color: Color32, side: f32) -> Response {
    let (rect, resp) = ui.allocate_exact_size(vec2(side, side), Sense::click());
    if ui.is_rect_visible(rect) {
        paint_swatch(ui.painter(), rect.shrink(1.0), color, resp.hovered());
    }
    resp
}

pub fn paint_swatch(painter: &Painter, rect: Rect, color: Color32, hovered: bool) {
    let border = if hovered { P.pressed_border } else { Color32::from_rgb(0xA0, 0xA0, 0xA0) };
    painter.rect_filled(rect, 0.0, Color32::WHITE);
    painter.rect_filled(rect.shrink(2.0), 0.0, color);
    painter.rect_stroke(rect, 0.0, Stroke::new(1.0, border), StrokeKind::Inside);
}

/// An empty custom-colour slot.
pub fn empty_swatch(ui: &mut Ui, side: f32) {
    let (rect, _) = ui.allocate_exact_size(vec2(side, side), Sense::hover());
    let r = rect.shrink(1.0);
    ui.painter().rect_filled(r, 0.0, Color32::WHITE);
    ui.painter().rect_stroke(r, 0.0, Stroke::new(1.0, Color32::from_rgb(0xC8, 0xC8, 0xC8)), StrokeKind::Inside);
}

/// Status bar item: 16px icon and text, followed by a separator.
pub fn status_item(ui: &mut Ui, icon: Icon, text: &str, width: f32) {
    let (rect, _) = ui.allocate_exact_size(vec2(width, 22.0), Sense::hover());
    icons::paint(icon, ui.painter(), icons::square(rect.left_center() + vec2(10.0, 0.0), 16.0), true);
    ui.painter().text(rect.left_center() + vec2(24.0, 0.0), Align2::LEFT_CENTER, text, FontId::proportional(12.0), P.text);
    ui.painter().vline(rect.max.x - 0.5, eframe::egui::Rangef::new(rect.top() + 2.0, rect.bottom() - 2.0), Stroke::new(1.0, P.status_border));
}

/// A status bar item that can be clicked, shown like a link.
pub fn status_link(ui: &mut Ui, icon: Icon, text: &str) -> Response {
    let galley = ui.painter().layout_no_wrap(text.to_owned(), FontId::proportional(12.0), P.accent);
    let (rect, resp) = ui.allocate_exact_size(vec2(galley.size().x + 34.0, 22.0), Sense::click());
    let resp = resp.on_hover_cursor(eframe::egui::CursorIcon::PointingHand);
    if resp.hovered() {
        ui.painter().rect_filled(rect.shrink2(vec2(1.0, 2.0)), HIGHLIGHT_RADIUS, P.hover);
    }
    icons::paint(icon, ui.painter(), icons::square(rect.left_center() + vec2(12.0, 0.0), 16.0), true);
    ui.painter().galley(rect.left_center() + vec2(26.0, -galley.size().y / 2.0), galley, P.accent);
    ui.painter().vline(rect.max.x - 0.5, eframe::egui::Rangef::new(rect.top() + 2.0, rect.bottom() - 2.0), Stroke::new(1.0, P.status_border));
    resp
}

/// The Windows 10 Paint zoom slider: thin track, tick at 100%, rectangular thumb.
pub fn zoom_slider(ui: &mut Ui, t: &mut f32, tick: f32, width: f32) -> Response {
    let (rect, mut resp) = ui.allocate_exact_size(vec2(width, 22.0), Sense::click_and_drag());
    let (x0, x1) = (rect.min.x + 4.0, rect.max.x - 4.0);
    if let Some(pos) = resp.interact_pointer_pos() {
        let nt = ((pos.x - x0) / (x1 - x0)).clamp(0.0, 1.0);
        // Snap to 100%.
        let nt = if (nt - tick).abs() < 0.015 { tick } else { nt };
        if nt != *t {
            *t = nt;
            resp.mark_changed();
        }
    }
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        let y = rect.center().y;
        painter.hline(eframe::egui::Rangef::new(x0, x1), y, Stroke::new(1.0, Color32::from_rgb(0x9A, 0x9A, 0x9A)));
        let tx = x0 + (x1 - x0) * tick;
        painter.vline(tx, eframe::egui::Rangef::new(y - 4.0, y + 4.0), Stroke::new(1.0, Color32::from_rgb(0x9A, 0x9A, 0x9A)));
        let x = x0 + (x1 - x0) * *t;
        let thumb = Rect::from_center_size(pos2(x, y), vec2(6.0, 14.0));
        let fill = if resp.hovered() || resp.dragged() { P.accent } else { Color32::from_rgb(0x60, 0x60, 0x60) };
        painter.rect_filled(thumb, 0.0, fill);
    }
    resp
}

/// A small square status-bar button (zoom in / out).
pub fn status_button(ui: &mut Ui, minus: bool) -> Response {
    let (rect, resp) = ui.allocate_exact_size(vec2(18.0, 18.0), Sense::click());
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        frame(painter, rect, resp.hovered(), resp.is_pointer_button_down_on(), false);
        let c = rect.center();
        let s = Stroke::new(1.5, Color32::from_rgb(0x40, 0x40, 0x40));
        painter.line_segment([c - vec2(4.5, 0.0), c + vec2(4.5, 0.0)], s);
        if !minus {
            painter.line_segment([c - vec2(0.0, 4.5), c + vec2(0.0, 4.5)], s);
        }
    }
    resp
}

/// A Windows-style slider with its value: thin grey track, blue fill and thumb.
pub fn value_slider(ui: &mut Ui, value: &mut f32, min: f32, max: f32, log: bool, suffix: &str) -> Response {
    let (to_t, from_t): (Box<dyn Fn(f32) -> f32>, Box<dyn Fn(f32) -> f32>) = if log {
        let (lo, hi) = (min.ln(), max.ln());
        (Box::new(move |v: f32| (v.ln() - lo) / (hi - lo)), Box::new(move |t: f32| (lo + t * (hi - lo)).exp()))
    } else {
        (Box::new(move |v: f32| (v - min) / (max - min)), Box::new(move |t: f32| min + t * (max - min)))
    };
    let width = ui.available_width();
    let (rect, mut resp) = ui.allocate_exact_size(vec2(width, 26.0), Sense::click_and_drag());
    let track = Rect::from_min_max(rect.min + vec2(8.0, 0.0), pos2(rect.max.x - 56.0, rect.max.y));
    if let Some(pos) = resp.interact_pointer_pos() {
        let t = ((pos.x - track.min.x) / track.width()).clamp(0.0, 1.0);
        let v = from_t(t).round().clamp(min, max);
        if v != *value {
            *value = v;
            resp.mark_changed();
        }
    }
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        let y = track.center().y;
        let x = track.min.x + track.width() * to_t(*value).clamp(0.0, 1.0);
        painter.hline(track.x_range(), y, Stroke::new(2.0, Color32::from_rgb(0xC4, 0xC4, 0xC4)));
        painter.hline(eframe::egui::Rangef::new(track.min.x, x), y, Stroke::new(2.0, P.accent));
        let thumb = Rect::from_center_size(pos2(x, y), vec2(8.0, 18.0));
        let fill = if resp.hovered() || resp.dragged() { Color32::from_rgb(0x17, 0x17, 0x17) } else { P.accent };
        painter.rect_filled(thumb, 4.0, fill);
        painter.text(pos2(rect.max.x - 6.0, y), Align2::RIGHT_CENTER, format!("{:.0}{suffix}", *value), FontId::proportional(12.0), P.text);
    }
    resp
}
