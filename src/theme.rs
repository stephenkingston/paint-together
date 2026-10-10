//! Look and feel of classic (Windows 7/10) Paint: a light "Scenic Ribbon" palette,
//! square Windows-style controls, and the fonts the UI draws with.

use std::sync::Arc;

use eframe::egui::{
    self, Color32, Context, CornerRadius, FontData, FontDefinitions, FontFamily, FontId, Shadow, Stroke, TextStyle,
    ThemePreference, Ui, Visuals, style::ScrollStyle, vec2,
};

#[derive(Clone, Copy)]
pub struct Palette {
    /// Title bar and tab strip.
    pub chrome: Color32,
    /// The ribbon body.
    pub ribbon: Color32,
    pub ribbon_border: Color32,
    pub group_separator: Color32,
    /// Around the canvas.
    pub workspace: Color32,
    pub status: Color32,
    pub status_border: Color32,
    pub card: Color32,
    pub text: Color32,
    pub text_weak: Color32,
    pub hover: Color32,
    pub hover_border: Color32,
    pub pressed: Color32,
    pub pressed_border: Color32,
    pub checked: Color32,
    pub checked_border: Color32,
    pub file_tab: Color32,
    pub file_tab_hover: Color32,
    pub accent: Color32,
    pub contextual: Color32,
    pub canvas_shadow: Color32,
}

const fn hex(rgb: u32) -> Color32 {
    Color32::from_rgb((rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8)
}

pub const CLASSIC: Palette = Palette {
    chrome: hex(0xFFFFFF),
    ribbon: hex(0xF5F6F7),
    ribbon_border: hex(0xDADBDC),
    group_separator: hex(0xE2E3E4),
    workspace: hex(0xC9D3E2),
    status: hex(0xF0F0F0),
    status_border: hex(0xD7D7D7),
    card: hex(0xFFFFFF),
    text: hex(0x1E1E1E),
    text_weak: hex(0x6D6D6D),
    hover: hex(0xE8EFF7),
    hover_border: hex(0xA4CEF9),
    pressed: hex(0xC9E0F7),
    pressed_border: hex(0x62A2E4),
    checked: hex(0xCCE4F7),
    checked_border: hex(0x66A7E8),
    file_tab: hex(0x1979CA),
    file_tab_hover: hex(0x2A8AD4),
    accent: hex(0x0078D7),
    contextual: hex(0xF2C811),
    canvas_shadow: Color32::from_black_alpha(40),
};

impl Palette {
    pub fn of(_ui: &Ui) -> Self {
        CLASSIC
    }

    pub fn of_ctx(_ctx: &Context) -> Self {
        CLASSIC
    }
}

/// Serif faces used for the "A" glyphs in the text icons.
pub fn serif(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("serif".into()))
}

pub fn serif_bold(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("serif-bold".into()))
}

pub fn install(ctx: &Context) {
    install_fonts(ctx);
    // Paint has a single, light look.
    ctx.set_theme(ThemePreference::Light);
    ctx.all_styles_mut(|style| {
        style.visuals = visuals(&CLASSIC);
        style.text_styles = [
            (TextStyle::Small, FontId::proportional(11.0)),
            (TextStyle::Body, FontId::proportional(12.5)),
            (TextStyle::Button, FontId::proportional(12.5)),
            (TextStyle::Heading, FontId::proportional(16.0)),
            (TextStyle::Monospace, FontId::monospace(12.0)),
        ]
        .into();
        let s = &mut style.spacing;
        s.item_spacing = vec2(6.0, 5.0);
        s.button_padding = vec2(8.0, 3.0);
        s.interact_size = vec2(36.0, 22.0);
        s.menu_margin = 6.into();
        s.window_margin = 14.into();
        s.combo_height = 320.0;
        s.slider_rail_height = 3.0;
        s.scroll = ScrollStyle::solid();
        s.scroll.bar_width = 12.0;
    });
}

fn install_fonts(ctx: &Context) {
    let mut fonts = FontDefinitions::default();
    let faces = [
        ("system", "/System/Library/Fonts/SFNS.ttf", None),
        ("serif", "/System/Library/Fonts/Supplemental/Times New Roman.ttf", Some("serif")),
        ("serif-bold", "/System/Library/Fonts/Supplemental/Times New Roman Bold.ttf", Some("serif-bold")),
    ];
    for (name, path, family) in faces {
        let Ok(bytes) = std::fs::read(path) else { continue };
        fonts.font_data.insert(name.to_owned(), Arc::new(FontData::from_owned(bytes)));
        match family {
            None => fonts.families.entry(FontFamily::Proportional).or_default().insert(0, name.to_owned()),
            Some(f) => {
                let mut list = vec![name.to_owned()];
                list.extend(fonts.families.get(&FontFamily::Proportional).cloned().unwrap_or_default());
                fonts.families.insert(FontFamily::Name(f.into()), list);
            }
        }
    }
    // Make sure the named families exist even without the font files.
    let fallback = fonts.families.get(&FontFamily::Proportional).cloned().unwrap_or_default();
    for f in ["serif", "serif-bold"] {
        fonts.families.entry(FontFamily::Name(f.into())).or_insert_with(|| fallback.clone());
    }
    ctx.set_fonts(fonts);
}

fn visuals(p: &Palette) -> Visuals {
    let mut v = Visuals::light();
    let square = CornerRadius::ZERO;
    v.panel_fill = p.ribbon;
    v.window_fill = p.card;
    v.window_stroke = Stroke::new(1.0, hex(0xA0A0A0));
    v.window_corner_radius = square;
    v.menu_corner_radius = square;
    v.window_shadow = Shadow { offset: [0, 4], blur: 16, spread: 0, color: Color32::from_black_alpha(50) };
    v.popup_shadow = Shadow { offset: [2, 3], blur: 6, spread: 0, color: Color32::from_black_alpha(45) };
    v.extreme_bg_color = Color32::WHITE;
    v.faint_bg_color = p.hover;
    v.hyperlink_color = p.accent;
    v.selection.bg_fill = hex(0xCCE8FF);
    v.selection.stroke = Stroke::new(1.0, p.accent);
    v.text_cursor.stroke = Stroke::new(1.0, p.text);
    v.slider_trailing_fill = false;
    v.override_text_color = Some(p.text);

    let w = &mut v.widgets;
    w.noninteractive.bg_fill = p.ribbon;
    w.noninteractive.weak_bg_fill = p.ribbon;
    w.noninteractive.bg_stroke = Stroke::new(1.0, p.ribbon_border);
    w.noninteractive.fg_stroke = Stroke::new(1.0, p.text);
    w.noninteractive.corner_radius = square;

    w.inactive.bg_fill = Color32::WHITE;
    w.inactive.weak_bg_fill = hex(0xFDFDFD);
    w.inactive.bg_stroke = Stroke::new(1.0, hex(0xABADB3));
    w.inactive.fg_stroke = Stroke::new(1.0, p.text);
    w.inactive.corner_radius = square;

    w.hovered.bg_fill = hex(0xE5F1FB);
    w.hovered.weak_bg_fill = hex(0xE5F1FB);
    w.hovered.bg_stroke = Stroke::new(1.0, hex(0x7EB4EA));
    w.hovered.fg_stroke = Stroke::new(1.0, p.text);
    w.hovered.corner_radius = square;
    w.hovered.expansion = 0.0;

    w.active.bg_fill = hex(0xCCE4F7);
    w.active.weak_bg_fill = hex(0xCCE4F7);
    w.active.bg_stroke = Stroke::new(1.0, hex(0x569DE5));
    w.active.fg_stroke = Stroke::new(1.0, p.text);
    w.active.corner_radius = square;
    w.active.expansion = 0.0;

    w.open.bg_fill = hex(0xCCE4F7);
    w.open.weak_bg_fill = hex(0xCCE4F7);
    w.open.bg_stroke = Stroke::new(1.0, hex(0x569DE5));
    w.open.fg_stroke = Stroke::new(1.0, p.text);
    w.open.corner_radius = square;
    v
}

/// A Windows push button; `default` gives it the blue default-button border.
pub fn dialog_button(text: &str, default: bool) -> egui::Button<'static> {
    let border = if default { CLASSIC.accent } else { hex(0xADADAD) };
    egui::Button::new(text.to_owned())
        .fill(hex(0xE1E1E1))
        .stroke(Stroke::new(if default { 1.5 } else { 1.0 }, border))
        .corner_radius(0)
        .min_size(vec2(80.0, 24.0))
}
