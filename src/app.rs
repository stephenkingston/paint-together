//! The Paint window: ribbon, canvas, status bar and all tool behaviour.

use std::path::PathBuf;

use eframe::egui::scroll_area::{DragScroll, ScrollSource};
use eframe::egui::{
    self, Align, Align2, Color32, ColorImage, Context, CursorIcon, Event, FontId, Id, Key, KeyboardShortcut, Layout,
    Modifiers, PointerButton, PopupCloseBehavior, Pos2, Rect, Response, RichText, ScrollArea, Sense, Shape, Stroke,
    StrokeKind, TextureHandle, TextureOptions, Ui, Vec2, ViewportCommand, pos2, vec2,
};
use image::{DynamicImage, Rgba, RgbaImage, imageops};

use crate::brush::{self, BrushKind};
use crate::collab::discovery::Discovery;
use crate::collab::hub::Hub;
use crate::collab::net::{self, Link, LinkEvent};
use crate::collab::patch::{self as collab_patch, Edit, Patch, Region};
use crate::collab::protocol::{FromHub, Held, Kid, PeerId, Presence, ToHub};
use crate::collab::{Received, Session, names};
use crate::history::{Change, History};
use crate::icons::{self, FileKind, Icon};
use crate::live_shape::{self, LiveShape};
use crate::ribbon::{self, CONTENT_H};
use crate::raster::{self, IRect, Mask, Rgba8};
use crate::shapes::{self, ShapeKind};
use crate::text::{self, FontLibrary, TextStyle};
use crate::text_edit::TextCursor;
use crate::theme::{self, Palette};

/// Gap between the canvas and the edge of the scrollable area, in screen points.
const MARGIN: f32 = 8.0;
const RULER: f32 = 18.0;
const DEFAULT_SIZE: (u32, u32) = (1152, 648);
const WHITE: Rgba8 = [255, 255, 255, 255];
const BLACK: Rgba8 = [0, 0, 0, 255];
const ZOOM_STEPS: [f32; 16] =
    [0.125, 0.25, 0.33, 0.5, 0.66, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0];
const TEXT_SIZES: [f32; 16] = [8.0, 9.0, 10.0, 11.0, 12.0, 14.0, 16.0, 18.0, 20.0, 22.0, 24.0, 26.0, 28.0, 36.0, 48.0, 72.0];

/// Paint's two fixed palette rows, with the names Paint shows.
const PALETTE: [([u8; 3], &str); 20] = [
    ([0, 0, 0], "Black"),
    ([127, 127, 127], "Gray-50%"),
    ([136, 0, 21], "Dark red"),
    ([237, 28, 36], "Red"),
    ([255, 127, 39], "Orange"),
    ([255, 242, 0], "Yellow"),
    ([34, 177, 76], "Green"),
    ([0, 162, 232], "Turquoise"),
    ([63, 72, 204], "Indigo"),
    ([163, 73, 164], "Purple"),
    ([255, 255, 255], "White"),
    ([195, 195, 195], "Gray-25%"),
    ([185, 122, 87], "Brown"),
    ([255, 174, 201], "Rose"),
    ([255, 201, 14], "Gold"),
    ([239, 228, 176], "Light yellow"),
    ([181, 230, 29], "Lime"),
    ([153, 217, 234], "Light turquoise"),
    ([112, 146, 190], "Blue-gray"),
    ([200, 191, 231], "Lavender"),
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Tab {
    Home,
    View,
    Together,
    Text,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Tool {
    Select,
    FreeSelect,
    Pencil,
    Fill,
    Text,
    Eraser,
    Picker,
    Magnifier,
    Brush,
    Shape,
}

impl Tool {
    fn is_selection(self) -> bool {
        matches!(self, Tool::Select | Tool::FreeSelect)
    }
}

/// Selected pixels that have been picked up and can be moved around.
struct Floating {
    pixels: RgbaImage,
    /// The canvas underneath the selection (the lifted area filled with Color 2).
    base: RgbaImage,
    /// The canvas before the pixels were lifted, for undo.
    before: RgbaImage,
    /// Where the pixels were lifted from; empty for pasted images.
    origin: IRect,
    /// Where they were lifted from (holding the pixels as they were then) and the
    /// Color 2 left behind there; none for pastes.
    hole: Option<(Patch, Rgba8)>,
    /// Pasting enlarged the canvas.
    canvas_changed: bool,
}

struct Selection {
    rect: IRect,
    /// Free-form outline, relative to `rect`'s top-left.
    lasso: Option<Vec<Pos2>>,
    float: Option<Floating>,
}

struct TextBox {
    pos: Pos2,
    text: String,
    snapshot: RgbaImage,
    mask: Mask,
    dirty: IRect,
    bounds: Rect,
    rendered: Option<(String, TextStyle, Rgba8, Rgba8, Pos2)>,
    /// Typing goes into the box (otherwise the text is just placed, still movable).
    editing: bool,
    cursor: TextCursor,
    /// Caret positions for the text as last rendered.
    layout: text::CaretLayout,
    /// An input method's uncommitted text, shown at the caret.
    preedit: String,
    /// When the caret last moved, so it shows solid while typing.
    blink: f64,
}

#[derive(Clone, Copy)]
enum Pen {
    Pencil,
    Eraser,
    Brush(BrushKind),
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Handle {
    Right,
    Bottom,
    Corner,
}

/// A mouse interaction in progress on the canvas.
enum Op {
    Idle,
    Stroke {
        button: PointerButton,
        pen: Pen,
        snapshot: RgbaImage,
        mask: Mask,
        color: Rgba8,
        opacity: f32,
        size: f32,
        last: Pos2,
        dirty: IRect,
    },
    /// Dragging out a new shape.
    ShapeCreate {
        button: PointerButton,
    },
    /// Dragging one of the live shape's handles; `start` is its geometry when grabbed.
    ShapeHandle {
        button: PointerButton,
        handle: usize,
        start: (Pos2, Pos2),
    },
    ShapeMove {
        button: PointerButton,
        grab: Pos2,
        start: (Pos2, Pos2),
    },
    /// Dragging the text box; a click without a drag reopens the editor.
    TextMove {
        button: PointerButton,
        grab: Pos2,
        start: Pos2,
        moved: bool,
    },
    /// Dragging a corner of the text box scales the text.
    TextScale {
        button: PointerButton,
        handle: usize,
        start_size: f32,
        start_bounds: Rect,
    },
    Selecting {
        button: PointerButton,
        start: Pos2,
        end: Pos2,
        lasso: Vec<Pos2>,
    },
    MovingSel {
        button: PointerButton,
        grab: Pos2,
        origin: (i32, i32),
    },
    ResizeCanvas {
        button: PointerButton,
        handle: Handle,
        size: (u32, u32),
    },
    Pan {
        last: Pos2,
    },
}

impl Op {
    fn button(&self) -> PointerButton {
        match self {
            Op::Stroke { button, .. }
            | Op::ShapeCreate { button }
            | Op::ShapeHandle { button, .. }
            | Op::ShapeMove { button, .. }
            | Op::TextMove { button, .. }
            | Op::TextScale { button, .. }
            | Op::Selecting { button, .. }
            | Op::MovingSel { button, .. }
            | Op::ResizeCanvas { button, .. } => *button,
            Op::Pan { .. } => PointerButton::Middle,
            Op::Idle => PointerButton::Primary,
        }
    }
}

/// An action waiting on the "save changes?" prompt.
#[derive(Clone)]
enum Pending {
    New,
    Open(Option<PathBuf>),
    Quit,
    /// Join the drawing at this address.
    Join(String),
}

/// A short notice at the bottom of the picture.
struct Toast {
    text: String,
    /// When it disappears.
    until: f64,
    /// For "… joined" / "… left": which, and how many friends it's about, so
    /// a burst of arrivals becomes one notice.
    group: Option<(&'static str, usize)>,
}

/// Moving the room to a new host after the host left.
struct Handover {
    /// Whom to try, in order: the old host (in case only my connection dropped),
    /// then everyone else, earliest to join first. Whoever reaches themselves hosts.
    candidates: Vec<Kid>,
    /// The listener kept ready for hosting, while no attempt holds it.
    spare: Option<std::net::TcpListener>,
}

struct ResizeDialog {
    percent: bool,
    w: f32,
    h: f32,
    keep_aspect: bool,
    base: (u32, u32),
}

pub struct PaintApp {
    ctx: Context,
    img: RgbaImage,
    path: Option<PathBuf>,
    history: History,
    saved_state: u64,
    /// Drawing together with friends.
    session: Option<Session>,
    /// `session.version` when last saved.
    saved_version: u64,
    /// The connection to the room (hosting or joined).
    link: Option<Link>,
    /// When hosting: the address friends can join at.
    invite: Option<std::net::SocketAddr>,
    /// My name and colour when drawing together.
    me_name: String,
    me_color: [u8; 3],
    /// Drawings I start: friends wait for me to let them in (else they come straight in).
    ask_first: bool,
    /// Joining a drawing whose host lets friends in: the host's name, while I wait.
    asking: Option<String>,
    /// The Join dialog, with the address typed so far.
    join_dialog: Option<String>,
    /// New/Open while drawing together: asks first, since it changes everyone's picture.
    confirm_everyone: Option<Pending>,
    /// Where the picture on screen changed since my preview was last sent.
    preview_dirty: IRect,
    /// When my preview and pointer were last sent.
    last_share: f64,
    /// The mouse pointer over the picture, in picture pixels (for friends to see).
    pointer: Option<Pos2>,
    /// Finding drawings nearby (started the first time the Together tab opens).
    discovery: Option<Discovery>,
    /// Short notices ("Blue Fox joined"), with when each disappears.
    toasts: Vec<Toast>,
    /// The host left: finding (or becoming) the new one.
    handover: Option<Handover>,
    /// Advertise and look for drawings with mDNS (off in tests).
    lan_discovery: bool,
    /// For my whole-picture edits (by undo step): how many friends' edits had
    /// arrived when I made it. It can only be undone while that's still the count.
    replace_marks: std::collections::HashMap<u64, u64>,

    tool: Tool,
    prev_tool: Tool,
    brush: BrushKind,
    shape: ShapeKind,
    shape_outline: bool,
    shape_fill: bool,
    size_pencil: f32,
    size_brush: f32,
    size_eraser: f32,
    size_shape: f32,
    fill_tolerance: f32,
    primary: Rgba8,
    secondary: Rgba8,
    editing_secondary: bool,
    custom_colors: [Option<Rgba8>; 10],
    transparent_selection: bool,

    op: Op,
    sel: Option<Selection>,
    text: Option<TextBox>,
    /// The shape just drawn, still movable and resizable.
    live_shape: Option<LiveShape>,
    text_style: TextStyle,
    fonts: FontLibrary,

    zoom: f32,
    pending_scroll: Option<Vec2>,
    scroll_offset: Vec2,
    view_rect: Rect,
    canvas_rect: Rect,
    fit_pending: bool,
    show_grid: bool,
    show_status: bool,

    tex: Option<TextureHandle>,
    tex_dirty: IRect,
    tex_full: bool,
    tex_nearest: bool,
    checker: TextureHandle,
    brush_previews: Vec<TextureHandle>,
    select_mode: Tool,
    tab: Tab,
    ribbon_minimized: bool,
    show_rulers: bool,
    show_thumbnail: bool,
    full_screen: bool,
    recent: Vec<PathBuf>,
    file_menu_open: bool,
    file_menu_formats: bool,
    file_tab_rect: Rect,
    properties: Option<(f32, f32)>,
    about_open: bool,
    /// Edit Colors dialog: the colour being edited, as hue/saturation/luminance.
    edit_colors: Option<[f32; 3]>,
    /// Size on disk and modification time of the open file.
    file_meta: Option<(u64, Option<std::time::SystemTime>)>,

    cursor: Option<(i32, i32)>,
    /// A tool drawn as the mouse pointer this frame, with the pointer's screen position.
    tool_cursor: Option<(Icon, Pos2)>,
    rng: u64,
    title: String,
    paste_key_was_down: bool,
    focused_once: bool,

    resize_dialog: Option<ResizeDialog>,
    confirm: Option<Pending>,
    allow_close: bool,
    error: Option<String>,
}

impl PaintApp {
    pub fn new(cc: &eframe::CreationContext<'_>, initial: Option<PathBuf>, join: Option<String>) -> Self {
        if let Some(render) = &cc.wgpu_render_state {
            let a = render.adapter.get_info();
            crate::crash::note_graphics(format!("{} ({:?}, {:?}, driver {} {})", a.name, a.backend, a.device_type, a.driver, a.driver_info));
        }
        let mut app = Self::with_context(cc.egui_ctx.clone(), initial);
        app.lan_discovery = true;
        match load_me() {
            Some((name, color, ask_first)) => (app.me_name, app.me_color, app.ask_first) = (name, color, ask_first),
            None => {
                (app.me_name, app.me_color) = names::random();
                app.save_me();
            }
        }
        if let Some(addr) = join {
            app.tab = Tab::Together;
            app.join_together(addr);
        }
        app
    }

    fn with_context(ctx: Context, initial: Option<PathBuf>) -> Self {
        theme::install(&ctx);
        // Ctrl/⌘ + and − zoom the picture; left on, egui would also grow the whole window's UI.
        ctx.options_mut(|o| o.zoom_with_keyboard = false);
        let (light, dark) = (Color32::from_gray(255), Color32::from_gray(204));
        let checker = ctx.load_texture(
            "checker",
            ColorImage::new([2, 2], vec![light, dark, dark, light]),
            TextureOptions { wrap_mode: egui::TextureWrapMode::Repeat, ..TextureOptions::NEAREST },
        );
        let fonts = FontLibrary::scan();
        let family = fonts.default_family();
        let mut app = Self {
            ctx: ctx.clone(),
            img: RgbaImage::from_pixel(DEFAULT_SIZE.0, DEFAULT_SIZE.1, Rgba(WHITE)),
            path: None,
            history: History::default(),
            saved_state: 0,
            session: None,
            saved_version: 0,
            link: None,
            invite: None,
            me_name: String::new(),
            me_color: names::COLORS[0],
            ask_first: false,
            asking: None,
            join_dialog: None,
            confirm_everyone: None,
            preview_dirty: IRect::EMPTY,
            last_share: f64::NEG_INFINITY,
            pointer: None,
            discovery: None,
            toasts: Vec::new(),
            handover: None,
            lan_discovery: false,
            replace_marks: Default::default(),
            tool: Tool::Pencil,
            prev_tool: Tool::Pencil,
            brush: BrushKind::Brush,
            shape: ShapeKind::Rectangle,
            shape_outline: true,
            shape_fill: false,
            size_pencil: 1.0,
            size_brush: 6.0,
            size_eraser: 12.0,
            size_shape: 3.0,
            fill_tolerance: 10.0,
            primary: BLACK,
            secondary: WHITE,
            editing_secondary: false,
            custom_colors: [None; 10],
            transparent_selection: false,
            op: Op::Idle,
            sel: None,
            text: None,
            live_shape: None,
            text_style: TextStyle { family, size_pt: 14.0, bold: false, italic: false, underline: false, strike: false, opaque: false },
            fonts,
            zoom: 1.0,
            pending_scroll: None,
            scroll_offset: Vec2::ZERO,
            view_rect: Rect::NOTHING,
            canvas_rect: Rect::NOTHING,
            fit_pending: false,
            show_grid: false,
            show_status: true,
            tex: None,
            tex_dirty: IRect::EMPTY,
            tex_full: true,
            tex_nearest: true,
            checker,
            brush_previews: BrushKind::ALL
                .iter()
                .map(|k| ctx.load_texture(format!("brush-{k:?}"), brush_preview(*k), TextureOptions::LINEAR))
                .collect(),
            select_mode: Tool::Select,
            tab: Tab::Home,
            ribbon_minimized: false,
            show_rulers: false,
            show_thumbnail: false,
            full_screen: false,
            recent: load_recent(),
            file_menu_open: false,
            file_menu_formats: false,
            file_tab_rect: Rect::NOTHING,
            properties: None,
            about_open: false,
            edit_colors: None,
            file_meta: None,
            cursor: None,
            tool_cursor: None,
            rng: 0x9E37_79B9_7F4A_7C15,
            title: String::new(),
            paste_key_was_down: false,
            focused_once: false,
            resize_dialog: None,
            confirm: None,
            allow_close: false,
            error: None,
        };
        if let Some(path) = initial {
            app.open_path(path);
        }
        app
    }

    // ---------------------------------------------------------------- document

    fn dims(&self) -> (u32, u32) {
        self.img.dimensions()
    }

    fn mark(&mut self, r: IRect) {
        self.tex_dirty = self.tex_dirty.union(r);
    }

    fn push_patch(&mut self, before: &RgbaImage, r: IRect) {
        if let Some(change) = Change::patch(before, &self.img, r) {
            self.record(change);
        }
    }

    /// A finished edit: one undo step, and (drawing together) sent to everyone.
    fn record(&mut self, change: Change) {
        let Some(s) = &mut self.session else {
            self.history.push(change);
            return;
        };
        let replace = matches!(change, Change::Replace { .. });
        if let Some(edit) = Edit::from_change(&change) {
            s.commit(edit);
        }
        self.history.push(change);
        if replace {
            self.replace_marks.insert(self.history.state_id(), s.foreign);
        }
    }

    fn is_dirty(&self) -> bool {
        let changed = match &self.session {
            Some(s) => s.version != self.saved_version,
            None => self.history.state_id() != self.saved_state,
        };
        changed
            || self.sel.as_ref().is_some_and(|s| s.float.is_some())
            || self.text.as_ref().is_some_and(|t| !t.text.trim().is_empty())
            || self.live_shape.is_some()
    }

    fn doc_name(&self) -> String {
        self.path
            .as_ref()
            .and_then(|p| p.file_name())
            .map_or_else(|| "Untitled".to_owned(), |n| n.to_string_lossy().into_owned())
    }

    fn load_image(&mut self, img: RgbaImage) {
        if self.session.is_some() {
            // Drawing together: the new picture replaces the shared one, for everyone.
            self.settle();
            self.sel = None;
            let before = std::mem::replace(&mut self.img, img);
            self.record(Change::Replace { before, after: self.img.clone() });
            self.tex_full = true;
            self.zoom = 1.0;
            self.fit_pending = true;
            self.pending_scroll = Some(Vec2::ZERO);
            return;
        }
        self.op = Op::Idle;
        self.sel = None;
        self.text = None;
        self.live_shape = None;
        self.img = img;
        self.history.clear();
        self.saved_state = self.history.state_id();
        self.tex_full = true;
        self.zoom = 1.0;
        self.fit_pending = true;
        self.pending_scroll = Some(Vec2::ZERO);
    }

    fn request(&mut self, action: Pending) {
        if self.session.is_some() && matches!(action, Pending::New | Pending::Open(_)) {
            self.confirm_everyone = Some(action);
            return;
        }
        if self.is_dirty() {
            self.confirm = Some(action);
        } else {
            self.perform(action);
        }
    }

    fn perform(&mut self, action: Pending) {
        match action {
            Pending::New => {
                self.load_image(RgbaImage::from_pixel(DEFAULT_SIZE.0, DEFAULT_SIZE.1, Rgba(WHITE)));
                self.path = None;
                self.file_meta = None;
            }
            Pending::Open(Some(path)) => self.open_path(path),
            Pending::Open(None) => self.open_dialog(),
            Pending::Join(addr) => self.join_together(addr),
            Pending::Quit => {
                self.allow_close = true;
                self.ctx.send_viewport_cmd(ViewportCommand::Close);
            }
        }
    }

    fn open_dialog(&mut self) {
        let picked = rfd::FileDialog::new()
            .add_filter("All picture files", &["png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff", "webp", "ico"])
            .pick_file();
        if let Some(path) = picked {
            self.open_path(path);
        }
    }

    fn open_path(&mut self, path: PathBuf) {
        match image::open(&path) {
            Ok(img) => {
                self.load_image(img.to_rgba8());
                self.file_meta = file_meta(&path);
                self.add_recent(&path);
                self.path = Some(path);
            }
            Err(e) => self.error = Some(format!("Paint cannot read {}.\n\n{e}", path.display())),
        }
    }

    fn save(&mut self) -> bool {
        match self.path.clone() {
            Some(path) => self.save_to(path),
            None => self.save_as(),
        }
    }

    fn save_as(&mut self) -> bool {
        let name = self.path.as_ref().and_then(|p| p.file_name()).map_or("Untitled.png".into(), |n| n.to_string_lossy());
        let picked = rfd::FileDialog::new()
            .set_file_name(name)
            .add_filter("PNG", &["png"])
            .add_filter("JPEG", &["jpg", "jpeg"])
            .add_filter("BMP", &["bmp"])
            .add_filter("GIF", &["gif"])
            .add_filter("TIFF", &["tif", "tiff"])
            .add_filter("WebP", &["webp"])
            .save_file();
        let Some(mut path) = picked else { return false };
        if path.extension().is_none() {
            path.set_extension("png");
        }
        self.save_to(path)
    }

    fn save_to(&mut self, path: PathBuf) -> bool {
        self.commit_all();
        let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase();
        // These formats have no alpha channel.
        let result = if matches!(ext.as_str(), "jpg" | "jpeg" | "bmp") {
            DynamicImage::ImageRgba8(self.img.clone()).to_rgb8().save(&path)
        } else {
            self.img.save(&path)
        };
        match result {
            Ok(()) => {
                self.file_meta = file_meta(&path);
                self.add_recent(&path);
                self.path = Some(path);
                self.saved_state = self.history.state_id();
                self.saved_version = self.session.as_ref().map_or(0, |s| s.version);
                true
            }
            Err(e) => {
                self.error = Some(format!("Paint cannot save {}.\n\n{e}", path.display()));
                false
            }
        }
    }

    /// Save via the File menu's "Save as" format list (`None`: all formats).
    fn save_as_format(&mut self, ext: Option<&str>) -> bool {
        let Some(ext) = ext else { return self.save_as() };
        let stem = self.path.as_ref().and_then(|p| p.file_stem()).map_or("Untitled".into(), |s| s.to_string_lossy().into_owned());
        let (name, exts): (&str, &[&str]) = match ext {
            "png" => ("PNG", &["png"]),
            "jpg" => ("JPEG", &["jpg", "jpeg"]),
            "bmp" => ("BMP", &["bmp"]),
            _ => ("GIF", &["gif"]),
        };
        let picked = rfd::FileDialog::new().set_file_name(format!("{stem}.{ext}")).add_filter(name, exts).save_file();
        let Some(mut path) = picked else { return false };
        if path.extension().is_none() {
            path.set_extension(ext);
        }
        self.save_to(path)
    }

    fn add_recent(&mut self, path: &std::path::Path) {
        let path = path.canonicalize().unwrap_or_else(|_| path.to_path_buf());
        self.recent.retain(|p| *p != path);
        self.recent.insert(0, path);
        self.recent.truncate(9);
        save_recent(&self.recent);
    }

    fn paste_from(&mut self) {
        let picked = rfd::FileDialog::new()
            .add_filter("All picture files", &["png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff", "webp", "ico"])
            .pick_file();
        let Some(path) = picked else { return };
        match image::open(&path) {
            Ok(img) => self.paste_image(img.to_rgba8()),
            Err(e) => self.error = Some(format!("Paint cannot read {}.\n\n{e}", path.display())),
        }
    }

    fn file_size(&self) -> Option<u64> {
        self.file_meta.map(|(len, _)| len)
    }

    fn open_properties(&mut self) {
        let (w, h) = self.dims();
        self.properties = Some((w as f32, h as f32));
    }

    fn set_full_screen(&mut self, on: bool) {
        self.full_screen = on;
        self.ctx.send_viewport_cmd(ViewportCommand::Fullscreen(on));
    }

    fn undo(&mut self) {
        if self.session.is_some() {
            return self.step_mine(true);
        }
        self.commit_all();
        if self.history.undo(&mut self.img) {
            self.tex_full = true;
        }
    }

    fn redo(&mut self) {
        if self.session.is_some() {
            return self.step_mine(false);
        }
        self.commit_all();
        if self.history.redo(&mut self.img) {
            self.tex_full = true;
        }
    }

    /// Undo or redo one of my own edits without touching what friends drew since:
    /// a pixel changes back only while it still shows my edit.
    fn step_mine(&mut self, back: bool) {
        self.settle();
        let Some(s) = &mut self.session else { return };
        let id = if back { self.history.state_id() } else { self.history.redo_id() };
        let (img, marks) = (&mut self.img, &mut self.replace_marks);
        let mut too_late = false;
        let mut whole = false;
        let mut region = IRect::EMPTY;
        let stepped = self.history.step_with(back, |change| {
            if matches!(change, Change::Replace { .. }) {
                if marks.get(&id) != Some(&s.foreign) {
                    too_late = true;
                    return false;
                }
                whole = true;
            }
            if let Some(edit) = collab_patch::revert(change, &s.base, back) {
                if let Region::Rect(r) = edit.region(img) {
                    region = r;
                }
                edit.apply(img);
                s.commit(edit);
            }
            true
        });
        if stepped && whole {
            marks.insert(id, s.foreign);
        }
        if too_late {
            self.error = Some("Too late to undo that: your friends have drawn on the picture since.".into());
        }
        if whole {
            self.tex_full = true;
        } else {
            self.mark(region);
        }
    }

    /// Finish whatever is in progress (a stroke, live shape, text, floating selection).
    fn settle(&mut self) {
        self.end();
        self.commit_all();
    }

    // ---------------------------------------------------------------- drawing together

    /// Handle one message from the room's hub.
    fn net_receive(&mut self, msg: FromHub) {
        if let FromHub::Welcome { you, host, seq, picture, kids, ask_first } = msg {
            let Some(img) = picture.to_image() else {
                return;
            };
            self.asking = None;
            if self.handover.take().is_some() {
                if let Some(s) = &mut self.session {
                    s.rejoin(you, host, seq, img, kids);
                    s.ask_first = ask_first;
                    self.img = s.base.clone();
                    self.tex_full = true;
                    let news = match s.kids.iter().find(|k| k.id == host) {
                        _ if host == you => "You're the host now".to_owned(),
                        Some(k) => format!("{} is the host now", k.name),
                        None => "Drawing together again".to_owned(),
                    };
                    self.toast(news);
                    self.advertise();
                    return;
                }
            }
            self.settle();
            self.sel = None;
            self.history.clear();
            self.replace_marks.clear();
            let mut session = Session::new(you, host, seq, img.clone(), kids);
            session.ask_first = ask_first;
            self.session = Some(session);
            self.saved_version = 0;
            self.img = img;
            self.tex_full = true;
            self.fit_pending = true;
            self.pending_scroll = Some(Vec2::ZERO);
            return;
        }
        match msg {
            FromHub::Asking(host) => return self.asking = Some(host),
            // Not let in (or taking over as host didn't work out: try the next kid).
            FromHub::Rejected(_) if self.handover.is_some() => return self.lost_host(),
            FromHub::Rejected(why) => {
                self.leave_together();
                return self.error = Some(why);
            }
            _ => {}
        }
        let Some(s) = &self.session else { return };
        let mut before = s.kids.clone();
        if let FromHub::KickedOut { kid, by, .. } = &msg {
            let name = |id: PeerId| before.iter().find(|k| k.id == id).map_or_else(|| "A friend".to_owned(), |k| k.name.clone());
            if *kid == s.me {
                let host = name(*by);
                self.leave_together();
                self.error = Some(format!("You have been kicked out by {host}.\n\nThe picture stays with you."));
                return;
            }
            let by = if *by == s.me { "you".to_owned() } else { name(*by) };
            self.toast(format!("{} has been kicked out by {by}", name(*kid)));
            // Not "… left" as well.
            before.retain(|k| k.id != *kid);
        }
        let Some(s) = &self.session else { return };
        // Whole-picture changes (and anything arriving while my canvas is a
        // different size, mid-resize) land after I finish what I'm doing.
        let foreign = matches!(&msg, FromHub::Sequenced { from, .. } if *from != s.me);
        if s.replaces_picture(&msg) || (foreign && self.img.dimensions() != s.base.dimensions()) {
            self.settle();
        }
        let Some(s) = &mut self.session else { return };
        match s.receive(msg) {
            Received::Picture(region) => self.rebase(region),
            Received::Kids => {
                self.announce_kids(&before);
                self.advertise();
            }
            Received::Nothing => {}
        }
    }

    fn save_me(&self) {
        save_me(&self.me_name, self.me_color, self.ask_first);
    }

    /// Whether this app runs the room.
    fn hosting(&self) -> bool {
        self.handover.is_none() && self.session.as_ref().is_some_and(|s| s.host == s.me)
    }

    /// Whether friends wait for me to let them in: for drawings I start, and the
    /// one I'm hosting.
    fn set_ask_first(&mut self, on: bool) {
        self.ask_first = on;
        self.save_me();
        if self.hosting()
            && let Some(s) = &mut self.session
        {
            s.ask_first = on;
            s.send(ToHub::AskFirst(on));
        }
    }

    /// Send a friend out of the drawing I'm hosting.
    fn kick(&mut self, kid: PeerId) {
        if self.hosting()
            && let Some(s) = &mut self.session
        {
            s.send(ToHub::Kick(kid));
        }
    }

    /// Answer friends waiting to be let in.
    fn let_in(&mut self, kids: &[PeerId], yes: bool) {
        let Some(s) = &mut self.session else { return };
        for &kid in kids {
            s.send(ToHub::LetIn { kid, yes });
        }
        s.waiting.retain(|k| !kids.contains(&k.id));
    }

    /// Friends waiting to be let in, for the host to answer, or my own wait to be
    /// let in: notices at the bottom of the picture. Returns how much room they take.
    fn door_notices(&mut self, ctx: &Context) -> f32 {
        let waiting = self.session.as_ref().filter(|_| self.hosting()).map(|s| s.waiting.clone()).unwrap_or_default();
        let start = self.view_rect.center_bottom() - vec2(0.0, 18.0);
        let mut bottom = start;
        let mut pill = |id: Id, add: &mut dyn FnMut(&mut Ui)| {
            let shown = egui::Area::new(id).order(egui::Order::Foreground).pivot(Align2::CENTER_BOTTOM).fixed_pos(bottom).show(ctx, add);
            bottom.y -= shown.response.rect.height() + 6.0;
        };
        if let Some(host) = self.asking.clone() {
            let mut stop = false;
            pill(Id::new("asking"), &mut |ui| {
                stop = door_pill(ui, None, &format!("Waiting for {host} to let you in…"), None, "Stop waiting").is_some();
            });
            if stop {
                self.leave_together();
            }
        }
        // A few at a time; a crowd at the door gets one more notice for the rest.
        const SHOWN: usize = 3;
        let mut answers: Vec<(Vec<PeerId>, bool)> = Vec::new();
        for kid in waiting.iter().take(SHOWN) {
            pill(Id::new(("waiting", kid.id)), &mut |ui| {
                let (who, text) = (Some((kid.name.as_str(), kid.color)), format!("{} wants to join", kid.name));
                if let Some(yes) = door_pill(ui, who, &text, Some(&format!("Let {} in", kid.name)), "Not this time") {
                    answers.push((vec![kid.id], yes));
                }
            });
        }
        if waiting.len() > SHOWN {
            let rest: Vec<PeerId> = waiting[SHOWN..].iter().map(|k| k.id).collect();
            pill(Id::new("waiting-more"), &mut |ui| {
                let text = match rest.len() {
                    1 => "1 more wants to join".to_owned(),
                    n => format!("{n} more want to join"),
                };
                if let Some(yes) = door_pill(ui, None, &text, Some("Let them all in"), "Not this time") {
                    answers.push((rest.clone(), yes));
                }
            });
        }
        for (kids, yes) in answers {
            self.let_in(&kids, yes);
        }
        start.y - bottom.y
    }

    /// Start a drawing that friends on the network can join.
    fn start_together(&mut self) {
        self.settle();
        match Link::host(self.img.clone(), self.me_name.clone(), self.me_color, self.ask_first, self.ctx.clone()) {
            Ok(link) => {
                self.invite = link.hosting_at();
                self.link = Some(link);
                self.advertise();
            }
            Err(e) => self.error = Some(format!("Paint couldn't start drawing together.\n\n{e}")),
        }
    }

    /// Join a friend's drawing (their picture replaces mine once connected).
    fn join_together(&mut self, addr: String) {
        self.settle();
        match Link::join(addr, self.me_name.clone(), self.me_color, None, self.ctx.clone()) {
            Ok(link) => {
                self.invite = None;
                self.link = Some(link);
            }
            Err(e) => self.error = Some(format!("Paint couldn't join that drawing.\n\n{e}")),
        }
    }

    /// Stop drawing together; the picture stays, as an ordinary unsaved one.
    fn leave_together(&mut self) {
        self.settle();
        self.flush_network();
        let dirty = self.is_dirty();
        self.link = None;
        self.invite = None;
        self.handover = None;
        self.asking = None;
        if let Some(d) = &mut self.discovery {
            d.stop_advertising();
        }
        if self.session.take().is_some() {
            self.history.clear();
            self.replace_marks.clear();
            self.saved_state = if dirty { u64::MAX } else { self.history.state_id() };
        }
    }

    /// "Blue Fox joined" / "Blue Fox left", comparing with who was here before.
    fn announce_kids(&mut self, before: &[Kid]) {
        let Some(s) = &self.session else { return };
        let joined: Vec<String> =
            s.kids.iter().filter(|k| k.id != s.me && !before.iter().any(|b| b.id == k.id)).map(|k| k.name.clone()).collect();
        let left: Vec<String> =
            before.iter().filter(|b| b.id != s.me && !s.kids.iter().any(|k| k.id == b.id)).map(|k| k.name.clone()).collect();
        self.announce("joined", &joined);
        self.announce("left", &left);
    }

    /// "Blue Fox joined"; or, when several come (or go) close together, one
    /// notice that keeps count: "5 friends joined".
    fn announce(&mut self, verb: &'static str, names: &[String]) {
        if names.is_empty() {
            return;
        }
        let now = self.ctx.input(|i| i.time);
        let showing = self.toasts.iter_mut().find(|t| t.until > now && t.group.is_some_and(|(v, _)| v == verb));
        match showing {
            Some(t) => {
                let count = t.group.map_or(0, |(_, n)| n) + names.len();
                (t.text, t.until, t.group) = (format!("{count} friends {verb}"), now + 3.5, Some((verb, count)));
            }
            None => {
                let text = match names {
                    [one] => format!("{one} {verb}"),
                    _ => format!("{} friends {verb}", names.len()),
                };
                self.push_toast(Toast { text, until: now + 3.5, group: Some((verb, names.len())) });
            }
        }
    }

    fn toast(&mut self, text: String) {
        let until = self.ctx.input(|i| i.time) + 3.5;
        self.push_toast(Toast { text, until, group: None });
    }

    /// At most four notices at once; the oldest goes first.
    fn push_toast(&mut self, toast: Toast) {
        self.toasts.push(toast);
        if self.toasts.len() > 4 {
            self.toasts.remove(0);
        }
    }

    /// Notices at the bottom of the picture area (above `lift` taken by others), fading out.
    fn paint_toasts(&mut self, ctx: &Context, lift: f32) {
        let now = ctx.input(|i| i.time);
        self.toasts.retain(|t| t.until > now);
        if self.toasts.is_empty() {
            return;
        }
        let painter = ctx.layer_painter(egui::LayerId::new(egui::Order::Foreground, Id::new("toasts")));
        let mut bottom = self.view_rect.center_bottom() - vec2(0.0, 18.0 + lift);
        for Toast { text, until, .. } in self.toasts.iter().rev() {
            let fade = ((until - now) / 0.5).clamp(0.0, 1.0) as f32;
            let galley = painter.layout_no_wrap(text.clone(), FontId::proportional(13.0), Color32::WHITE.gamma_multiply(fade));
            let pill = Rect::from_center_size(bottom - vec2(0.0, 14.0), galley.size() + vec2(28.0, 12.0));
            painter.rect_filled(pill, 14.0, Color32::from_rgba_unmultiplied(40, 40, 48, (215.0 * fade) as u8));
            painter.galley(pill.center() - galley.size() / 2.0, galley, Color32::WHITE);
            bottom.y -= pill.height() + 6.0;
        }
        ctx.request_repaint_after(std::time::Duration::from_millis(50));
    }

    /// When hosting, tell the network about the drawing (and how many are in it).
    fn advertise(&mut self) {
        let Some(port) = self.invite.map(|a| a.port()) else {
            return;
        };
        let kids = self.session.as_ref().map_or(1, |s| s.kids.len().max(1));
        if self.discovery.is_none() && self.lan_discovery {
            self.discovery = Discovery::new();
        }
        if let Some(d) = &mut self.discovery {
            d.advertise(&self.me_name, kids, port);
        }
    }

    /// Look for drawings nearby, from now on.
    fn discover(&mut self) {
        if self.discovery.is_none() && self.lan_discovery {
            self.discovery = Discovery::new();
        }
        if let Some(d) = &mut self.discovery {
            d.browse();
        }
    }

    /// Take in everything the network has delivered.
    fn pump_network(&mut self) {
        if let Some(d) = &mut self.discovery {
            d.poll();
            // Keep checking for drawings appearing while nobody touches the mouse.
            if self.session.is_none() {
                self.ctx.request_repaint_after(std::time::Duration::from_millis(700));
            }
        }
        if let Some(s) = &mut self.session {
            s.now = self.ctx.input(|i| i.time);
        }
        while let Some(event) = self.link.as_ref().and_then(|l| l.try_recv()) {
            match event {
                LinkEvent::Message(msg) => self.net_receive(msg),
                LinkEvent::Closed(why) if self.session.is_none() => {
                    // Couldn't join in the first place.
                    self.leave_together();
                    if !why.is_empty() {
                        self.error = Some(why);
                    }
                }
                LinkEvent::Closed(_) => self.lost_host(),
            }
        }
        self.step_handover();
    }

    /// The connection to the room dropped (or a takeover attempt failed).
    fn lost_host(&mut self) {
        let spare = self.link.take().and_then(|mut l| l.take_spare());
        match &mut self.handover {
            Some(h) => {
                h.candidates.remove(0);
                h.spare = spare.or(h.spare.take());
            }
            None => {
                let Some(s) = &self.session else { return };
                let mut candidates: Vec<Kid> = s.kids.clone();
                candidates.sort_by_key(|k| (k.id != s.host, k.id));
                self.handover = Some(Handover { candidates, spare });
                self.toast("The host left: carrying on…".to_owned());
            }
        }
    }

    /// Try the next kid in line for host, or become it.
    fn step_handover(&mut self) {
        if self.link.is_some() {
            return; // waiting to hear back
        }
        let (Some(h), Some(s)) = (&mut self.handover, &self.session) else {
            return;
        };
        let Some(next) = h.candidates.first().cloned() else {
            self.leave_together();
            self.error = Some("The drawing ended: the host left.".into());
            return;
        };
        let (name, color, ctx) = (self.me_name.clone(), self.me_color, self.ctx.clone());
        if next.id == s.me {
            let listener = match h.spare.take() {
                Some(l) => Ok(l),
                None => net::listen(),
            };
            // The room's rules carry on, and everyone who was in it comes straight back.
            let hub = Hub::new(s.confirmed().clone()).with_door(s.ask_first, s.kids.clone());
            match listener.and_then(|l| Link::host_with(l, hub, name, color, ctx)) {
                Ok(link) => {
                    self.invite = link.hosting_at();
                    self.link = Some(link);
                }
                Err(e) => {
                    self.leave_together();
                    self.error = Some(format!("Paint couldn't carry on the drawing.\n\n{e}"));
                }
            }
        } else if next.id == s.host && next.addr.is_empty() {
            h.candidates.remove(0);
        } else {
            match Link::join(next.addr.clone(), name, color, h.spare.take(), ctx) {
                Ok(link) => self.link = Some(link),
                Err(_) => _ = h.candidates.remove(0),
            }
        }
    }

    /// Send what I've drawn since last time.
    fn flush_network(&mut self) {
        if self.handover.is_some() {
            return;
        }
        let out = self.net_outbox();
        if let Some(link) = &self.link {
            for msg in out {
                link.send(msg);
            }
        }
    }

    fn together_tab(&mut self, ui: &mut Ui) {
        self.discover();
        let pal = Palette::of(ui);
        let hosting = self.hosting();
        ribbon::group(ui, Some("Draw together"), CONTENT_H, |ui| {
            if self.link.is_none() {
                let start = ribbon::big_button(ui, Icon::Together, "Start\ndrawing", false, false, true);
                if start.on_hover_text("Let friends on this Wi-Fi draw on this picture with you").clicked() {
                    self.start_together();
                }
                let join = ribbon::big_button(ui, Icon::JoinRoom, "Join a\ndrawing", false, false, true);
                if join.on_hover_text("Draw on a friend's picture").clicked() {
                    self.join_dialog = Some(String::new());
                }
            } else {
                let leave = ribbon::big_button(ui, Icon::LeaveRoom, "Leave", false, false, true);
                if leave.on_hover_text("Stop drawing together (you keep the picture)").clicked() {
                    self.leave_together();
                }
            }
            // Who gets in: chosen before starting, and changeable while hosting.
            if hosting || (self.link.is_none() && self.session.is_none()) {
                ui.vertical(|ui| {
                    ui.add_space(8.0);
                    let mut let_in = !self.session.as_ref().filter(|_| hosting).map_or(self.ask_first, |s| s.ask_first);
                    let tip = "When this is off, you're asked about each friend who wants to join, and can let them in or not";
                    if ui.checkbox(&mut let_in, "Let friends in\nwithout asking").on_hover_text(tip).changed() {
                        self.set_ask_first(!let_in);
                    }
                });
                ui.add_space(6.0);
            }
        });
        ribbon::group(ui, Some("Friends"), CONTENT_H, |ui| {
            let kids: Vec<Kid> = self.session.as_ref().map(|s| s.kids.clone()).unwrap_or_default();
            let me = self.session.as_ref().map(|s| s.me);
            if kids.is_empty() {
                let hint = match (&self.asking, &self.link) {
                    (Some(host), _) => format!("Waiting for {host}\nto let you in…"),
                    (None, Some(_)) => "Connecting…".to_owned(),
                    (None, None) => "Start or join a drawing\nto draw with friends.".to_owned(),
                };
                ui.add_space(6.0);
                ui.label(RichText::new(hint).color(pal.text_weak));
                ui.add_space(6.0);
            }
            // Three columns at most; a big room's full list opens from the "more" button.
            const SHOWN: usize = 9;
            let label = |kid: &Kid| if Some(kid.id) == me { format!("{} (you)", kid.name) } else { kid.name.clone() };
            // The host can send anyone else out.
            fn kick(kid: &Kid, hosting: bool, me: Option<PeerId>) -> Option<&str> {
                (hosting && Some(kid.id) != me).then_some(kid.name.as_str())
            }
            let mut kicked = None;
            for (i, column) in kids[..kids.len().min(SHOWN)].chunks(3).enumerate() {
                if i > 0 {
                    ui.add_space(16.0);
                }
                ui.vertical(|ui| {
                    ui.add_space(2.0);
                    for kid in column {
                        if kid_row(ui, &label(kid), kid.color, pal.text, kick(kid, hosting, me)) {
                            kicked = Some(kid.id);
                        }
                    }
                });
            }
            if kids.len() > SHOWN {
                ui.add_space(8.0);
                let more = more_button(ui, &format!("+{}", kids.len() - SHOWN), &pal)
                    .on_hover_text(format!("Everyone drawing ({})", kids.len()));
                egui::Popup::menu(&more).show(|ui| {
                    ui.add_space(2.0);
                    ui.label(RichText::new(format!("Everyone drawing ({})", kids.len())).strong().color(pal.text));
                    ui.add_space(6.0);
                    // Up to seven names a column and four columns side by side; a bigger
                    // room scrolls.
                    let columns = kids.len().div_ceil(7).clamp(1, 4);
                    ScrollArea::vertical().max_height(7.0 * 27.0).min_scrolled_height(7.0 * 27.0).show(ui, |ui| {
                        ui.horizontal_top(|ui| {
                            for (i, column) in kids.chunks(kids.len().div_ceil(columns)).enumerate() {
                                if i > 0 {
                                    ui.add_space(16.0);
                                }
                                ui.vertical(|ui| {
                                    for kid in column {
                                        if kid_row(ui, &label(kid), kid.color, pal.text, kick(kid, hosting, me)) {
                                            kicked = Some(kid.id);
                                        }
                                    }
                                });
                            }
                        });
                    });
                });
            }
            if let Some(kid) = kicked {
                self.kick(kid);
            }
        });
        if let Some(addr) = self.invite {
            ribbon::group(ui, Some("Invite"), CONTENT_H, |ui| {
                ui.vertical(|ui| {
                    ui.add_space(8.0);
                    ui.label(RichText::new("Friends can join at").color(pal.text_weak));
                    ui.label(RichText::new(addr.to_string()).size(16.0).strong().color(pal.text));
                });
                ui.add_space(6.0);
            });
        }
        ribbon::group(ui, Some("Me"), CONTENT_H, |ui| {
            ui.vertical(|ui| {
                ui.add_space(4.0);
                let editable = self.link.is_none();
                ui.horizontal(|ui| {
                    paint_avatar(ui, &self.me_name, self.me_color, 20.0);
                    let edit = egui::TextEdit::singleline(&mut self.me_name).desired_width(130.0).char_limit(20);
                    let resp = ui.add_enabled(editable, edit).on_hover_text("Your name, shown to friends");
                    if resp.lost_focus() {
                        self.me_name = names::tidy(&self.me_name);
                        self.save_me();
                    }
                });
                ui.add_space(3.0);
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 2.0;
                    for c in names::COLORS {
                        let (rect, resp) = ui.allocate_exact_size(Vec2::splat(13.0), Sense::click());
                        let color = Color32::from_rgb(c[0], c[1], c[2]);
                        let chosen = c == self.me_color;
                        ui.painter().circle_filled(rect.center(), 6.0, color);
                        if chosen {
                            ui.painter().circle_stroke(rect.center(), 6.0, Stroke::new(1.5, pal.text));
                        }
                        if editable && resp.clicked() {
                            self.me_color = c;
                            self.save_me();
                        }
                    }
                });
            });
            ui.add_space(6.0);
        });
    }

    fn together_dialogs(&mut self, ctx: &Context) {
        let pal = Palette::of_ctx(ctx);
        if let Some(mut addr) = self.join_dialog.take() {
            let mut join = false;
            self.discover();
            let nearby = self.discovery.as_ref().map(|d| d.nearby.clone()).unwrap_or_default();
            let mut picked = None;
            let (cancel, dismissed) = dialog(ctx, "join", "Join a drawing", 360.0, |ui| {
                if nearby.is_empty() {
                    ui.label(RichText::new("Looking for drawings on this Wi-Fi…").color(pal.text_weak));
                } else {
                    ui.label(RichText::new("Drawings nearby:").color(pal.text));
                    ui.add_space(2.0);
                    for n in &nearby {
                        if nearby_row(ui, n, &pal).clicked() && n.compatible {
                            picked = Some(n.addr.to_string());
                        }
                    }
                }
                ui.add_space(12.0);
                ui.label(RichText::new("Can't see it? Type the address shown on your friend's Together tab:").color(pal.text_weak));
                ui.add_space(4.0);
                let field = ui.add(egui::TextEdit::singleline(&mut addr).hint_text("192.168.1.20").desired_width(f32::INFINITY));
                join = field.lost_focus() && ui.input(|i| i.key_pressed(Key::Enter));
                if !join && ui.memory(|m| m.focused().is_none()) {
                    field.request_focus();
                }
                ui.add_space(14.0);
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    let cancel = ui.add(theme::dialog_button("Cancel", false)).clicked();
                    join |= ui.add_enabled(!addr.trim().is_empty(), theme::dialog_button("Join", true)).clicked();
                    cancel
                })
                .inner
            });
            if let Some(addr) = picked {
                self.request(Pending::Join(addr));
            } else if join && !addr.trim().is_empty() {
                self.request(Pending::Join(addr.trim().to_owned()));
            } else if !(cancel || dismissed) {
                self.join_dialog = Some(addr);
            }
        }
        if let Some(action) = self.confirm_everyone.take() {
            let mut go = false;
            let (cancel, dismissed) = dialog(ctx, "everyone", "Paint", 380.0, |ui| {
                let what = if matches!(action, Pending::New) { "Start a new picture" } else { "Open a picture" };
                ui.label(RichText::new(format!("{what} for everyone drawing with you?")).color(pal.text));
                ui.add_space(4.0);
                ui.label(RichText::new("It replaces the picture on all your friends' screens too.").color(pal.text_weak));
                ui.add_space(14.0);
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    let cancel = ui.add(theme::dialog_button("Cancel", false)).clicked();
                    go = ui.add(theme::dialog_button("Continue", true)).clicked();
                    cancel
                })
                .inner
            });
            if go {
                self.perform(action);
            } else if !(cancel || dismissed) {
                self.confirm_everyone = Some(action);
            }
        }
    }

    /// What to send the hub: my finished edits, and (at most 30 times a second)
    /// what I'm drawing right now and where my pointer is.
    fn net_outbox(&mut self) -> Vec<ToHub> {
        let now = self.ctx.input(|i| i.time);
        self.net_outbox_at(now)
    }

    fn net_outbox_at(&mut self, now: f64) -> Vec<ToHub> {
        let Some(s) = &mut self.session else {
            return Vec::new();
        };
        let mut out = s.take_outbox();
        if now - self.last_share < 1.0 / 30.0 {
            return out;
        }
        self.last_share = now;
        let changed = if self.tex_full { IRect::of_image(&self.img) } else { self.preview_dirty.union(self.tex_dirty) };
        self.preview_dirty = IRect::EMPTY;
        let tiles = s.preview_tiles(&self.img, changed);
        if !tiles.is_empty() {
            out.push(ToHub::Preview(tiles));
        }
        let tool = match self.tool {
            Tool::Pencil => Held::Pencil,
            Tool::Fill => Held::Fill,
            Tool::Brush => Held::Brush,
            Tool::Eraser => Held::Eraser,
            Tool::Picker => Held::Picker,
            _ => Held::Other,
        };
        let (color, size) = if tool == Held::Eraser { (self.secondary, self.size_eraser) } else { (self.primary, 0.0) };
        out.extend(s.presence(Presence { at: self.pointer.map(|p| (p.x, p.y)), tool, color, size }));
        out
    }

    /// A friend's edit changed `base` in `region`: put it under whatever I'm in
    /// the middle of, then draw that on top again.
    fn rebase(&mut self, region: Region) {
        let Some(s) = &self.session else { return };
        let base = &s.base;
        let r = match region {
            Region::Rect(r) if self.img.dimensions() == base.dimensions() => r,
            // Only reached with nothing in progress (see `net_receive`).
            _ => {
                self.img = base.clone();
                self.tex_full = true;
                return;
            }
        };
        if let Op::Stroke { snapshot, .. } = &mut self.op {
            raster::copy_rect(snapshot, base, r);
        }
        if let Some(live) = &mut self.live_shape {
            live.rebase(base, r);
        }
        if let Some(tb) = &mut self.text {
            raster::copy_rect(&mut tb.snapshot, base, r);
            tb.rendered = None;
        }
        let float = self.sel.as_mut().and_then(|sel| sel.float.as_mut());
        let floating = float.is_some();
        match float {
            Some(f) => {
                raster::copy_rect(&mut f.before, base, r);
                raster::copy_rect(&mut f.base, base, r);
                // The hole stays Color 2, except where a friend has drawn into it since.
                if let Some((lifted, color)) = &f.hole {
                    lifted.each(|x, y, was| {
                        if r.contains(pos2(x as f32, y as f32)) && base.get_pixel(x as u32, y as u32).0 == was {
                            f.base.put_pixel(x as u32, y as u32, Rgba(*color));
                        }
                    });
                }
                raster::copy_rect(&mut self.img, &f.base, r);
            }
            None => raster::copy_rect(&mut self.img, base, r),
        }
        if let Op::Stroke { mask, color, opacity, dirty, .. } = &self.op {
            raster::blend_mask(&mut self.img, mask, *color, *opacity, r.intersect(*dirty));
        }
        self.mark(r);
        self.render_live_shape();
        self.update_text_preview();
        if floating {
            self.refresh_float();
        }
    }

    // ---------------------------------------------------------------- tools & colours

    fn set_tool(&mut self, tool: Tool) {
        if tool == self.tool {
            return;
        }
        if self.tool == Tool::Text {
            self.commit_text();
        }
        if self.tool.is_selection() && !tool.is_selection() {
            self.commit_selection();
        }
        if self.tool == Tool::Shape {
            self.commit_shape();
        }
        if tool == Tool::Picker {
            self.prev_tool = self.tool;
        }
        self.tool = tool;
        if tool == Tool::Text {
            self.tab = Tab::Text;
        } else if self.tab == Tab::Text {
            self.tab = Tab::Home;
        }
    }

    fn active_color(&self) -> Rgba8 {
        if self.editing_secondary { self.secondary } else { self.primary }
    }

    fn set_active_color(&mut self, c: Rgba8) {
        if self.editing_secondary {
            self.secondary = c;
        } else {
            self.primary = c;
        }
    }

    fn commit_all(&mut self) {
        self.commit_text();
        self.commit_selection();
        self.commit_shape();
    }

    // ---------------------------------------------------------------- live shape

    fn shape_style(&self, swapped: bool) -> live_shape::Style {
        let (line_color, fill_color) = if swapped { (self.secondary, self.primary) } else { (self.primary, self.secondary) };
        live_shape::Style { outline: self.shape_outline, fill: self.shape_fill, size: self.size_shape, line_color, fill_color }
    }

    /// Bring the live shape's pixels up to date with its geometry and the ribbon.
    fn render_live_shape(&mut self) {
        let Some(swapped) = self.live_shape.as_ref().map(|l| l.swapped) else { return };
        let style = self.shape_style(swapped);
        if let Some(live) = &mut self.live_shape {
            let r = live.render(&mut self.img, style);
            self.tex_dirty = self.tex_dirty.union(r);
        }
    }

    /// Stamp the live shape onto the picture as one undo step.
    fn commit_shape(&mut self) {
        self.render_live_shape();
        if let Some(live) = self.live_shape.take() {
            self.push_patch(live.snapshot(), live.touched());
        }
    }

    /// Choose a shape from the gallery: the shape just drawn is stamped as it
    /// is, and the next drag draws a new one.
    fn pick_shape(&mut self, kind: ShapeKind) {
        self.commit_shape();
        self.shape = kind;
        self.set_tool(Tool::Shape);
    }

    fn discard_shape(&mut self) {
        if let Some(live) = self.live_shape.take() {
            let r = live.erase(&mut self.img);
            self.mark(r);
        }
    }

    /// Whether a press at canvas point `p` lands on the text box or one of its handles.
    fn text_hit(&self, p: Pos2) -> bool {
        let tol = 5.0 / self.zoom;
        self.tool == Tool::Text
            && self.text.as_ref().is_some_and(|tb| tb.bounds.expand(tol).contains(p) || text_handle_at(tb.bounds, p, tol, self.zoom).is_some())
    }

    /// Whether a press at canvas point `p` lands on the live shape (a handle or its body).
    fn live_shape_hit(&self, p: Pos2) -> bool {
        let tol = 5.0 / self.zoom;
        self.tool == Tool::Shape && self.live_shape.as_ref().is_some_and(|l| l.handle_at(p, tol).is_some() || l.contains(p, tol))
    }

    // ---------------------------------------------------------------- selection

    fn select_all(&mut self) {
        self.commit_all();
        self.sel = Some(Selection { rect: IRect::of_image(&self.img), lasso: None, float: None });
        self.tool = Tool::Select;
    }

    /// Pick the selected pixels up so they can move, leaving Color 2 behind.
    fn lift(&mut self) {
        let secondary = self.secondary;
        let Some(sel) = &mut self.sel else { return };
        if sel.float.is_some() {
            return;
        }
        let r = sel.rect.intersect(IRect::of_image(&self.img));
        if r.is_empty() {
            return;
        }
        let before = self.img.clone();
        let mut pixels = raster::crop(&self.img, r);
        let mut base = self.img.clone();
        match &sel.lasso {
            Some(poly) => {
                let inside = lasso_mask(poly, pixels.width(), pixels.height());
                for (x, y, px) in pixels.enumerate_pixels_mut() {
                    if inside.get(x as i32, y as i32) < 0.5 {
                        px.0[3] = 0;
                    } else {
                        base.put_pixel(r.x0 as u32 + x, r.y0 as u32 + y, Rgba(secondary));
                    }
                }
            }
            None => raster::fill_rect(&mut base, r, secondary),
        }
        // Remember the lifted pixels, to redraw the hole if a friend draws under the selection.
        let inside = sel.lasso.as_ref().map(|poly| lasso_mask(poly, r.width() as u32, r.height() as u32));
        let hole = Patch::build(r, |x, y| {
            inside.as_ref().is_none_or(|m| m.get(x - r.x0, y - r.y0) >= 0.5).then(|| before.get_pixel(x as u32, y as u32).0)
        })
        .map(|lifted| (lifted, secondary));
        sel.rect = r;
        sel.float = Some(Floating { pixels, base, before, origin: r, hole, canvas_changed: false });
    }

    fn move_float_to(&mut self, x: i32, y: i32) {
        let key = self.transparent_selection.then(|| rgb(self.secondary));
        let Some(sel) = &mut self.sel else { return };
        let Some(f) = &sel.float else { return };
        let old = sel.rect;
        raster::copy_rect(&mut self.img, &f.base, old);
        raster::blit_over(&mut self.img, &f.pixels, x, y, key);
        sel.rect = IRect::new(x, y, x + f.pixels.width() as i32, y + f.pixels.height() as i32);
        self.tex_dirty = self.tex_dirty.union(old).union(sel.rect);
    }

    fn refresh_float(&mut self) {
        if let Some(r) = self.sel.as_ref().filter(|s| s.float.is_some()).map(|s| s.rect) {
            self.move_float_to(r.x0, r.y0);
        }
    }

    /// Drop the selection, recording any move/paste as one undo step.
    fn commit_selection(&mut self) {
        let Some(sel) = self.sel.take() else { return };
        let Some(f) = sel.float else { return };
        if f.canvas_changed {
            self.record(Change::Replace { before: f.before, after: self.img.clone() });
        } else {
            self.push_patch(&f.before, f.origin.union(sel.rect));
        }
    }

    fn delete_selection(&mut self) {
        let secondary = self.secondary;
        let Some(sel) = self.sel.take() else { return };
        match sel.float {
            Some(f) => {
                self.img = f.base;
                if f.canvas_changed {
                    self.record(Change::Replace { before: f.before, after: self.img.clone() });
                    self.tex_full = true;
                } else {
                    let region = f.origin.union(sel.rect);
                    self.mark(region);
                    self.push_patch(&f.before, region);
                }
            }
            None => {
                let before = self.img.clone();
                let r = sel.rect;
                match &sel.lasso {
                    Some(poly) => {
                        let inside = lasso_mask(poly, r.width() as u32, r.height() as u32);
                        let bounds = IRect::of_image(&self.img);
                        for y in r.y0..r.y1 {
                            for x in r.x0..r.x1 {
                                if inside.get(x - r.x0, y - r.y0) >= 0.5 && bounds.contains(pos2(x as f32, y as f32)) {
                                    self.img.put_pixel(x as u32, y as u32, Rgba(secondary));
                                }
                            }
                        }
                    }
                    None => raster::fill_rect(&mut self.img, r, secondary),
                }
                self.mark(r);
                self.push_patch(&before, r);
            }
        }
    }

    fn selection_pixels(&self) -> Option<RgbaImage> {
        let sel = self.sel.as_ref()?;
        if let Some(f) = &sel.float {
            return Some(f.pixels.clone());
        }
        let mut px = raster::crop(&self.img, sel.rect);
        if let Some(poly) = &sel.lasso {
            let inside = lasso_mask(poly, px.width(), px.height());
            for (x, y, p) in px.enumerate_pixels_mut() {
                if inside.get(x as i32, y as i32) < 0.5 {
                    p.0[3] = 0;
                }
            }
        }
        Some(px)
    }

    fn copy(&mut self) {
        self.commit_text();
        let img = self.selection_pixels().unwrap_or_else(|| self.img.clone());
        let data = arboard::ImageData {
            width: img.width() as usize,
            height: img.height() as usize,
            bytes: std::borrow::Cow::Owned(img.into_raw()),
        };
        if let Err(e) = arboard::Clipboard::new().and_then(|mut c| c.set_image(data)) {
            self.error = Some(format!("Couldn't copy to the clipboard.\n\n{e}"));
        }
    }

    fn cut(&mut self) {
        if self.sel.is_some() {
            self.copy();
            self.delete_selection();
        }
    }

    fn paste(&mut self) {
        let Ok(data) = arboard::Clipboard::new().and_then(|mut c| c.get_image()) else { return };
        if let Some(pixels) = RgbaImage::from_raw(data.width as u32, data.height as u32, data.bytes.into_owned()) {
            self.paste_image(pixels);
        }
    }

    /// Paste as a floating selection at the top-left of the visible canvas,
    /// enlarging the canvas if the image doesn't fit.
    fn paste_image(&mut self, pixels: RgbaImage) {
        self.commit_all();
        let before = self.img.clone();
        let (cw, ch) = self.dims();
        let (pw, ph) = pixels.dimensions();
        let canvas_changed = pw > cw || ph > ch;
        if canvas_changed {
            self.img = raster::resize_canvas(&before, cw.max(pw), ch.max(ph), self.secondary);
            self.tex_full = true;
        }
        let (cw, ch) = self.dims();
        let vx = ((self.scroll_offset.x - MARGIN) / self.zoom).max(0.0) as i32;
        let vy = ((self.scroll_offset.y - MARGIN) / self.zoom).max(0.0) as i32;
        let x = vx.min(cw as i32 - pw as i32).max(0);
        let y = vy.min(ch as i32 - ph as i32).max(0);
        let base = self.img.clone();
        let key = self.transparent_selection.then(|| rgb(self.secondary));
        let r = raster::blit_over(&mut self.img, &pixels, x, y, key);
        self.mark(r);
        self.sel = Some(Selection {
            rect: IRect::new(x, y, x + pw as i32, y + ph as i32),
            lasso: None,
            float: Some(Floating { pixels, base, before, origin: IRect::EMPTY, hole: None, canvas_changed }),
        });
        self.tool = Tool::Select;
    }

    // ---------------------------------------------------------------- image operations

    /// Apply `f` to the selection if there is one, otherwise to the whole image.
    fn transform(&mut self, f: impl Fn(&RgbaImage) -> RgbaImage) {
        self.commit_text();
        if self.sel.is_some() {
            self.lift();
            let Some(sel) = &mut self.sel else { return };
            let Some(fl) = &mut sel.float else { return };
            fl.pixels = f(&fl.pixels);
            sel.lasso = None;
            self.refresh_float();
        } else {
            let before = self.img.clone();
            self.img = f(&before);
            self.record(Change::Replace { before, after: self.img.clone() });
            self.tex_full = true;
        }
    }

    fn crop(&mut self) {
        self.commit_text();
        let Some(r) = self.sel.as_ref().map(|s| s.rect.intersect(IRect::of_image(&self.img))) else { return };
        self.commit_selection();
        if r.is_empty() {
            return;
        }
        let before = self.img.clone();
        self.img = raster::crop(&before, r);
        self.record(Change::Replace { before, after: self.img.clone() });
        self.tex_full = true;
        self.pending_scroll = Some(Vec2::ZERO);
    }

    fn clear_image(&mut self) {
        self.commit_all();
        let before = self.img.clone();
        let all = IRect::of_image(&self.img);
        raster::fill_rect(&mut self.img, all, self.secondary);
        self.mark(all);
        self.push_patch(&before, all);
    }

    fn open_resize_dialog(&mut self) {
        let base = self.sel.as_ref().map_or(self.dims(), |s| (s.rect.width() as u32, s.rect.height() as u32));
        self.resize_dialog = Some(ResizeDialog { percent: true, w: 100.0, h: 100.0, keep_aspect: true, base });
    }

    // ---------------------------------------------------------------- text

    fn update_text_preview(&mut self) {
        let style = self.text_style.clone();
        let (fg, bg) = (self.primary, self.secondary);
        let Some(tb) = &mut self.text else { return };
        let shown = if tb.preedit.is_empty() {
            tb.text.clone()
        } else {
            let mut t = tb.text.clone();
            let at = t.char_indices().nth(tb.cursor.cursor).map_or(t.len(), |(b, _)| b);
            t.insert_str(at, &tb.preedit);
            t
        };
        let key = (shown.clone(), style.clone(), fg, bg, tb.pos);
        if tb.rendered.as_ref() == Some(&key) {
            return;
        }
        let Some(font) = self.fonts.load(style.family, style.bold, style.italic) else { return };
        let px = style.px();
        let layout = text::measure(&font, px, &shown);
        tb.layout = text::caret_layout(&font, px, &shown);
        tb.bounds = Rect::from_min_size(tb.pos, vec2(layout.width.max(px * 0.5), layout.height));
        tb.mask.clear(tb.dirty);
        let mut r = text::render(&mut tb.mask, &font, px, &shown, tb.pos, style.underline, style.strike);
        let opaque = style.opaque && !shown.is_empty();
        let bg_rect = IRect::around(tb.bounds.min, tb.bounds.max, 1.0);
        if opaque {
            r = r.union(bg_rect);
        }
        let region = tb.dirty.union(r);
        raster::copy_rect(&mut self.img, &tb.snapshot, region);
        if opaque {
            raster::fill_rect(&mut self.img, bg_rect, bg);
        }
        raster::blend_mask(&mut self.img, &tb.mask, fg, 1.0, r);
        tb.dirty = r;
        tb.rendered = Some(key);
        self.tex_dirty = self.tex_dirty.union(region);
    }

    fn discard_text(&mut self) {
        if let Some(tb) = self.text.take() {
            raster::copy_rect(&mut self.img, &tb.snapshot, tb.dirty);
            self.mark(tb.dirty);
        }
    }

    fn commit_text(&mut self) {
        self.update_text_preview();
        let Some(tb) = self.text.take() else { return };
        if tb.text.trim().is_empty() {
            raster::copy_rect(&mut self.img, &tb.snapshot, tb.dirty);
            self.mark(tb.dirty);
        } else {
            self.push_patch(&tb.snapshot, tb.dirty);
        }
    }

    // ---------------------------------------------------------------- zoom

    fn zoom_at(&mut self, z: f32, anchor: Option<Vec2>) {
        let z = z.clamp(0.05, 64.0);
        if (z - self.zoom).abs() < 1e-4 {
            return;
        }
        // Keep the canvas point under `anchor` (viewport coordinates) in place.
        let a = anchor.unwrap_or(self.view_rect.size() * 0.5);
        let px = (self.scroll_offset + a - Vec2::splat(MARGIN)) / self.zoom;
        self.zoom = z;
        self.pending_scroll = Some((Vec2::splat(MARGIN) + px * z - a).max(Vec2::ZERO));
    }

    fn zoom_step(&mut self, up: bool, anchor: Option<Vec2>) {
        let z = if up {
            ZOOM_STEPS.iter().copied().find(|s| *s > self.zoom + 1e-3).unwrap_or(64.0)
        } else {
            ZOOM_STEPS.iter().rev().copied().find(|s| *s < self.zoom - 1e-3).unwrap_or(0.05)
        };
        self.zoom_at(z, anchor);
    }

    fn zoom_fit(&mut self) {
        let avail = self.view_rect.size() - Vec2::splat(MARGIN * 2.0 + 12.0);
        let (w, h) = self.dims();
        self.zoom = (avail.x / w as f32).min(avail.y / h as f32).clamp(0.05, 64.0);
        self.pending_scroll = Some(Vec2::ZERO);
    }

    // ---------------------------------------------------------------- canvas interaction

    fn begin(&mut self, p: Pos2, button: PointerButton, anchor: Vec2) {
        let secondary = button == PointerButton::Secondary;
        let c1 = if secondary { self.secondary } else { self.primary };
        let (w, h) = self.dims();
        let inside = p.x >= 0.0 && p.y >= 0.0 && p.x < w as f32 && p.y < h as f32;
        match self.tool {
            Tool::Pencil | Tool::Brush | Tool::Eraser => {
                self.commit_all();
                let (pen, color, opacity, size) = match self.tool {
                    Tool::Pencil => (Pen::Pencil, c1, 1.0, self.size_pencil),
                    Tool::Eraser => (Pen::Eraser, self.secondary, 1.0, self.size_eraser),
                    _ => (Pen::Brush(self.brush), c1, self.brush.opacity(), self.size_brush),
                };
                self.op = Op::Stroke {
                    button,
                    pen,
                    snapshot: self.img.clone(),
                    mask: Mask::new(w, h),
                    color,
                    opacity,
                    size,
                    last: p,
                    dirty: IRect::EMPTY,
                };
                self.stroke_to(&[p], true);
            }
            Tool::Fill => {
                if !inside {
                    return;
                }
                self.commit_all();
                let before = self.img.clone();
                let tolerance = (self.fill_tolerance / 100.0 * 255.0).round() as u8;
                let r = raster::flood_fill(&mut self.img, p.x as u32, p.y as u32, c1, tolerance);
                self.mark(r);
                self.push_patch(&before, r);
            }
            Tool::Picker => {
                if !inside {
                    return;
                }
                let c = self.img.get_pixel(p.x as u32, p.y as u32).0;
                if secondary {
                    self.secondary = c;
                } else {
                    self.primary = c;
                }
                self.tool = self.prev_tool;
            }
            Tool::Magnifier => self.zoom_step(!secondary, Some(anchor)),
            Tool::Text => {
                let tol = 5.0 / self.zoom;
                if let Some(tb) = &self.text {
                    if let Some(handle) = text_handle_at(tb.bounds, p, tol, self.zoom) {
                        self.op = Op::TextScale { button, handle, start_size: self.text_style.size_pt, start_bounds: tb.bounds };
                        return;
                    }
                    if tb.bounds.expand(tol).contains(p) {
                        self.op = Op::TextMove { button, grab: p, start: tb.pos, moved: false };
                        return;
                    }
                }
                self.commit_all();
                if !inside {
                    return;
                }
                self.text = Some(TextBox {
                    pos: pos2(p.x.floor(), p.y.floor()),
                    text: String::new(),
                    snapshot: self.img.clone(),
                    mask: Mask::new(w, h),
                    dirty: IRect::EMPTY,
                    bounds: Rect::from_min_size(p, Vec2::ZERO),
                    rendered: None,
                    editing: true,
                    cursor: TextCursor::default(),
                    layout: text::CaretLayout::default(),
                    preedit: String::new(),
                    blink: self.ctx.input(|i| i.time),
                });
            }
            Tool::Shape => {
                let tol = 5.0 / self.zoom;
                if let Some(live) = &self.live_shape {
                    let start = (live.a, live.b);
                    if let Some(handle) = live.handle_at(p, tol) {
                        self.op = Op::ShapeHandle { button, handle, start };
                        return;
                    }
                    if live.contains(p, tol) {
                        self.op = Op::ShapeMove { button, grab: p, start };
                        return;
                    }
                }
                // Clicking away from the live shape stamps it and starts a new one.
                self.commit_all();
                self.live_shape = Some(LiveShape::new(self.shape, p, secondary, self.img.clone()));
                self.op = Op::ShapeCreate { button };
            }
            Tool::Select | Tool::FreeSelect => {
                self.commit_text();
                if let Some(r) = self.sel.as_ref().map(|s| s.rect).filter(|r| r.contains(p)) {
                    self.lift();
                    self.op = Op::MovingSel { button, grab: p, origin: (r.x0, r.y0) };
                    return;
                }
                self.commit_selection();
                let q = clamp_to(p, w, h);
                self.op = Op::Selecting { button, start: q, end: q, lasso: vec![q] };
            }
        }
    }

    fn update(&mut self, pts: &[Pos2], shift: bool, dt: f32) {
        self.update_op(pts, shift, dt);
        self.render_live_shape();
        self.update_text_preview();
    }

    fn update_op(&mut self, pts: &[Pos2], shift: bool, dt: f32) {
        let Some(&p) = pts.last() else { return };
        match self.op {
            Op::Stroke { .. } => {
                self.stroke_to(pts, false);
                self.spray_frame(dt);
            }
            Op::ShapeCreate { .. } => {
                if let Some(live) = &mut self.live_shape {
                    live.b = shapes::constrain(live.kind, live.a, p, shift);
                }
            }
            Op::ShapeHandle { handle, start, .. } => {
                if let Some(live) = &mut self.live_shape {
                    live.drag_handle(handle, start, p, shift);
                }
            }
            Op::ShapeMove { grab, start, .. } => {
                if let Some(live) = &mut self.live_shape {
                    live.translate(start, p - grab);
                }
            }
            Op::TextMove { .. } => {
                let zoom = self.zoom;
                if let (Op::TextMove { grab, start, moved, .. }, Some(tb)) = (&mut self.op, &mut self.text) {
                    let d = p - *grab;
                    *moved |= d.length() * zoom > 2.0;
                    if *moved {
                        tb.pos = (*start + d).round();
                    }
                }
            }
            Op::TextScale { handle, start_size, start_bounds, .. } => {
                // Scale about the opposite corner, following the larger of the two stretches.
                let corners = [start_bounds.left_top(), start_bounds.right_top(), start_bounds.right_bottom(), start_bounds.left_bottom()];
                let anchor = corners[(handle + 2) % 4];
                let d = (p - anchor).abs();
                let ratio = (d.x / start_bounds.width().max(1.0)).max(d.y / start_bounds.height().max(1.0));
                let size = (start_size * ratio).round().clamp(4.0, 400.0);
                self.text_style.size_pt = size;
                let k = size / start_size;
                let (w, h) = (start_bounds.width() * k, start_bounds.height() * k);
                let (left, top) = (matches!(handle, 0 | 3), matches!(handle, 0 | 1));
                if let Some(tb) = &mut self.text {
                    tb.pos = pos2(if left { anchor.x - w } else { anchor.x }, if top { anchor.y - h } else { anchor.y }).round();
                }
            }
            Op::Selecting { .. } => {
                let (w, h) = self.dims();
                if let Op::Selecting { end, lasso, .. } = &mut self.op {
                    for q in pts.iter().map(|q| clamp_to(*q, w, h)) {
                        *end = q;
                        if lasso.last().is_none_or(|l| l.distance(q) >= 1.0) {
                            lasso.push(q);
                        }
                    }
                }
            }
            Op::MovingSel { grab, origin, .. } => {
                let d = p - grab;
                self.move_float_to(origin.0 + d.x.round() as i32, origin.1 + d.y.round() as i32);
            }
            Op::ResizeCanvas { handle, .. } => {
                let (w, h) = self.dims();
                let nw = if handle == Handle::Bottom { w } else { p.x.round().clamp(1.0, 30000.0) as u32 };
                let nh = if handle == Handle::Right { h } else { p.y.round().clamp(1.0, 30000.0) as u32 };
                if let Op::ResizeCanvas { size, .. } = &mut self.op {
                    *size = (nw, nh);
                }
            }
            Op::Pan { .. } | Op::Idle => {}
        }
    }

    fn stroke_to(&mut self, pts: &[Pos2], first: bool) {
        let Op::Stroke { pen, snapshot, mask, color, opacity, size, last, dirty, .. } = &mut self.op else { return };
        let mut r = IRect::EMPTY;
        for &p in pts {
            if !first && p == *last {
                continue;
            }
            let seg = match pen {
                Pen::Pencil | Pen::Eraser => mask.square_line(*last, p, size.round() as i32),
                Pen::Brush(kind) => kind.segment(mask, *last, p, *size, &mut self.rng),
            };
            r = r.union(seg);
            *last = p;
        }
        if r.is_empty() {
            return;
        }
        raster::copy_rect(&mut self.img, snapshot, r);
        raster::blend_mask(&mut self.img, mask, *color, *opacity, r);
        *dirty = dirty.union(r);
        self.tex_dirty = self.tex_dirty.union(r);
    }

    /// The airbrush keeps spraying while the button is held, even without moving.
    fn spray_frame(&mut self, dt: f32) {
        let Op::Stroke { pen: Pen::Brush(BrushKind::Airbrush), snapshot, mask, color, opacity, size, last, dirty, .. } =
            &mut self.op
        else {
            return;
        };
        let n = (*size * *size * 0.12 * dt * 60.0).clamp(2.0, 2000.0) as usize;
        let r = brush::spray(mask, *last, *size, &mut self.rng, n);
        raster::copy_rect(&mut self.img, snapshot, r);
        raster::blend_mask(&mut self.img, mask, *color, *opacity, r);
        *dirty = dirty.union(r);
        self.tex_dirty = self.tex_dirty.union(r);
    }

    fn end(&mut self) {
        match std::mem::replace(&mut self.op, Op::Idle) {
            Op::Stroke { snapshot, dirty, .. } => self.push_patch(&snapshot, dirty),
            Op::ShapeCreate { .. } => {
                // A click without a drag draws nothing.
                if self.live_shape.as_ref().is_some_and(|l| l.is_empty()) {
                    self.discard_shape();
                }
            }
            Op::ShapeHandle { .. } | Op::ShapeMove { .. } => {}
            Op::TextMove { moved, grab, .. } => {
                // A click (no drag) puts the caret where it landed and resumes typing.
                if let (false, Some(tb)) = (moved, &mut self.text) {
                    let index = tb.layout.index_at(grab - tb.pos);
                    tb.cursor.move_to(index, false);
                    tb.editing = true;
                    tb.blink = self.ctx.input(|i| i.time);
                }
            }
            Op::TextScale { .. } => {}
            Op::Selecting { start, end, lasso, .. } => {
                if self.tool == Tool::FreeSelect {
                    let (mut lo, mut hi) = (lasso[0], lasso[0]);
                    for p in &lasso {
                        lo = lo.min(*p);
                        hi = hi.max(*p);
                    }
                    let r = IRect::new(lo.x.floor() as i32, lo.y.floor() as i32, hi.x.ceil() as i32, hi.y.ceil() as i32);
                    if lasso.len() >= 3 && r.width() >= 2 && r.height() >= 2 {
                        let off = vec2(r.x0 as f32, r.y0 as f32);
                        let poly = lasso.iter().map(|p| *p - off).collect();
                        self.sel = Some(Selection { rect: r, lasso: Some(poly), float: None });
                    }
                } else {
                    let r = IRect::new(
                        start.x.min(end.x).round() as i32,
                        start.y.min(end.y).round() as i32,
                        start.x.max(end.x).round() as i32,
                        start.y.max(end.y).round() as i32,
                    );
                    if !r.is_empty() {
                        self.sel = Some(Selection { rect: r, lasso: None, float: None });
                    }
                }
            }
            Op::ResizeCanvas { size: (nw, nh), .. } => {
                if (nw, nh) != self.dims() {
                    let before = self.img.clone();
                    self.img = raster::resize_canvas(&before, nw, nh, self.secondary);
                    self.record(Change::Replace { before, after: self.img.clone() });
                    self.tex_full = true;
                }
            }
            Op::MovingSel { .. } | Op::Pan { .. } | Op::Idle => {}
        }
    }

    fn canvas_input(&mut self, ctx: &Context, resp: &Response, crect: Rect) {
        let zoom = self.zoom;
        let to_canvas = move |p: Pos2| pos2((p.x - crect.min.x) / zoom, (p.y - crect.min.y) / zoom);
        let (w, h) = self.dims();
        let hover = resp.hover_pos();
        self.pointer = resp
            .hover_pos()
            .or_else(|| (!matches!(self.op, Op::Idle)).then(|| ctx.input(|i| i.pointer.latest_pos())).flatten())
            .map(to_canvas)
            .filter(|p| p.x >= 0.0 && p.y >= 0.0 && p.x <= w as f32 && p.y <= h as f32);
        self.cursor = hover
            .map(to_canvas)
            .filter(|p| p.x >= 0.0 && p.y >= 0.0 && p.x < w as f32 && p.y < h as f32)
            .map(|p| (p.x as i32, p.y as i32));

        let (moves, latest, shift, pressed, dt) = ctx.input(|i| {
            let moves: Vec<Pos2> =
                i.events.iter().filter_map(|e| if let Event::PointerMoved(p) = e { Some(*p) } else { None }).collect();
            let pressed = [PointerButton::Primary, PointerButton::Secondary, PointerButton::Middle]
                .into_iter()
                .find(|b| i.pointer.button_pressed(*b));
            (moves, i.pointer.latest_pos(), i.modifiers.shift, pressed, i.stable_dt)
        });

        self.tool_cursor = None;
        if matches!(self.op, Op::Idle) {
            self.hover_cursor(ctx, hover, crect);
            let (Some(hp), Some(button)) = (hover, pressed) else { return };
            if button == PointerButton::Middle {
                self.op = Op::Pan { last: hp };
            } else if self.live_shape_hit(to_canvas(hp)) || self.text_hit(to_canvas(hp)) {
                self.begin(to_canvas(hp), button, hp - self.view_rect.min);
            } else if let Some(handle) = hit_handle(crect, hp) {
                self.commit_all();
                self.op = Op::ResizeCanvas { button, handle, size: (w, h) };
            } else if self.tool.is_selection() && button == PointerButton::Secondary {
                // Handled by the context menu.
            } else if crect.contains(hp) || self.tool.is_selection() {
                self.begin(to_canvas(hp), button, hp - self.view_rect.min);
            }
            return;
        }

        let down = ctx.input(|i| i.pointer.button_down(self.op.button()));
        match &mut self.op {
            Op::Pan { last } => {
                if let Some(p) = latest {
                    let delta = p - *last;
                    *last = p;
                    self.pending_scroll = Some(self.scroll_offset - delta);
                }
            }
            _ => {
                let mut pts: Vec<Pos2> = moves.into_iter().map(to_canvas).collect();
                if pts.is_empty() {
                    pts.extend(latest.map(to_canvas));
                }
                self.update(&pts, shift, dt);
            }
        }
        let stroke_pointer = matches!(self.op, Op::Stroke { .. }).then(|| tool_pointer(self.tool)).flatten();
        if let (Some(icon), Some(p)) = (stroke_pointer, latest) {
            self.tool_cursor = Some((icon, p));
        }
        ctx.set_cursor_icon(match &self.op {
            _ if self.tool_cursor.is_some() => CursorIcon::None,
            Op::Stroke { pen: Pen::Eraser, .. } if latest.is_some_and(|p| crect.contains(p)) => CursorIcon::None,
            Op::MovingSel { .. } | Op::ShapeMove { .. } => CursorIcon::Move,
            Op::ShapeHandle { handle, .. } => live_shape::handle_cursor(self.shape == ShapeKind::Line, *handle),
            Op::TextMove { .. } => CursorIcon::Move,
            Op::TextScale { handle, .. } => text_handle_cursor(*handle),
            Op::Pan { .. } => CursorIcon::Grabbing,
            Op::ResizeCanvas { handle, .. } => handle_cursor(*handle),
            _ => CursorIcon::Crosshair,
        });
        if !down {
            self.end();
        }
    }

    fn hover_cursor(&mut self, ctx: &Context, hover: Option<Pos2>, crect: Rect) {
        let Some(hp) = hover else { return };
        if let (Tool::Text, Some(tb)) = (self.tool, &self.text) {
            let p = pos2((hp.x - crect.min.x) / self.zoom, (hp.y - crect.min.y) / self.zoom);
            let tol = 5.0 / self.zoom;
            if let Some(h) = text_handle_at(tb.bounds, p, tol, self.zoom) {
                ctx.set_cursor_icon(text_handle_cursor(h));
                return;
            }
            if tb.bounds.expand(tol).contains(p) {
                ctx.set_cursor_icon(CursorIcon::Move);
                return;
            }
        }
        if let (Tool::Shape, Some(live)) = (self.tool, &self.live_shape) {
            let p = pos2((hp.x - crect.min.x) / self.zoom, (hp.y - crect.min.y) / self.zoom);
            let tol = 5.0 / self.zoom;
            if let Some(h) = live.handle_at(p, tol) {
                ctx.set_cursor_icon(live_shape::handle_cursor(live.is_line(), h));
                return;
            }
            if live.contains(p, tol) {
                ctx.set_cursor_icon(CursorIcon::Move);
                return;
            }
        }
        if let Some(handle) = hit_handle(crect, hp) {
            ctx.set_cursor_icon(handle_cursor(handle));
            return;
        }
        if !crect.contains(hp) {
            return;
        }
        if let Some(icon) = tool_pointer(self.tool) {
            self.tool_cursor = Some((icon, hp));
            ctx.set_cursor_icon(CursorIcon::None);
            return;
        }
        if self.tool == Tool::Eraser {
            // The eraser's footprint (drawn with the canvas) is the pointer.
            ctx.set_cursor_icon(CursorIcon::None);
            return;
        }
        let p = pos2((hp.x - crect.min.x) / self.zoom, (hp.y - crect.min.y) / self.zoom);
        ctx.set_cursor_icon(match self.tool {
            Tool::Select | Tool::FreeSelect if self.sel.as_ref().is_some_and(|s| s.rect.contains(p)) => CursorIcon::Move,
            Tool::Text => CursorIcon::Text,
            Tool::Magnifier => CursorIcon::ZoomIn,
            _ => CursorIcon::Crosshair,
        });
    }

    fn sync_texture(&mut self, ctx: &Context) {
        self.preview_dirty = if self.tex_full { IRect::of_image(&self.img) } else { self.preview_dirty.union(self.tex_dirty) };
        // Friends' drawings in progress are part of the picture on screen (one
        // texture, so zooming blends them exactly like everything else).
        if let Some(s) = &mut self.session {
            self.tex_dirty = self.tex_dirty.union(std::mem::replace(&mut s.overlay_dirty, IRect::EMPTY));
        }
        let friends = self.session.as_ref().filter(|s| s.has_previews()).map(|s| &s.overlay);
        let nearest = self.zoom >= 1.0;
        let opts = if nearest { TextureOptions::NEAREST } else { TextureOptions::LINEAR };
        let (w, h) = (self.img.width() as usize, self.img.height() as usize);
        let full = self.tex_full || nearest != self.tex_nearest || self.tex.as_ref().is_none_or(|t| t.size() != [w, h]);
        if full {
            let image = with_friends(&self.img, friends, IRect::of_image(&self.img));
            match &mut self.tex {
                Some(t) => t.set(image, opts),
                None => self.tex = Some(ctx.load_texture("canvas", image, opts)),
            }
            self.tex_full = false;
            self.tex_nearest = nearest;
        } else {
            let r = self.tex_dirty.intersect(IRect::of_image(&self.img));
            if let (false, Some(t)) = (r.is_empty(), &mut self.tex) {
                t.set_partial([r.x0 as usize, r.y0 as usize], with_friends(&self.img, friends, r), opts);
            }
        }
        self.tex_dirty = IRect::EMPTY;
    }

    /// Friends' pointers: their tool (or an arrow) in their colour, with their name.
    fn paint_friends(&self, painter: &egui::Painter, crect: Rect) {
        let Some(s) = &self.session else { return };
        let now = self.ctx.input(|i| i.time);
        let mut fading = false;
        for (id, (p, since)) in &s.pointers {
            let (Some((x, y)), Some(kid)) = (p.at, s.kids.iter().find(|k| k.id == *id)) else {
                continue;
            };
            if now - since > 10.0 {
                continue;
            }
            fading = true;
            let at = crect.min + vec2(x, y) * self.zoom;
            let [r, g, b] = kid.color;
            let color = Color32::from_rgb(r, g, b);
            match p.tool {
                Held::Pencil => paint_tool_pointer(painter, Icon::Pencil, at),
                Held::Fill => paint_tool_pointer(painter, Icon::Fill, at),
                Held::Brush => paint_tool_pointer(painter, Icon::Brushes, at),
                Held::Picker => paint_tool_pointer(painter, Icon::Picker, at),
                Held::Eraser => {
                    // Their eraser's footprint, filled with what it paints, outlined in their colour.
                    let r = Rect::from_center_size(at, Vec2::splat((p.size * self.zoom).max(4.0)));
                    painter.rect_filled(r, 0.0, c32(p.color));
                    painter.rect_stroke(r, 0.0, Stroke::new(2.0, color), StrokeKind::Outside);
                }
                Held::Other => {
                    let arrow = [
                        vec2(0.0, 0.0),
                        vec2(0.0, 16.0),
                        vec2(4.2, 12.2),
                        vec2(7.0, 18.0),
                        vec2(9.4, 17.0),
                        vec2(6.6, 11.2),
                        vec2(12.0, 11.2),
                    ];
                    let pts: Vec<Pos2> = arrow.iter().map(|d| at + *d).collect();
                    painter.add(egui::Shape::convex_polygon(pts[..3].iter().chain(&pts[5..]).copied().collect(), color, Stroke::NONE));
                    painter.add(egui::Shape::convex_polygon(pts[2..6].to_vec(), color, Stroke::NONE));
                    painter.add(egui::Shape::closed_line(pts, Stroke::new(1.2, Color32::WHITE)));
                }
            }
            let galley = painter.layout_no_wrap(kid.name.clone(), FontId::proportional(11.5), Color32::WHITE);
            let tag = Rect::from_min_size(at + vec2(10.0, 16.0), galley.size() + vec2(12.0, 5.0));
            painter.rect_filled(tag, 7.0, color);
            painter.galley(tag.min + vec2(6.0, 2.5), galley, Color32::WHITE);
        }
        if fading {
            self.ctx.request_repaint_after(std::time::Duration::from_secs(1));
        }
    }

    // ---------------------------------------------------------------- UI

    fn handle_keys(&mut self, ctx: &Context) {
        if ctx.egui_wants_keyboard_input() || self.confirm.is_some() || self.resize_dialog.is_some() {
            self.paste_key_was_down = false;
            return;
        }
        let (events, cmd) = ctx.input(|i| (i.events.clone(), i.modifiers.command));
        let mut pasted = false;
        let mut last_text = None;
        for event in events {
            // The same keystroke can arrive as both typed text and an input-method commit.
            if let Event::Ime(egui::ImeEvent::Commit(c)) = &event {
                if last_text.as_ref() == Some(c) {
                    continue;
                }
            }
            if let Event::Text(t) = &event {
                last_text = Some(t.clone());
            }
            if self.text_event(&event) {
                pasted |= matches!(event, Event::Paste(_));
                self.update_text_preview();
                continue;
            }
            match event {
                Event::Copy => self.copy(),
                Event::Cut => self.cut(),
                Event::Paste(_) if !pasted => {
                    self.paste();
                    pasted = true;
                }
                Event::Key { key, pressed: true, modifiers, .. } => self.key(key, modifiers),
                _ => {}
            }
        }
        // egui drops ⌘V entirely when the clipboard holds an image but no text,
        // so watch the key itself.
        let typing = self.text.as_ref().is_some_and(|t| t.editing);
        let v_down = cmd && !typing && paste_key_down();
        if v_down && !self.paste_key_was_down && !pasted {
            self.paste();
        }
        self.paste_key_was_down = v_down;
    }

    /// Typing into the text box on the canvas. Returns whether the event was used.
    /// Selection highlight and blinking caret for the text being typed.
    fn paint_caret(&self, painter: &egui::Painter, tb: &TextBox, to_screen: &dyn Fn(Pos2) -> Pos2, frame: Rect) {
        let layout = &tb.layout;
        let line_h = layout.line_h * self.zoom;
        let sel = tb.cursor.range();
        if !sel.is_empty() {
            let (l0, _) = layout.line_col(sel.start);
            let (l1, _) = layout.line_col(sel.end);
            for l in l0..=l1 {
                let from = if l == l0 { sel.start } else { layout.index(l, 0) };
                let to = if l == l1 { sel.end } else { layout.index(l, layout.lines[l].len() - 1) };
                let a = to_screen(tb.pos + layout.caret(from));
                let b = to_screen(tb.pos + layout.caret(to)) + vec2(if l == l1 { 0.0 } else { 4.0 }, line_h);
                painter.rect_filled(Rect::from_min_max(a, b), 0.0, Color32::from_rgba_unmultiplied(0, 120, 215, 70));
            }
        }
        let index = tb.cursor.cursor + tb.preedit.chars().count();
        let top = to_screen(tb.pos + layout.caret(index));
        let caret = Rect::from_min_size(top - vec2(0.5, 0.0), vec2(1.5, line_h));
        let ctx = painter.ctx();
        let since = ctx.input(|i| i.time) - tb.blink;
        if since % 1.06 < 0.53 {
            painter.rect_filled(caret, 0.0, Color32::BLACK);
        }
        ctx.request_repaint_after(std::time::Duration::from_millis(80));
        // Lets the system input method (accents, CJK) position its window.
        ctx.output_mut(|o| o.ime = Some(egui::output::IMEOutput { purpose: egui::IMEPurpose::Normal, rect: frame, cursor_rect: caret, should_interrupt_composition: false }));
    }

    fn text_event(&mut self, event: &Event) -> bool {
        let now = self.ctx.input(|i| i.time);
        let Some(tb) = self.text.as_mut().filter(|t| t.editing) else { return false };
        let (text, cur, layout) = (&mut tb.text, &mut tb.cursor, &tb.layout);
        let used = match event {
            Event::Text(t) => {
                let t: String = t.chars().filter(|c| !c.is_control()).collect();
                cur.insert(text, &t);
                true
            }
            Event::Ime(egui::ImeEvent::Commit(t)) => {
                tb.preedit.clear();
                cur.insert(text, t);
                true
            }
            Event::Ime(egui::ImeEvent::Preedit { text: t, .. }) => {
                tb.preedit = t.clone();
                true
            }
            Event::Paste(t) => {
                cur.insert(text, &t.replace("\r\n", "\n"));
                true
            }
            Event::Copy | Event::Cut => {
                if cur.has_selection() {
                    self.ctx.copy_text(cur.selected(text).to_owned());
                    if matches!(event, Event::Cut) {
                        cur.delete_selection(text);
                    }
                }
                true
            }
            Event::Key { key, pressed: true, modifiers: m, .. } => {
                // On a Mac ⌥ moves by word and ⌘ by line; elsewhere Ctrl moves by word.
                let (word, line) = if cfg!(target_os = "macos") { (m.alt, m.command) } else { (m.command, false) };
                let (shift, cmd) = (m.shift, m.command);
                match key {
                    Key::Backspace => cur.backspace(text, layout, word, line),
                    Key::Delete => cur.delete(text, word),
                    Key::Enter => cur.insert(text, "\n"),
                    Key::ArrowLeft if line => cur.line_start(layout, shift),
                    Key::ArrowRight if line => cur.line_end(layout, shift),
                    Key::ArrowLeft => cur.left(text, shift, word),
                    Key::ArrowRight => cur.right(text, shift, word),
                    Key::ArrowUp => cur.vertical(text, layout, -1, shift),
                    Key::ArrowDown => cur.vertical(text, layout, 1, shift),
                    Key::Home => cur.line_start(layout, shift),
                    Key::End => cur.line_end(layout, shift),
                    Key::A if cmd => cur.select_all(text),
                    Key::Z if cmd && shift => cur.redo(text),
                    Key::Y if cmd => cur.redo(text),
                    Key::Z if cmd => cur.undo(text),
                    Key::Escape => tb.editing = false,
                    // Anything else (e.g. ⌘S) goes on to the app.
                    _ => return false,
                }
                true
            }
            // Releases of keys we handle.
            Event::Key { key, pressed: false, .. } => {
                matches!(key, Key::Backspace | Key::Delete | Key::Enter | Key::Escape | Key::ArrowLeft | Key::ArrowRight | Key::ArrowUp | Key::ArrowDown)
            }
            _ => false,
        };
        if used {
            tb.blink = now;
        }
        used
    }

    fn key(&mut self, key: Key, m: Modifiers) {
        let cmd = m.command;
        match key {
            Key::Z if cmd && m.shift => self.redo(),
            Key::Z if cmd => self.undo(),
            Key::Y if cmd => self.redo(),
            Key::N if cmd && !m.shift => self.request(Pending::New),
            Key::O if cmd => self.request(Pending::Open(None)),
            Key::S if cmd && m.shift => _ = self.save_as(),
            Key::S if cmd => _ = self.save(),
            Key::A if cmd => self.select_all(),
            Key::X if cmd && m.shift => self.crop(),
            Key::I if cmd && m.shift => self.transform(raster::invert),
            Key::W if cmd => self.open_resize_dialog(),
            Key::G if cmd => self.show_grid = !self.show_grid,
            Key::R if cmd => self.show_rulers = !self.show_rulers,
            Key::E if cmd => self.open_properties(),
            Key::F11 => self.set_full_screen(!self.full_screen),
            Key::N if cmd && m.shift => self.clear_image(),
            Key::Escape if self.full_screen => self.set_full_screen(false),
            Key::Equals | Key::Plus if cmd => self.zoom_step(true, None),
            Key::Minus if cmd => self.zoom_step(false, None),
            Key::Num0 if cmd => self.zoom_fit(),
            Key::Num1 if cmd => self.zoom_at(1.0, None),
            Key::Delete | Key::Backspace if !cmd && self.live_shape.is_some() => self.discard_shape(),
            Key::Delete | Key::Backspace if !cmd && self.text.is_some() => self.discard_text(),
            Key::Enter if self.text.is_some() => self.commit_text(),
            Key::Delete | Key::Backspace if !cmd => self.delete_selection(),
            Key::Escape | Key::Enter if self.live_shape.is_some() => self.commit_shape(),
            Key::Escape => {
                if self.text.is_some() {
                    self.commit_text();
                } else {
                    self.commit_selection();
                }
            }
            Key::ArrowLeft | Key::ArrowRight | Key::ArrowUp | Key::ArrowDown if self.text.is_some() => {
                let d = match key {
                    Key::ArrowLeft => vec2(-1.0, 0.0),
                    Key::ArrowRight => vec2(1.0, 0.0),
                    Key::ArrowUp => vec2(0.0, -1.0),
                    _ => vec2(0.0, 1.0),
                };
                if let Some(tb) = &mut self.text {
                    tb.pos += d;
                }
            }
            Key::ArrowLeft | Key::ArrowRight | Key::ArrowUp | Key::ArrowDown if self.live_shape.is_some() => {
                let d = match key {
                    Key::ArrowLeft => vec2(-1.0, 0.0),
                    Key::ArrowRight => vec2(1.0, 0.0),
                    Key::ArrowUp => vec2(0.0, -1.0),
                    _ => vec2(0.0, 1.0),
                };
                if let Some(live) = &mut self.live_shape {
                    let start = (live.a, live.b);
                    live.translate(start, d);
                }
            }
            Key::ArrowLeft | Key::ArrowRight | Key::ArrowUp | Key::ArrowDown if self.sel.is_some() => {
                let (dx, dy) = match key {
                    Key::ArrowLeft => (-1, 0),
                    Key::ArrowRight => (1, 0),
                    Key::ArrowUp => (0, -1),
                    _ => (0, 1),
                };
                self.lift();
                if let Some(r) = self.sel.as_ref().map(|s| s.rect) {
                    self.move_float_to(r.x0 + dx, r.y0 + dy);
                }
            }
            _ => {}
        }
    }

    fn shortcut(&self, m: Modifiers, k: Key) -> String {
        self.ctx.format_shortcut(&KeyboardShortcut::new(m, k))
    }

    /// Our own title bar (the window's is hidden): Quick Access Toolbar and title.
    fn title_bar(&mut self, ui: &mut Ui) {
        let p = Palette::of(ui);
        let ctx = ui.ctx().clone();
        let bar = ui.max_rect();
        // On macOS this strip *is* the title bar (the system one is hidden): it shows the
        // title and moves the window. Elsewhere the window keeps its own title bar and
        // this is just the Quick Access Toolbar.
        if cfg!(target_os = "macos") {
            // Registered first so the buttons on top of it win.
            let drag = ui.interact(bar, Id::new("titlebar-drag"), Sense::click_and_drag());
            if drag.drag_started_by(PointerButton::Primary) {
                ctx.send_viewport_cmd(ViewportCommand::StartDrag);
            }
            if drag.double_clicked() {
                let maximized = ctx.input(|i| i.viewport().maximized.unwrap_or(false));
                ctx.send_viewport_cmd(ViewportCommand::Maximized(!maximized));
            }
            let title = format!("{} - Paint", self.doc_name());
            ui.painter().text(bar.center(), Align2::CENTER_CENTER, title, FontId::proportional(12.5), p.text);
        }

        ui.horizontal_centered(|ui| {
            ui.spacing_mut().item_spacing.x = 0.0;
            // Room for the macOS traffic lights.
            ui.add_space(if cfg!(target_os = "macos") { 74.0 } else { 6.0 });
            let (logo, _) = ui.allocate_exact_size(Vec2::splat(20.0), Sense::hover());
            paint_logo(ui.painter(), logo);
            ui.add_space(6.0);
            let tip = format!("Save ({})", self.shortcut(Modifiers::COMMAND, Key::S));
            if ribbon::icon_button(ui, Icon::Save, false, true, &tip).clicked() {
                self.save();
            }
            let can_undo = self.history.can_undo() || self.is_dirty();
            let tip = format!("Undo ({})", self.shortcut(Modifiers::COMMAND, Key::Z));
            if ribbon::icon_button(ui, Icon::Undo, false, can_undo, &tip).clicked() {
                self.undo();
            }
            let tip = format!("Redo ({})", self.shortcut(Modifiers::COMMAND | Modifiers::SHIFT, Key::Z));
            if ribbon::icon_button(ui, Icon::Redo, false, self.history.can_redo(), &tip).clicked() {
                self.redo();
            }
        });
    }

    /// Tab strip and ribbon body.
    fn ribbon(&mut self, ui: &mut Ui) {
        let p = Palette::of(ui);
        let full = ui.max_rect();
        // The tab strip and the ribbon body sit flush; the default gap would show white.
        let spacing = ui.spacing().item_spacing;
        ui.spacing_mut().item_spacing.y = 0.0;
        if self.tab == Tab::Text && self.tool != Tool::Text {
            self.tab = Tab::Home;
        }
        let mut selected = None;
        let strip = ui.allocate_ui_with_layout(vec2(full.width(), 24.0), Layout::left_to_right(Align::Min), |ui| {
            // Room for the selected tab's flares between neighbours.
            ui.spacing_mut().item_spacing.x = ribbon::TAB_FLARE;
            ui.add_space(4.0);
            let file = ribbon::file_tab(ui);
            if file.clicked() {
                self.file_menu_open = !self.file_menu_open;
                self.file_menu_formats = false;
            }
            self.file_tab_rect = file.rect;
            let mut tabs = vec![(Tab::Home, "Home", false), (Tab::View, "View", false), (Tab::Together, "Together", false)];
            if self.tool == Tool::Text {
                tabs.push((Tab::Text, "Text", true));
            }
            for (tab, label, contextual) in tabs {
                let is_selected = self.tab == tab && !self.ribbon_minimized;
                let r = ribbon::tab(ui, label, is_selected, contextual);
                if r.clicked() {
                    self.tab = tab;
                    self.ribbon_minimized = false;
                }
                if is_selected {
                    selected = Some(r.rect);
                }
            }
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.add_space(6.0);
                if ribbon::icon_button(ui, Icon::Help, false, true, "About Paint").clicked() {
                    self.about_open = true;
                }
                let (icon, tip) =
                    if self.ribbon_minimized { (Icon::Dropdown, "Expand the Ribbon") } else { (Icon::Collapse, "Minimize the Ribbon") };
                if ribbon::icon_button(ui, icon, false, true, tip).clicked() {
                    self.ribbon_minimized = !self.ribbon_minimized;
                }
            });
        });
        // The line under the tabs, open beneath the selected one so it joins the ribbon.
        let y = strip.response.rect.max.y - 0.5;
        let stroke = Stroke::new(1.0, p.ribbon_border);
        match selected {
            Some(r) => {
                ui.painter().hline(egui::Rangef::new(full.min.x, r.min.x - ribbon::TAB_FLARE), y, stroke);
                ui.painter().hline(egui::Rangef::new(r.max.x + ribbon::TAB_FLARE, full.max.x), y, stroke);
            }
            None => _ = ui.painter().hline(full.x_range(), y, stroke),
        }
        if self.ribbon_minimized {
            return;
        }
        let (body, _) = ui.allocate_exact_size(vec2(full.width(), CONTENT_H + 26.0), Sense::hover());
        ui.painter().rect_filled(Rect::from_min_max(pos2(body.min.x, body.min.y - 0.5), body.max), 0.0, p.ribbon);
        if let Some(r) = selected {
            // Re-open the gap the fill just covered.
            ui.painter().hline(egui::Rangef::new(full.min.x, r.min.x - ribbon::TAB_FLARE), y, stroke);
            ui.painter().hline(egui::Rangef::new(r.max.x + ribbon::TAB_FLARE, full.max.x), y, stroke);
        }
        ui.painter().hline(full.x_range(), body.max.y - 0.5, stroke);
        let inner = Rect::from_min_max(body.min + vec2(4.0, 4.0), body.max - vec2(4.0, 2.0));
        ui.scope_builder(egui::UiBuilder::new().max_rect(inner).layout(Layout::left_to_right(Align::Min)), |ui| {
            ui.spacing_mut().item_spacing = spacing;
            ScrollArea::horizontal().id_salt("ribbon").scroll_bar_visibility(egui::scroll_area::ScrollBarVisibility::AlwaysHidden).show(ui, |ui| {
                ui.style_mut().wrap_mode = Some(egui::TextWrapMode::Extend);
                ui.horizontal_top(|ui| {
                    ui.spacing_mut().item_spacing.x = 0.0;
                    match self.tab {
                        Tab::Home => self.home_tab(ui),
                        Tab::View => self.view_tab(ui),
                        Tab::Together => self.together_tab(ui),
                        Tab::Text => self.text_tab(ui),
                    }
                });
            });
        });
    }

    fn home_tab(&mut self, ui: &mut Ui) {
        ribbon::group(ui, Some("Clipboard"), CONTENT_H, |ui| self.clipboard_group(ui));
        ribbon::group(ui, Some("Image"), CONTENT_H, |ui| self.image_group(ui));
        ribbon::group(ui, Some("Tools"), 46.0, |ui| self.tools_group(ui));
        ribbon::group(ui, None, CONTENT_H, |ui| self.brushes_group(ui));
        ribbon::group(ui, Some("Shapes"), CONTENT_H, |ui| self.shapes_group(ui));
        ribbon::group(ui, None, CONTENT_H, |ui| self.size_group(ui));
        ribbon::group(ui, Some("Colors"), CONTENT_H, |ui| self.colors_group(ui));
    }

    fn view_tab(&mut self, ui: &mut Ui) {
        ribbon::group(ui, Some("Zoom"), CONTENT_H, |ui| {
            if ribbon::big_button(ui, Icon::ZoomIn, "Zoom\nin", false, false, true).clicked() {
                self.zoom_step(true, None);
            }
            if ribbon::big_button(ui, Icon::ZoomOut, "Zoom\nout", false, false, true).clicked() {
                self.zoom_step(false, None);
            }
            if ribbon::big_button(ui, Icon::Zoom100, "100\n%", false, false, true).clicked() {
                self.zoom_at(1.0, None);
            }
        });
        ribbon::group(ui, Some("Show or hide"), CONTENT_H, |ui| {
            ui.vertical(|ui| {
                ui.spacing_mut().item_spacing.y = 0.0;
                ui.checkbox(&mut self.show_rulers, "Rulers");
                ui.checkbox(&mut self.show_grid, "Gridlines");
                ui.checkbox(&mut self.show_status, "Status bar");
            });
            ui.add_space(8.0);
        });
        ribbon::group(ui, Some("Display"), CONTENT_H, |ui| {
            if ribbon::big_button(ui, Icon::FullScreen, "Full\nscreen", false, false, true).on_hover_text("Full screen (F11)").clicked() {
                self.set_full_screen(true);
            }
            ui.vertical(|ui| {
                ui.add_space(4.0);
                ui.checkbox(&mut self.show_thumbnail, "Thumbnail");
            });
            ui.add_space(8.0);
        });
    }

    fn text_tab(&mut self, ui: &mut Ui) {
        ribbon::group(ui, Some("Clipboard"), CONTENT_H, |ui| self.clipboard_group(ui));
        ribbon::group(ui, Some("Font"), 52.0, |ui| {
            ui.vertical(|ui| {
                ui.spacing_mut().item_spacing.y = 6.0;
                ui.horizontal(|ui| {
                    let current = self.fonts.families.get(self.text_style.family).map_or("—", |f| f.name.as_str());
                    let mut family = self.text_style.family;
                    egui::ComboBox::from_id_salt("font").width(150.0).height(420.0).selected_text(current).show_ui(ui, |ui| {
                        for (i, fam) in self.fonts.families.iter().enumerate() {
                            ui.selectable_value(&mut family, i, &fam.name);
                        }
                    });
                    self.text_style.family = family;
                    egui::ComboBox::from_id_salt("font-size")
                        .width(46.0)
                        .selected_text(format!("{}", self.text_style.size_pt))
                        .show_ui(ui, |ui| {
                            for s in TEXT_SIZES {
                                ui.selectable_value(&mut self.text_style.size_pt, s, format!("{s}"));
                            }
                        });
                });
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 0.0;
                    let st = &mut self.text_style;
                    for (icon, on, tip) in [
                        (Icon::Bold, &mut st.bold, "Bold"),
                        (Icon::Italic, &mut st.italic, "Italic"),
                        (Icon::Underline, &mut st.underline, "Underline"),
                        (Icon::Strike, &mut st.strike, "Strikethrough"),
                    ] {
                        if ribbon::icon_button(ui, icon, *on, true, tip).clicked() {
                            *on = !*on;
                        }
                    }
                });
            });
            ui.add_space(6.0);
        });
        ribbon::group(ui, Some("Background"), 46.0, |ui| {
            ui.vertical(|ui| {
                ui.spacing_mut().item_spacing.y = 2.0;
                if ribbon::small_button(ui, Some(Icon::Opaque), "Opaque", false, self.text_style.opaque, true).clicked() {
                    self.text_style.opaque = true;
                }
                if ribbon::small_button(ui, Some(Icon::Transparent), "Transparent", false, !self.text_style.opaque, true).clicked() {
                    self.text_style.opaque = false;
                }
            });
        });
        ribbon::group(ui, Some("Colors"), CONTENT_H, |ui| self.colors_group(ui));
    }

    fn clipboard_group(&mut self, ui: &mut Ui) {
        let (paste, menu) = ribbon::big_split_button(ui, Icon::Paste, "Paste", false, true);
        if paste.on_hover_text(format!("Paste ({})", self.shortcut(Modifiers::COMMAND, Key::V))).clicked() {
            self.paste();
        }
        egui::Popup::menu(&menu).width(180.0).show(|ui| {
            if ribbon::menu_item(ui, Some(Icon::Paste), "Paste", false, true).clicked() {
                self.paste();
                ui.close();
            }
            if ribbon::menu_item(ui, Some(Icon::Open), "Paste from", false, true).clicked() {
                ui.close();
                self.paste_from();
            }
        });
        ui.vertical(|ui| {
            ui.spacing_mut().item_spacing.y = 0.0;
            let has_sel = self.sel.is_some();
            if ribbon::small_button(ui, Some(Icon::Cut), "Cut", false, false, has_sel).on_hover_text("Cut the selection").clicked() {
                self.cut();
            }
            if ribbon::small_button(ui, Some(Icon::Copy), "Copy", false, false, true).on_hover_text("Copy the selection").clicked() {
                self.copy();
            }
        });
    }

    fn image_group(&mut self, ui: &mut Ui) {
        let icon = if self.select_mode == Tool::FreeSelect { Icon::FreeSelect } else { Icon::Select };
        let (main, menu) = ribbon::big_split_button(ui, icon, "Select", self.tool.is_selection(), true);
        if main.on_hover_text("Select").clicked() {
            self.set_tool(self.select_mode);
        }
        egui::Popup::menu(&menu).width(230.0).close_behavior(PopupCloseBehavior::CloseOnClickOutside).show(|ui| {
            ribbon::menu_header(ui, "Selection shapes");
            for (tool, icon, label) in [(Tool::Select, Icon::Select, "Rectangular selection"), (Tool::FreeSelect, Icon::FreeSelect, "Free-form selection")] {
                if ribbon::menu_item(ui, Some(icon), label, self.select_mode == tool, true).clicked() {
                    self.select_mode = tool;
                    self.set_tool(tool);
                    ui.close();
                }
            }
            ribbon::menu_header(ui, "Selection options");
            if ribbon::menu_item(ui, Some(Icon::SelectAll), "Select all", false, true).clicked() {
                self.select_all();
                ui.close();
            }
            if ribbon::menu_item(ui, Some(Icon::Delete), "Delete", false, self.sel.is_some()).clicked() {
                self.delete_selection();
                ui.close();
            }
            if ribbon::menu_item(ui, None, "Transparent selection", self.transparent_selection, true).clicked() {
                self.transparent_selection = !self.transparent_selection;
                self.refresh_float();
            }
        });
        ui.vertical(|ui| {
            ui.spacing_mut().item_spacing.y = 0.0;
            let has_sel = self.sel.is_some();
            if ribbon::small_button(ui, Some(Icon::Crop), "Crop", false, false, has_sel).on_hover_text("Crop to the selection").clicked() {
                self.crop();
            }
            if ribbon::small_button(ui, Some(Icon::Resize), "Resize", false, false, true).on_hover_text("Resize and skew").clicked() {
                self.open_resize_dialog();
            }
            let rotate = ribbon::small_button(ui, Some(Icon::Rotate), "Rotate", true, false, true);
            egui::Popup::menu(&rotate).width(200.0).show(|ui| self.rotate_items(ui));
        });
    }

    fn rotate_items(&mut self, ui: &mut Ui) {
        let items: [(Icon, &str, fn(&RgbaImage) -> RgbaImage); 5] = [
            (Icon::Rotate, "Rotate right 90°", imageops::rotate90),
            (Icon::RotateLeft, "Rotate left 90°", imageops::rotate270),
            (Icon::Rotate180, "Rotate 180°", imageops::rotate180),
            (Icon::FlipVertical, "Flip vertical", imageops::flip_vertical),
            (Icon::FlipHorizontal, "Flip horizontal", imageops::flip_horizontal),
        ];
        for (icon, label, f) in items {
            if ribbon::menu_item(ui, Some(icon), label, false, true).clicked() {
                self.transform(f);
                ui.close();
            }
        }
    }

    fn tools_group(&mut self, ui: &mut Ui) {
        let tools = [
            (Tool::Pencil, Icon::Pencil, "Pencil"),
            (Tool::Fill, Icon::Fill, "Fill with color"),
            (Tool::Text, Icon::Text, "Text"),
            (Tool::Eraser, Icon::Eraser, "Eraser"),
            (Tool::Picker, Icon::Picker, "Color picker"),
            (Tool::Magnifier, Icon::Magnifier, "Magnifier"),
        ];
        egui::Grid::new("tools").spacing(vec2(0.0, 2.0)).min_col_width(0.0).min_row_height(0.0).show(ui, |ui| {
            for (i, (tool, icon, tip)) in tools.into_iter().enumerate() {
                if ribbon::icon_button(ui, icon, self.tool == tool, true, tip).clicked() {
                    self.set_tool(tool);
                }
                if i == 2 {
                    ui.end_row();
                }
            }
        });
        ui.add_space(4.0);
    }

    fn brushes_group(&mut self, ui: &mut Ui) {
        let p = Palette::of(ui);
        let (main, menu) = ribbon::big_split_button(ui, Icon::Brushes, "Brushes", self.tool == Tool::Brush, true);
        if main.on_hover_text(self.brush.name()).clicked() {
            self.set_tool(Tool::Brush);
        }
        egui::Popup::menu(&menu).width(3.0 * 52.0 + 8.0).show(|ui| {
            egui::Grid::new("brush-gallery").spacing(vec2(2.0, 2.0)).min_col_width(0.0).min_row_height(0.0).show(ui, |ui| {
                for (i, kind) in BrushKind::ALL.into_iter().enumerate() {
                    let (rect, resp) = ui.allocate_exact_size(Vec2::splat(50.0), Sense::click());
                    let checked = self.brush == kind;
                    let painter = ui.painter();
                    if checked || resp.hovered() {
                        let (fill, border) = if checked { (p.checked, p.checked_border) } else { (p.hover, p.hover_border) };
                        painter.rect_filled(rect, ribbon::HIGHLIGHT_RADIUS, fill);
                        painter.rect_stroke(rect, ribbon::HIGHLIGHT_RADIUS, Stroke::new(1.0, border), StrokeKind::Inside);
                    }
                    let uv = Rect::from_min_max(Pos2::ZERO, pos2(1.0, 1.0));
                    painter.image(self.brush_previews[i].id(), rect.shrink(5.0), uv, p.text);
                    if resp.on_hover_text(kind.name()).clicked() {
                        self.brush = kind;
                        self.set_tool(Tool::Brush);
                        ui.close();
                    }
                    if i % 3 == 2 {
                        ui.end_row();
                    }
                }
            });
        });
    }

    fn shapes_group(&mut self, ui: &mut Ui) {
        let p = Palette::of(ui);
        const CELL: f32 = 21.0;
        let (rect, _) = ui.allocate_exact_size(vec2(7.0 * CELL + 2.0 + 15.0, 3.0 * CELL + 2.0), Sense::hover());
        let painter = ui.painter().clone();
        painter.rect_filled(rect, 0.0, Color32::WHITE);
        for (i, kind) in ShapeKind::ALL.into_iter().enumerate() {
            let cell = Rect::from_min_size(rect.min + vec2(1.0 + (i % 7) as f32 * CELL, 1.0 + (i / 7) as f32 * CELL), Vec2::splat(CELL));
            let resp = ui.interact(cell, Id::new(("shape", i)), Sense::click());
            let checked = self.tool == Tool::Shape && self.shape == kind;
            if checked || resp.hovered() {
                let (fill, border) = if checked { (p.checked, p.checked_border) } else { (p.hover, p.hover_border) };
                painter.rect_filled(cell, ribbon::HIGHLIGHT_RADIUS, fill);
                painter.rect_stroke(cell, ribbon::HIGHLIGHT_RADIUS, Stroke::new(1.0, border), StrokeKind::Inside);
            }
            icons::paint(Icon::Shape(kind), &painter, icons::square(cell.center(), 16.0), true);
            if resp.on_hover_text(kind.name()).clicked() {
                self.pick_shape(kind);
            }
        }
        // Paint's gallery scroll strip.
        let strip = Rect::from_min_max(pos2(rect.max.x - 15.0, rect.min.y), rect.max);
        painter.rect_filled(strip, 0.0, Color32::from_rgb(0xF0, 0xF1, 0xF2));
        painter.vline(strip.min.x + 0.5, strip.y_range(), Stroke::new(1.0, p.ribbon_border));
        let third = strip.height() / 3.0;
        let mut more = None;
        for (k, icon) in [Icon::ScrollUp, Icon::ScrollDown, Icon::GalleryMore].into_iter().enumerate() {
            let b = Rect::from_min_size(strip.min + vec2(1.0, k as f32 * third), vec2(14.0, third));
            let enabled = icon == Icon::GalleryMore;
            let resp = ui.interact(b, Id::new(("shape-scroll", k)), if enabled { Sense::click() } else { Sense::hover() });
            if enabled && resp.hovered() {
                painter.rect_filled(b, ribbon::HIGHLIGHT_RADIUS, p.hover);
                painter.rect_stroke(b, ribbon::HIGHLIGHT_RADIUS, Stroke::new(1.0, p.hover_border), StrokeKind::Inside);
            }
            icons::paint(icon, &painter, icons::square(b.center(), 12.0), enabled);
            if enabled {
                more = Some(resp);
            }
        }
        painter.rect_stroke(rect, 0.0, Stroke::new(1.0, Color32::from_rgb(0xC5, 0xC5, 0xC5)), StrokeKind::Inside);
        if let Some(more) = more {
            egui::Popup::menu(&more).width(240.0).show(|ui| {
                for kind in ShapeKind::ALL {
                    if ribbon::menu_item(ui, Some(Icon::Shape(kind)), kind.name(), self.tool == Tool::Shape && self.shape == kind, true).clicked() {
                        self.pick_shape(kind);
                        ui.close();
                    }
                }
            });
        }
        ui.add_space(4.0);
        ui.vertical(|ui| {
            ui.spacing_mut().item_spacing.y = 0.0;
            // The icons show the current choice: the colour in use, or a red slash for none.
            let (line_color, fill_color) = (c32(self.primary), c32(self.secondary));
            let icon = Icon::Outline(self.shape_outline.then_some(line_color));
            let tip = if self.shape_outline { "Shape outline: solid Color 1" } else { "Shape outline: none" };
            let outline = ribbon::small_button(ui, Some(icon), "Outline", true, false, true).on_hover_text(tip);
            egui::Popup::menu(&outline).width(170.0).show(|ui| {
                for (value, label, icon) in [(false, "No outline", Icon::Outline(None)), (true, "Solid color", Icon::Outline(Some(line_color)))] {
                    if ribbon::menu_item(ui, Some(icon), label, self.shape_outline == value, true).clicked() {
                        self.shape_outline = value;
                        ui.close();
                    }
                }
            });
            let icon = Icon::FillShape(self.shape_fill.then_some(fill_color));
            let tip = if self.shape_fill { "Shape fill: solid Color 2" } else { "Shape fill: none" };
            let fill = ribbon::small_button(ui, Some(icon), "Fill", true, false, true).on_hover_text(tip);
            egui::Popup::menu(&fill).width(170.0).show(|ui| {
                for (value, label, icon) in [(false, "No fill", Icon::FillShape(None)), (true, "Solid color", Icon::FillShape(Some(fill_color)))] {
                    if ribbon::menu_item(ui, Some(icon), label, self.shape_fill == value, true).clicked() {
                        self.shape_fill = value;
                        ui.close();
                    }
                }
            });
        });
    }

    fn size_group(&mut self, ui: &mut Ui) {
        let p = Palette::of(ui);
        let sized = matches!(self.tool, Tool::Pencil | Tool::Brush | Tool::Eraser | Tool::Shape | Tool::Fill);
        let resp = ribbon::big_button(ui, Icon::Size, "Size", true, false, sized);
        egui::Popup::menu(&resp).width(200.0).close_behavior(PopupCloseBehavior::CloseOnClickOutside).show(|ui| {
            if self.tool == Tool::Fill {
                ribbon::menu_header(ui, "Fill tolerance");
                ui.add_space(4.0);
                ribbon::value_slider(ui, &mut self.fill_tolerance, 0.0, 100.0, false, "%");
                return;
            }
            let (size, presets, max): (&mut f32, &[f32], f32) = match self.tool {
                Tool::Pencil => (&mut self.size_pencil, &[1.0, 2.0, 3.0, 4.0], 20.0),
                Tool::Eraser => (&mut self.size_eraser, &[4.0, 6.0, 8.0, 10.0], 100.0),
                Tool::Shape => (&mut self.size_shape, &[1.0, 3.0, 5.0, 8.0], 50.0),
                _ => (&mut self.size_brush, &[1.0, 3.0, 5.0, 8.0], 100.0),
            };
            for &s in presets {
                let (rect, resp) = ui.allocate_exact_size(vec2(ui.available_width(), 30.0), Sense::click());
                let checked = (*size - s).abs() < 0.5;
                let painter = ui.painter();
                if checked || resp.hovered() {
                    let (fill, border) = if checked { (p.checked, p.checked_border) } else { (p.hover, p.hover_border) };
                    painter.rect_filled(rect.shrink(1.0), ribbon::HIGHLIGHT_RADIUS, fill);
                    painter.rect_stroke(rect.shrink(1.0), ribbon::HIGHLIGHT_RADIUS, Stroke::new(1.0, border), StrokeKind::Inside);
                }
                let y = rect.center().y;
                let r = Rect::from_min_max(pos2(rect.min.x + 10.0, y - s / 2.0), pos2(rect.max.x - 52.0, y + s / 2.0));
                painter.rect_filled(r, 0.0, p.text);
                painter.text(pos2(rect.max.x - 10.0, y), Align2::RIGHT_CENTER, format!("{s:.0}px"), FontId::proportional(11.5), p.text_weak);
                if resp.clicked() {
                    *size = s;
                    ui.close();
                }
            }
            ui.separator();
            ribbon::value_slider(ui, size, 1.0, max, true, " px");
        });
    }

    fn colors_group(&mut self, ui: &mut Ui) {
        for (slot, label, side) in [(false, "Color\n1", 30.0), (true, "Color\n2", 22.0)] {
            let c = c32(if slot { self.secondary } else { self.primary });
            let tip = if slot {
                "Color 2 (background color)\nClick here and then select a color from the palette. This color is used with the eraser and for shape fills."
            } else {
                "Color 1 (foreground color)\nClick here and then select a color from the palette. This color is used with the pencil and with brushes, as well as for shape outlines."
            };
            let resp = ribbon::big_button_with(ui, label, false, self.editing_secondary == slot, true, |painter, rect, _| {
                ribbon::paint_swatch(painter, Rect::from_center_size(rect.center(), Vec2::splat(side)), c, false);
            });
            if resp.on_hover_text(tip).clicked() {
                self.editing_secondary = slot;
            }
        }
        ui.add_space(3.0);
        ui.vertical(|ui| {
            ui.spacing_mut().item_spacing = Vec2::ZERO;
            for row in 0..3 {
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing = Vec2::ZERO;
                    for col in 0..10 {
                        let (color, name) = if row < 2 {
                            let ([r, g, b], name) = PALETTE[row * 10 + col];
                            (Some([r, g, b, 255]), name)
                        } else {
                            (self.custom_colors[col], "Custom color")
                        };
                        match color {
                            Some(c) => {
                                let resp = ribbon::swatch(ui, c32(c), 22.0).on_hover_text(name);
                                if resp.clicked() {
                                    self.set_active_color(c);
                                }
                                if resp.secondary_clicked() {
                                    self.secondary = c;
                                }
                            }
                            None => ribbon::empty_swatch(ui, 22.0),
                        }
                    }
                });
            }
        });
        ui.add_space(3.0);
        let edit = ribbon::big_button(ui, Icon::EditColors, "Edit\ncolors", false, false, true);
        if edit.on_hover_text("Edit colors\nSelect a color from the color palette.").clicked() {
            self.edit_colors = Some(rgb_to_hsl(self.active_color()));
        }
    }

    fn status_bar(&mut self, ui: &mut Ui) {
        ui.horizontal_centered(|ui| {
            ui.spacing_mut().item_spacing.x = 0.0;
            ui.add_space(4.0);
            let pos = self.cursor.map_or(String::new(), |(x, y)| format!("{x}, {y}px"));
            ribbon::status_item(ui, Icon::CursorPos, &pos, 130.0);
            let sel = match &self.op {
                Op::Selecting { start, end, .. } if self.tool == Tool::Select => {
                    let d = (*end - *start).abs();
                    Some((d.x.round() as i32, d.y.round() as i32))
                }
                _ => self.sel.as_ref().map(|s| (s.rect.width(), s.rect.height())),
            };
            let sel = sel.map_or(String::new(), |(w, h)| format!("{w} × {h}px"));
            ribbon::status_item(ui, Icon::SelectionSize, &sel, 130.0);
            let (w, h) = match &self.op {
                Op::ResizeCanvas { size, .. } => *size,
                _ => self.dims(),
            };
            ribbon::status_item(ui, Icon::CanvasSize, &format!("{w} × {h}px"), 150.0);
            match self.file_size() {
                Some(b) => ribbon::status_item(ui, Icon::FileSize, &format!("Size: {}", format_bytes(b)), 140.0),
                None => _ = ui.allocate_exact_size(vec2(140.0, 22.0), Sense::hover()),
            }
            // Drawing together: who's here, or a drawing nearby to join (shown left
            // of the zoom controls, when there's room).
            let invite = match (&self.link, &self.discovery) {
                (None, Some(d)) => d.nearby.iter().find(|n| n.compatible).cloned(),
                _ => None,
            };
            let together = self.session.as_ref().map(|s| match s.kids.len().saturating_sub(1) {
                0 => "Waiting for friends".to_owned(),
                1 => "Drawing with 1 friend".to_owned(),
                n => format!("Drawing with {n} friends"),
            });

            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.spacing_mut().item_spacing.x = 2.0;
                ui.add_space(8.0);
                if ribbon::status_button(ui, false).on_hover_text("Zoom in").clicked() {
                    self.zoom_step(true, None);
                }
                let (lo, hi) = (0.125f32.ln(), 8f32.ln());
                let mut t = ((self.zoom.ln() - lo) / (hi - lo)).clamp(0.0, 1.0);
                let tick = -lo / (hi - lo);
                if ribbon::zoom_slider(ui, &mut t, tick, 110.0).changed() {
                    self.zoom_at((lo + t * (hi - lo)).exp(), None);
                }
                if ribbon::status_button(ui, true).on_hover_text("Zoom out").clicked() {
                    self.zoom_step(false, None);
                }
                ui.add_sized(vec2(44.0, 20.0), egui::Label::new(format!("{:.0}%", self.zoom * 100.0)));
                ui.add_space(6.0);
                let fits = |ui: &Ui, text: &str| {
                    let w = ui.painter().layout_no_wrap(text.to_owned(), FontId::proportional(12.0), Color32::BLACK).size().x + 36.0;
                    (ui.available_width() >= w).then_some(w)
                };
                if let Some((text, w)) = together.and_then(|t| fits(ui, &t).map(|w| (t, w))) {
                    ribbon::status_item(ui, Icon::Together, &text, w);
                }
                if let Some(n) = invite {
                    let text = format!("Join {}'s drawing", n.name);
                    if fits(ui, &text).is_some()
                        && ribbon::status_link(ui, Icon::Together, &text).on_hover_text("A drawing nearby: draw on it together").clicked()
                    {
                        self.request(Pending::Join(n.addr.to_string()));
                    }
                }
            });
        });
    }

    /// The File tab's application menu.
    fn file_menu(&mut self, ctx: &Context) {
        if !self.file_menu_open {
            return;
        }
        let p = Palette::of_ctx(ctx);
        let anchor = self.file_tab_rect.left_bottom() - vec2(0.0, 1.0);
        let area = egui::Area::new(Id::new("file-menu")).fixed_pos(anchor).order(egui::Order::Foreground).show(ctx, |ui| {
            egui::Frame::new()
                .fill(p.card)
                .stroke(Stroke::new(1.0, Color32::from_rgb(0xA0, 0xA0, 0xA0)))
                .shadow(egui::Shadow { offset: [2, 4], blur: 10, spread: 0, color: Color32::from_black_alpha(55) })
                .show(ui, |ui| {
                    ui.spacing_mut().item_spacing = Vec2::ZERO;
                    ui.horizontal_top(|ui| {
                        ui.vertical(|ui| {
                            ui.set_width(210.0);
                            ui.add_space(4.0);
                            let mut formats = None;
                            let items = [
                                (Icon::New, "New", false),
                                (Icon::Open, "Open", false),
                                (Icon::Save, "Save", false),
                                (Icon::SaveAs, "Save as", true),
                                (Icon::Properties, "Properties", false),
                                (Icon::About, "About Paint", false),
                                (Icon::Exit, "Exit", false),
                            ];
                            for (i, (icon, label, arrow)) in items.into_iter().enumerate() {
                                if matches!(i, 4 | 5 | 6) {
                                    ui.add_space(3.0);
                                    let (r, _) = ui.allocate_exact_size(vec2(ui.available_width(), 1.0), Sense::hover());
                                    ui.painter().hline(r.x_range().shrink(8.0), r.center().y, Stroke::new(1.0, p.ribbon_border));
                                    ui.add_space(3.0);
                                }
                                let resp = ribbon::file_menu_item(ui, icon, label, arrow, arrow && self.file_menu_formats);
                                if resp.hovered() {
                                    formats = Some(arrow);
                                }
                                if resp.clicked() {
                                    self.file_menu_open = false;
                                    match label {
                                        "New" => self.request(Pending::New),
                                        "Open" => self.request(Pending::Open(None)),
                                        "Save" => _ = self.save(),
                                        "Save as" => _ = self.save_as(),
                                        "Properties" => self.open_properties(),
                                        "About Paint" => self.about_open = true,
                                        _ => self.ctx.send_viewport_cmd(ViewportCommand::Close),
                                    }
                                }
                            }
                            if let Some(f) = formats {
                                self.file_menu_formats = f;
                            }
                            ui.add_space(4.0);
                        });
                        let (sep, _) = ui.allocate_exact_size(vec2(1.0, 338.0), Sense::hover());
                        ui.painter().vline(sep.center().x, sep.y_range(), Stroke::new(1.0, p.ribbon_border));
                        ui.vertical(|ui| {
                            ui.set_width(300.0);
                            ui.set_min_height(338.0);
                            if self.file_menu_formats {
                                self.file_menu_formats_column(ui);
                            } else {
                                self.file_menu_recent_column(ui);
                            }
                        });
                    });
                });
        });
        let clicked_outside = ctx.input(|i| i.pointer.any_pressed())
            && !area.response.contains_pointer()
            && !ctx.pointer_hover_pos().is_some_and(|pos| self.file_tab_rect.contains(pos));
        if clicked_outside || ctx.input(|i| i.key_pressed(Key::Escape)) {
            self.file_menu_open = false;
        }
    }

    fn file_menu_recent_column(&mut self, ui: &mut Ui) {
        let p = Palette::of(ui);
        let (head, _) = ui.allocate_exact_size(vec2(ui.available_width(), 30.0), Sense::hover());
        ui.painter().text(head.left_center() + vec2(12.0, 0.0), Align2::LEFT_CENTER, "Recent pictures", FontId::proportional(13.0), p.text);
        ui.painter().hline(head.x_range().shrink(8.0), head.max.y - 0.5, Stroke::new(1.0, p.ribbon_border));
        let mut open = None;
        for (i, path) in self.recent.iter().enumerate() {
            let name = path.file_name().map_or_else(|| path.display().to_string(), |n| n.to_string_lossy().into_owned());
            let (rect, resp) = ui.allocate_exact_size(vec2(ui.available_width(), 24.0), Sense::click());
            if resp.hovered() {
                ui.painter().rect_filled(rect.shrink2(vec2(4.0, 1.0)), ribbon::HIGHLIGHT_RADIUS, p.hover);
                ui.painter().rect_stroke(rect.shrink2(vec2(4.0, 1.0)), ribbon::HIGHLIGHT_RADIUS, Stroke::new(1.0, p.hover_border), StrokeKind::Inside);
            }
            ui.painter().text(rect.left_center() + vec2(14.0, 0.0), Align2::LEFT_CENTER, format!("{}", i + 1), FontId::proportional(12.0), p.text);
            ui.painter().text(rect.left_center() + vec2(32.0, 0.0), Align2::LEFT_CENTER, name, FontId::proportional(12.5), p.text);
            if resp.on_hover_text(path.display().to_string()).clicked() {
                open = Some(path.clone());
            }
        }
        if self.recent.is_empty() {
            ui.add_space(8.0);
            ui.horizontal(|ui| {
                ui.add_space(14.0);
                ui.label(RichText::new("Pictures you open or save appear here.").color(p.text_weak));
            });
        }
        if let Some(path) = open {
            self.file_menu_open = false;
            self.request(Pending::Open(Some(path)));
        }
    }

    fn file_menu_formats_column(&mut self, ui: &mut Ui) {
        let p = Palette::of(ui);
        let (head, _) = ui.allocate_exact_size(vec2(ui.available_width(), 30.0), Sense::hover());
        ui.painter().text(head.left_center() + vec2(12.0, 0.0), Align2::LEFT_CENTER, "Save a copy of the document", FontId::proportional(13.0), p.text);
        ui.painter().hline(head.x_range().shrink(8.0), head.max.y - 0.5, Stroke::new(1.0, p.ribbon_border));
        let formats = [
            (FileKind::Png, "PNG picture", "Save a photo or drawing with high quality and use it on your computer or on the web.", Some("png")),
            (FileKind::Jpeg, "JPEG picture", "Save a photo with good quality and use it on your computer, in email, or on the web.", Some("jpg")),
            (FileKind::Bmp, "BMP picture", "Save any kind of picture with high quality and use it on your computer.", Some("bmp")),
            (FileKind::Gif, "GIF picture", "Save a simple drawing with lower quality and use it in email or on the web.", Some("gif")),
            (FileKind::Other, "Other formats", "Open the Save As dialog box to select from all possible file types.", None),
        ];
        for (kind, title, desc, ext) in formats {
            let (rect, resp) = ui.allocate_exact_size(vec2(ui.available_width(), 54.0), Sense::click());
            let painter = ui.painter();
            if resp.hovered() {
                painter.rect_filled(rect.shrink2(vec2(4.0, 1.0)), ribbon::HIGHLIGHT_RADIUS, p.hover);
                painter.rect_stroke(rect.shrink2(vec2(4.0, 1.0)), ribbon::HIGHLIGHT_RADIUS, Stroke::new(1.0, p.hover_border), StrokeKind::Inside);
            }
            icons::paint(Icon::FileType(kind), painter, icons::square(rect.left_center() + vec2(26.0, 0.0), 32.0), true);
            painter.text(rect.min + vec2(50.0, 8.0), Align2::LEFT_TOP, title, FontId::proportional(13.0), p.text);
            let galley = painter.layout(desc.to_owned(), FontId::proportional(11.0), p.text_weak, rect.width() - 62.0);
            painter.galley(rect.min + vec2(50.0, 25.0), galley, p.text_weak);
            if resp.clicked() {
                self.file_menu_open = false;
                self.save_as_format(ext);
            }
        }
    }

    fn canvas(&mut self, ui: &mut Ui) {
        let ctx = ui.ctx().clone();
        let full = ui.max_rect();
        let ruler = if self.show_rulers { RULER } else { 0.0 };
        let panel = Rect::from_min_max(full.min + Vec2::splat(ruler), full.max);
        if self.fit_pending && panel.width() > 64.0 {
            self.fit_pending = false;
            self.view_rect = panel;
            let (w, h) = self.dims();
            if w as f32 + 2.0 * MARGIN > panel.width() || h as f32 + 2.0 * MARGIN > panel.height() {
                self.zoom_fit();
            }
        }
        // Pinch or ⌘-scroll zooms around the pointer.
        let zoom_delta = ctx.input(|i| i.zoom_delta());
        if zoom_delta != 1.0 {
            if let Some(p) = ctx.pointer_hover_pos().filter(|p| panel.contains(*p)) {
                self.zoom_at(self.zoom * zoom_delta, Some(p - self.view_rect.min));
            }
        }

        let mut area = ScrollArea::both()
            .auto_shrink([false, false])
            .scroll_source(ScrollSource { drag: DragScroll::Never, ..Default::default() });
        if let Some(offset) = self.pending_scroll.take() {
            area = area.scroll_offset(offset);
        }
        let out = ui
            .scope_builder(egui::UiBuilder::new().max_rect(panel), |ui| {
                area.show(ui, |ui| {
                    let (w, h) = self.dims();
                    let size = vec2(w as f32, h as f32) * self.zoom;
                    let total = (size + Vec2::splat(MARGIN * 2.0 + 8.0)).max(ui.available_size());
                    let (resp, painter) = ui.allocate_painter(total, Sense::click_and_drag());
                    let crect = Rect::from_min_size(resp.rect.min + Vec2::splat(MARGIN), size);
                    self.canvas_rect = crect;
                    self.canvas_input(&ctx, &resp, crect);
                    self.render_live_shape();
                    self.sync_texture(&ctx);
                    self.paint_canvas(&painter, &resp, crect);
                    self.paint_friends(&painter, crect);
                    if let Some((icon, p)) = self.tool_cursor {
                        paint_tool_pointer(&painter, icon, p);
                    }
                    if self.tool.is_selection() && matches!(self.op, Op::Idle) {
                        resp.context_menu(|ui| self.selection_menu(ui));
                    }
                })
            })
            .inner;
        self.scroll_offset = out.state.offset;
        self.view_rect = out.inner_rect;
        if self.show_rulers {
            self.paint_rulers(ui.painter(), full, panel);
        }
    }

    /// Paint's right-click menu for the selection tools.
    fn selection_menu(&mut self, ui: &mut Ui) {
        ui.set_width(200.0);
        let has_sel = self.sel.is_some();
        let actions: [(Option<Icon>, &str, bool); 3] = [(Some(Icon::Cut), "Cut", has_sel), (Some(Icon::Copy), "Copy", true), (Some(Icon::Paste), "Paste", true)];
        for (icon, label, enabled) in actions {
            if ribbon::menu_item(ui, icon, label, false, enabled).clicked() {
                match label {
                    "Cut" => self.cut(),
                    "Copy" => self.copy(),
                    _ => self.paste(),
                }
                ui.close();
            }
        }
        ui.separator();
        if ribbon::menu_item(ui, Some(Icon::SelectAll), "Select all", false, true).clicked() {
            self.select_all();
            ui.close();
        }
        if ribbon::menu_item(ui, Some(Icon::Crop), "Crop", false, has_sel).clicked() {
            self.crop();
            ui.close();
        }
        if ribbon::menu_item(ui, Some(Icon::Delete), "Delete", false, has_sel).clicked() {
            self.delete_selection();
            ui.close();
        }
        ui.separator();
        ui.menu_button("      Rotate", |ui| {
            ui.set_width(200.0);
            self.rotate_items(ui);
        });
        if ribbon::menu_item(ui, Some(Icon::Resize), "Resize", false, true).clicked() {
            self.open_resize_dialog();
            ui.close();
        }
        if ribbon::menu_item(ui, None, "Invert color", false, true).clicked() {
            self.transform(raster::invert);
            ui.close();
        }
    }

    fn paint_rulers(&self, painter: &egui::Painter, full: Rect, view: Rect) {
        let p = Palette::of_ctx(&self.ctx);
        let top = Rect::from_min_max(pos2(view.min.x, full.min.y), pos2(view.max.x, view.min.y));
        let left = Rect::from_min_max(pos2(full.min.x, view.min.y), pos2(view.min.x, view.max.y));
        let edge = Stroke::new(1.0, Color32::from_rgb(0xA0, 0xA0, 0xA0));
        let tick = Stroke::new(1.0, Color32::from_rgb(0x6A, 0x6A, 0x6A));
        painter.rect_filled(top, 0.0, Color32::WHITE);
        painter.rect_filled(left, 0.0, Color32::WHITE);
        painter.rect_filled(Rect::from_min_max(full.min, view.min), 0.0, p.ribbon);
        painter.hline(top.x_range(), top.max.y - 0.5, edge);
        painter.vline(left.max.x - 0.5, left.y_range(), edge);

        let z = self.zoom;
        let major = [1.0, 2.0, 5.0, 10.0, 20.0, 25.0, 50.0, 100.0, 200.0, 250.0, 500.0, 1000.0, 2000.0, 5000.0f32]
            .into_iter()
            .find(|m| m * z >= 64.0)
            .unwrap_or(10_000.0);
        let minor = [major / 10.0, major / 5.0, major / 2.0, major].into_iter().find(|m| *m >= 1.0 && m * z >= 5.0).unwrap_or(major);
        let origin = self.canvas_rect.min;
        let font = FontId::proportional(9.5);
        let ticks = |lo: f32, hi: f32, o: f32| {
            let first = ((lo - o) / z / minor).floor() as i64;
            let last = ((hi - o) / z / minor).ceil() as i64;
            (first..=last).map(move |i| {
                let v = i as f32 * minor;
                let is_major = ((v / major).round() * major - v).abs() < 1e-3;
                let is_half = ((v / (major / 2.0)).round() * (major / 2.0) - v).abs() < 1e-3;
                (v, o + v * z, if is_major { RULER - 3.0 } else if is_half { 7.0 } else { 4.0 }, is_major)
            })
        };
        for (v, x, len, is_major) in ticks(top.min.x, top.max.x, origin.x) {
            if x < top.min.x || x > top.max.x {
                continue;
            }
            painter.vline(x.round() + 0.5, egui::Rangef::new(top.max.y - len, top.max.y), tick);
            if is_major {
                painter.text(pos2(x + 3.0, top.min.y + 1.0), Align2::LEFT_TOP, format!("{}", v as i64), font.clone(), p.text_weak);
            }
        }
        for (v, y, len, is_major) in ticks(left.min.y, left.max.y, origin.y) {
            if y < left.min.y || y > left.max.y {
                continue;
            }
            painter.hline(egui::Rangef::new(left.max.x - len, left.max.x), y.round() + 0.5, tick);
            if is_major {
                let galley = painter.layout_no_wrap(format!("{}", v as i64), font.clone(), p.text_weak);
                let shape = egui::epaint::TextShape::new(pos2(left.min.x + 1.0, y - 3.0), galley, p.text_weak).with_angle(-std::f32::consts::FRAC_PI_2);
                painter.add(shape);
            }
        }
        if let Some((cx, cy)) = self.cursor {
            let marker = Stroke::new(1.0, p.accent);
            painter.vline(origin.x + (cx as f32 + 0.5) * z, top.y_range(), marker);
            painter.hline(left.x_range(), origin.y + (cy as f32 + 0.5) * z, marker);
        }
    }

    fn paint_canvas(&self, painter: &egui::Painter, resp: &Response, crect: Rect) {
        let zoom = self.zoom;
        let to_screen = |p: Pos2| crect.min + p.to_vec2() * zoom;
        let pal = theme::Palette::of_ctx(&self.ctx);
        painter.add(egui::Shadow { offset: [0, 4], blur: 22, spread: 0, color: pal.canvas_shadow }.as_shape(crect, 0));
        let checker_uv = Rect::from_min_max(Pos2::ZERO, pos2(crect.width() / 16.0, crect.height() / 16.0));
        painter.image(self.checker.id(), crect, checker_uv, Color32::WHITE);
        if let Some(tex) = &self.tex {
            painter.image(tex.id(), crect, Rect::from_min_max(Pos2::ZERO, pos2(1.0, 1.0)), Color32::WHITE);
        }

        if self.show_grid {
            let vis = painter.clip_rect().intersect(crect);
            let gap = grid_step(zoom) as f32 * zoom;
            // One physical pixel wide and centred on one, so the lines stay sharp at any display scale.
            let ppp = self.ctx.pixels_per_point();
            let snap = |v: f32| ((v * ppp).floor() + 0.5) / ppp;
            let stroke = Stroke::new(1.0 / ppp, Color32::from_rgba_unmultiplied(128, 128, 128, 110));
            let first = |lo: f32, origin: f32| ((lo - origin) / gap).floor() as i32;
            let last = |hi: f32, origin: f32| ((hi - origin) / gap).ceil() as i32;
            for k in first(vis.min.x, crect.min.x)..=last(vis.max.x, crect.min.x) {
                painter.vline(snap(crect.min.x + k as f32 * gap), vis.y_range(), stroke);
            }
            for k in first(vis.min.y, crect.min.y)..=last(vis.max.y, crect.min.y) {
                painter.hline(vis.x_range(), snap(crect.min.y + k as f32 * gap), stroke);
            }
        }

        if let Some(sel) = &self.sel {
            let off = vec2(sel.rect.x0 as f32, sel.rect.y0 as f32);
            let pts = match &sel.lasso {
                Some(poly) => poly.iter().map(|p| to_screen(*p + off)).collect(),
                None => rect_points(to_screen(pos2(sel.rect.x0 as f32, sel.rect.y0 as f32)), to_screen(pos2(sel.rect.x1 as f32, sel.rect.y1 as f32))),
            };
            ants(painter, pts, true);
        }
        match &self.op {
            Op::Selecting { start, end, lasso, .. } => {
                if self.tool == Tool::FreeSelect {
                    ants(painter, lasso.iter().map(|p| to_screen(*p)).collect(), false);
                } else {
                    ants(painter, rect_points(to_screen(*start), to_screen(*end)), true);
                }
            }
            Op::ResizeCanvas { size, .. } => {
                let r = Rect::from_min_size(crect.min, vec2(size.0 as f32, size.1 as f32) * zoom);
                ants(painter, rect_points(r.min, r.max), true);
            }
            _ => {}
        }
        if let (Some(live), Tool::Shape) = (&self.live_shape, self.tool) {
            if !live.is_line() {
                let r = live.rect();
                ants(painter, rect_points(to_screen(r.min), to_screen(r.max)), true);
            }
            for (_, h) in live.handles() {
                let hr = Rect::from_center_size(to_screen(h), Vec2::splat(7.0));
                painter.rect_filled(hr, 0.0, Color32::WHITE);
                painter.rect_stroke(hr, 0.0, Stroke::new(1.0, Color32::from_rgb(0x55, 0x5A, 0x64)), StrokeKind::Inside);
            }
        }
        if let Some(tb) = &self.text {
            let r = Rect::from_min_max(to_screen(tb.bounds.min), to_screen(tb.bounds.max)).expand(3.0);
            if tb.editing {
                self.paint_caret(painter, tb, &to_screen, r);
            }
            ants(painter, rect_points(r.min, r.max), true);
            for c in [r.left_top(), r.right_top(), r.right_bottom(), r.left_bottom()] {
                let hr = Rect::from_center_size(c, Vec2::splat(7.0));
                painter.rect_filled(hr, 0.0, Color32::WHITE);
                painter.rect_stroke(hr, 0.0, Stroke::new(1.0, Color32::from_rgb(0x55, 0x5A, 0x64)), StrokeKind::Inside);
            }
        }

        let hovered_handle = resp.hover_pos().and_then(|hp| hit_handle(crect, hp));
        for (handle, r) in handle_rects(crect) {
            let active = hovered_handle == Some(handle) || matches!(&self.op, Op::ResizeCanvas { handle: h, .. } if *h == handle);
            painter.rect_filled(r, 2.0, Color32::WHITE);
            let border = if active { pal.accent } else { Color32::from_gray(110) };
            painter.rect_stroke(r, 2.0, Stroke::new(if active { 1.5 } else { 1.0 }, border), StrokeKind::Inside);
        }

        // Outline of the brush/eraser footprint under the pointer.
        let Some(hp) = resp.hover_pos().filter(|p| crect.contains(*p)) else { return };
        let (dark, light) = (Stroke::new(1.0, Color32::from_black_alpha(160)), Stroke::new(1.0, Color32::from_white_alpha(160)));
        match self.tool {
            Tool::Brush if self.size_brush * zoom >= 4.0 => {
                let r = self.size_brush * zoom * 0.5;
                painter.circle_stroke(hp, r, dark);
                painter.circle_stroke(hp, r + 1.0, light);
            }
            Tool::Eraser => {
                let s = (self.size_eraser * zoom).max(3.0);
                let r = Rect::from_center_size(hp, Vec2::splat(s));
                painter.rect_filled(r, 0.0, c32(self.secondary));
                painter.rect_stroke(r, 0.0, dark, StrokeKind::Outside);
            }
            _ => {}
        }
    }

    fn dialogs(&mut self, ctx: &Context) {
        self.together_dialogs(ctx);
        if let Some(action) = self.confirm.clone() {
            let name = self.doc_name();
            let mut choice = None;
            let (_, dismissed) = dialog(ctx, "confirm", "Paint", 380.0, |ui| {
                ui.label(RichText::new(format!("Do you want to save changes to {name}?")).size(15.0).color(Color32::from_rgb(0x00, 0x33, 0x99)));
                ui.add_space(16.0);
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if ui.add(theme::dialog_button("Cancel", false)).clicked() {
                        choice = Some(2);
                    }
                    if ui.add(theme::dialog_button("Don't Save", false)).clicked() {
                        choice = Some(1);
                    }
                    if ui.add(theme::dialog_button("Save", true)).clicked() {
                        choice = Some(0);
                    }
                });
            });
            if dismissed && choice.is_none() {
                choice = Some(2);
            }
            match choice {
                Some(0) => {
                    self.confirm = None;
                    if self.save() {
                        self.perform(action);
                    }
                }
                Some(1) => {
                    self.confirm = None;
                    self.perform(action);
                }
                Some(_) => self.confirm = None,
                None => {}
            }
        }

        if let Some(d) = &mut self.resize_dialog {
            let mut apply = None;
            let mut close = false;
            let pal = Palette::of_ctx(ctx);
            let (_, dismissed) = dialog(ctx, "resize", "Resize and Skew", 320.0, |ui| {
                ui.label(RichText::new(if self.sel.is_some() { "Resize the selection" } else { "Resize the whole image" }).color(pal.text_weak));
                ui.add_space(8.0);
                ui.horizontal(|ui| {
                    ui.label("By:");
                    if ui.radio_value(&mut d.percent, true, "Percentage").clicked() {
                        (d.w, d.h) = (100.0, 100.0);
                    }
                    if ui.radio_value(&mut d.percent, false, "Pixels").clicked() {
                        (d.w, d.h) = (d.base.0 as f32, d.base.1 as f32);
                    }
                });
                let aspect = d.base.1 as f32 / d.base.0.max(1) as f32;
                let max = if d.percent { 1000.0 } else { 30000.0 };
                egui::Grid::new("resize-grid").num_columns(2).spacing(vec2(12.0, 6.0)).show(ui, |ui| {
                    ui.label("Horizontal:");
                    let hw = ui.add(egui::DragValue::new(&mut d.w).range(1.0..=max).speed(1.0).max_decimals(0));
                    ui.end_row();
                    ui.label("Vertical:");
                    let hv = ui.add(egui::DragValue::new(&mut d.h).range(1.0..=max).speed(1.0).max_decimals(0));
                    ui.end_row();
                    if d.keep_aspect {
                        if hw.changed() {
                            d.h = if d.percent { d.w } else { (d.w * aspect).round().max(1.0) };
                        }
                        if hv.changed() {
                            d.w = if d.percent { d.h } else { (d.h / aspect).round().max(1.0) };
                        }
                    }
                });
                ui.checkbox(&mut d.keep_aspect, "Maintain aspect ratio");
                ui.add_space(14.0);
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if ui.add(theme::dialog_button("Cancel", false)).clicked() {
                        close = true;
                    }
                    if ui.add(theme::dialog_button("OK", true)).clicked() {
                        let (w, h) = if d.percent {
                            (d.base.0 as f32 * d.w / 100.0, d.base.1 as f32 * d.h / 100.0)
                        } else {
                            (d.w, d.h)
                        };
                        apply = Some((w.round().max(1.0) as u32, h.round().max(1.0) as u32));
                    }
                });
            });
            if dismissed {
                close = true;
            }
            if let Some((w, h)) = apply {
                self.resize_dialog = None;
                self.transform(move |img| imageops::resize(img, w, h, imageops::FilterType::Triangle));
            } else if close {
                self.resize_dialog = None;
            }
        }

        if let Some((mut w, mut h)) = self.properties {
            let mut result = None;
            let meta = self.file_meta;
            let (_, dismissed) = dialog(ctx, "properties", "Image Properties", 380.0, |ui| {
                group_box(ui, "File attributes", 344.0, |ui| {
                    egui::Grid::new("attrs").num_columns(2).spacing(vec2(16.0, 4.0)).show(ui, |ui| {
                        ui.label("Last saved:");
                        ui.label(meta.and_then(|(_, t)| t).map_or("Not available".to_owned(), format_time));
                        ui.end_row();
                        ui.label("Size on disk:");
                        ui.label(meta.map_or("Not available".to_owned(), |(b, _)| format_bytes(b)));
                        ui.end_row();
                        ui.label("Resolution:");
                        ui.label("96 DPI");
                        ui.end_row();
                    });
                });
                ui.horizontal_top(|ui| {
                    group_box(ui, "Units", 169.0, |ui| {
                        ui.add_enabled(false, egui::RadioButton::new(false, "Inches"));
                        ui.add_enabled(false, egui::RadioButton::new(false, "Centimeters"));
                        let _ = ui.radio(true, "Pixels");
                    });
                    group_box(ui, "Colors", 169.0, |ui| {
                        ui.add_enabled(false, egui::RadioButton::new(false, "Black and white"));
                        let _ = ui.radio(true, "Color");
                    });
                });
                ui.horizontal(|ui| {
                    ui.label("Width:");
                    ui.add(egui::DragValue::new(&mut w).range(1.0..=30000.0).speed(1.0).max_decimals(0));
                    ui.add_space(12.0);
                    ui.label("Height:");
                    ui.add(egui::DragValue::new(&mut h).range(1.0..=30000.0).speed(1.0).max_decimals(0));
                });
                ui.add_space(12.0);
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if ui.add(theme::dialog_button("Cancel", false)).clicked() {
                        result = Some(false);
                    }
                    if ui.add(theme::dialog_button("OK", true)).clicked() {
                        result = Some(true);
                    }
                });
            });
            if dismissed && result.is_none() {
                result = Some(false);
            }
            match result {
                Some(ok) => {
                    self.properties = None;
                    let (nw, nh) = (w.round().max(1.0) as u32, h.round().max(1.0) as u32);
                    if ok && (nw, nh) != self.dims() {
                        self.commit_all();
                        let before = self.img.clone();
                        self.img = raster::resize_canvas(&before, nw, nh, self.secondary);
                        self.record(Change::Replace { before, after: self.img.clone() });
                        self.tex_full = true;
                    }
                }
                None => self.properties = Some((w, h)),
            }
        }

        if self.about_open {
            let (inner, dismissed) = dialog(ctx, "about", "About Paint", 380.0, |ui| {
                ui.horizontal_top(|ui| {
                    let (r, _) = ui.allocate_exact_size(Vec2::splat(64.0), Sense::hover());
                    paint_logo(ui.painter(), r);
                    ui.vertical(|ui| {
                        ui.label(RichText::new("Paint").size(22.0).color(Color32::from_rgb(0x00, 0x33, 0x99)));
                        ui.label(format!("Version {}", env!("CARGO_PKG_VERSION")));
                        ui.add_space(6.0);
                        ui.label("A from-scratch reimplementation of Microsoft Paint, written in Rust.");
                    });
                });
                ui.add_space(12.0);
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| ui.add(theme::dialog_button("OK", true)).clicked()).inner
            });
            if inner || dismissed {
                self.about_open = false;
            }
        }

        if let Some(mut hsl) = self.edit_colors {
            let mut result = None;
            let (_, dismissed) = dialog(ctx, "edit-colors", "Edit Colors", 520.0, |ui| {
                ui.horizontal_top(|ui| {
                    ui.vertical(|ui| {
                        ui.label("Basic colors:");
                        for row in BASIC_COLORS.chunks(8) {
                            ui.horizontal(|ui| {
                                ui.spacing_mut().item_spacing = vec2(2.0, 0.0);
                                for &rgb in row {
                                    let c = [(rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8, 255];
                                    if ribbon::swatch(ui, c32(c), 22.0).clicked() {
                                        hsl = rgb_to_hsl(c);
                                    }
                                }
                            });
                        }
                        ui.add_space(8.0);
                        ui.label("Custom colors:");
                        for row in 0..2 {
                            ui.horizontal(|ui| {
                                ui.spacing_mut().item_spacing = vec2(2.0, 0.0);
                                for col in 0..5 {
                                    match self.custom_colors[row * 5 + col] {
                                        Some(c) => {
                                            if ribbon::swatch(ui, c32(c), 22.0).clicked() {
                                                hsl = rgb_to_hsl(c);
                                            }
                                        }
                                        None => ribbon::empty_swatch(ui, 22.0),
                                    }
                                }
                            });
                        }
                    });
                    ui.add_space(10.0);
                    ui.vertical(|ui| {
                        ui.horizontal_top(|ui| {
                            spectrum(ui, &mut hsl);
                            ui.add_space(4.0);
                            luminance_bar(ui, &mut hsl);
                        });
                        ui.add_space(8.0);
                        ui.horizontal_top(|ui| {
                            ui.vertical(|ui| {
                                let (r, _) = ui.allocate_exact_size(vec2(66.0, 42.0), Sense::hover());
                                ribbon::paint_swatch(ui.painter(), r, c32(hsl_to_rgb(hsl)), false);
                                ui.label("Color|Solid");
                            });
                            ui.add_space(8.0);
                            color_fields(ui, &mut hsl);
                        });
                        ui.add_space(6.0);
                        if ui.add(theme::dialog_button("Add to Custom Colors", false).min_size(vec2(220.0, 24.0))).clicked() {
                            let c = hsl_to_rgb(hsl);
                            if !self.custom_colors.contains(&Some(c)) {
                                self.custom_colors.rotate_right(1);
                                self.custom_colors[0] = Some(c);
                            }
                        }
                    });
                });
                ui.add_space(12.0);
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if ui.add(theme::dialog_button("Cancel", false)).clicked() {
                        result = Some(false);
                    }
                    if ui.add(theme::dialog_button("OK", true)).clicked() {
                        result = Some(true);
                    }
                });
            });
            if dismissed && result.is_none() {
                result = Some(false);
            }
            match result {
                Some(true) => {
                    self.set_active_color(hsl_to_rgb(hsl));
                    self.edit_colors = None;
                }
                Some(false) => self.edit_colors = None,
                None => self.edit_colors = Some(hsl),
            }
        }

        if let Some(msg) = self.error.clone() {
            let pal = Palette::of_ctx(ctx);
            let (inner, dismissed) = dialog(ctx, "error", "Paint", 400.0, |ui| {
                ui.label(RichText::new(msg).color(pal.text));
                ui.add_space(14.0);
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| ui.add(theme::dialog_button("OK", true)).clicked()).inner
            });
            if inner || dismissed {
                self.error = None;
            }
        }
    }

    /// View > Full screen: just the picture; any click or Esc returns.
    fn full_screen_view(&mut self, ui: &mut Ui) {
        let ctx = ui.ctx().clone();
        self.sync_texture(&ctx);
        egui::CentralPanel::default().frame(egui::Frame::new().fill(Palette::of(ui).workspace)).show(ui, |ui| {
            let area = ui.max_rect();
            let (w, h) = self.dims();
            let scale = (area.width() / w as f32).min(area.height() / h as f32).min(1.0);
            let r = Rect::from_center_size(area.center(), vec2(w as f32, h as f32) * scale);
            if let Some(tex) = &self.tex {
                ui.painter().image(tex.id(), r, Rect::from_min_max(Pos2::ZERO, pos2(1.0, 1.0)), Color32::WHITE);
            }
            if ui.interact(area, Id::new("full-screen"), Sense::click()).clicked() {
                self.set_full_screen(false);
            }
        });
    }

    /// View > Thumbnail: a small window showing the whole picture.
    fn thumbnail(&mut self, ctx: &Context) {
        if !self.show_thumbnail {
            return;
        }
        let mut open = true;
        egui::Window::new("Thumbnail")
            .open(&mut open)
            .default_size([240.0, 170.0])
            .resizable(true)
            .collapsible(false)
            .show(ctx, |ui| {
                let avail = ui.available_size().max(vec2(40.0, 30.0));
                let (rect, _) = ui.allocate_exact_size(avail, Sense::hover());
                let (w, h) = self.dims();
                let scale = (avail.x / w as f32).min(avail.y / h as f32);
                let r = Rect::from_center_size(rect.center(), vec2(w as f32, h as f32) * scale);
                if let Some(tex) = &self.tex {
                    ui.painter().image(tex.id(), r, Rect::from_min_max(Pos2::ZERO, pos2(1.0, 1.0)), Color32::WHITE);
                }
                ui.painter().rect_stroke(r, 0.0, Stroke::new(1.0, Color32::from_gray(160)), StrokeKind::Outside);
            });
        self.show_thumbnail = open;
    }

    fn update_title(&mut self, ctx: &Context) {
        let together = if self.session.is_some() { " (drawing together)" } else { "" };
        let title = format!("{}{} - Paint{together}", if self.is_dirty() { "*" } else { "" }, self.doc_name());
        if title != self.title {
            ctx.send_viewport_cmd(ViewportCommand::Title(title.clone()));
            self.title = title;
        }
    }
}

impl eframe::App for PaintApp {
    fn ui(&mut self, ui: &mut Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        if !self.focused_once {
            // Launched from a terminal, macOS would otherwise leave the window in the background.
            self.focused_once = true;
            ctx.send_viewport_cmd(ViewportCommand::Focus);
        }
        if ctx.input(|i| i.viewport().close_requested()) && !self.allow_close && self.is_dirty() {
            ctx.send_viewport_cmd(ViewportCommand::CancelClose);
            self.confirm = Some(Pending::Quit);
        }
        let dropped = ctx.input(|i| i.raw.dropped_files.first().map(|f| f.path().to_path_buf()));
        if let Some(path) = dropped {
            self.request(Pending::Open(Some(path)));
        }
        self.pump_network();
        self.handle_keys(&ctx);

        let p = Palette::of(ui);
        if self.full_screen {
            self.full_screen_view(ui);
            self.dialogs(&ctx);
            self.flush_network();
            return;
        }
        egui::Panel::top("titlebar")
            .exact_size(30.0)
            .show_separator_line(false)
            .frame(egui::Frame::new().fill(p.chrome).inner_margin(egui::Margin::symmetric(6, 0)))
            .show(ui, |ui| self.title_bar(ui));
        egui::Panel::top("ribbon")
            .show_separator_line(false)
            .frame(egui::Frame::new().fill(p.chrome))
            .show(ui, |ui| self.ribbon(ui));
        if self.show_status {
            egui::Panel::bottom("status")
                .exact_size(24.0)
                .frame(egui::Frame::new().fill(p.status).inner_margin(egui::Margin::symmetric(4, 0)))
                .show(ui, |ui| self.status_bar(ui));
        }
        egui::CentralPanel::default().frame(egui::Frame::new().fill(p.workspace)).show(ui, |ui| self.canvas(ui));

        self.file_menu(&ctx);
        self.thumbnail(&ctx);
        self.update_text_preview();
        self.dialogs(&ctx);
        let lift = self.door_notices(&ctx);
        self.paint_toasts(&ctx, lift);
        self.update_title(&ctx);
        self.flush_network();
        if self.tex_full || !self.tex_dirty.is_empty() || !matches!(self.op, Op::Idle) {
            ctx.request_repaint();
        }
    }
}

// -------------------------------------------------------------------- helpers

fn c32(c: Rgba8) -> Color32 {
    Color32::from_rgba_unmultiplied(c[0], c[1], c[2], c[3])
}

fn rgb(c: Rgba8) -> [u8; 3] {
    [c[0], c[1], c[2]]
}

fn clamp_to(p: Pos2, w: u32, h: u32) -> Pos2 {
    pos2(p.x.clamp(0.0, w as f32), p.y.clamp(0.0, h as f32))
}

fn lasso_mask(poly: &[Pos2], w: u32, h: u32) -> Mask {
    let mut m = Mask::new(w, h);
    m.fill_polygon(poly, 1.0);
    m
}

/// The text box's corner handles (in canvas pixels), sitting just outside its bounds.
fn text_handle_at(bounds: Rect, p: Pos2, tol: f32, zoom: f32) -> Option<usize> {
    let r = bounds.expand(3.0 / zoom);
    [r.left_top(), r.right_top(), r.right_bottom(), r.left_bottom()].iter().position(|c| (p - *c).abs().max_elem() <= tol)
}

/// The pixels of `r` in `img`, with friends' drawing-in-progress (`overlay`,
/// transparent where there is none) on top.
fn with_friends(img: &RgbaImage, overlay: Option<&RgbaImage>, r: IRect) -> ColorImage {
    let (w, rw) = (img.width() as usize, r.width() as usize);
    let (raw, over) = (img.as_raw(), overlay.filter(|o| o.dimensions() == img.dimensions()).map(|o| o.as_raw()));
    let color = |p: &[u8]| Color32::from_rgba_unmultiplied(p[0], p[1], p[2], p[3]);
    // Straight into the texture's pixels: a big picture is copied once on its way to the screen, not twice.
    let mut pixels = Vec::with_capacity(rw * r.height() as usize);
    for y in r.y0 as usize..r.y1 as usize {
        let row = (y * w + r.x0 as usize) * 4..(y * w + r.x1 as usize) * 4;
        match over {
            None => pixels.extend(raw[row].chunks_exact(4).map(color)),
            Some(over) => pixels.extend(
                raw[row.clone()].chunks_exact(4).zip(over[row].chunks_exact(4)).map(|(mine, theirs)| color(if theirs[3] > 0 { theirs } else { mine })),
            ),
        }
    }
    ColorImage::new([rw, r.height() as usize], pixels)
}

/// Tools whose mouse pointer is their own icon, as in Paint.
fn tool_pointer(tool: Tool) -> Option<Icon> {
    match tool {
        Tool::Pencil => Some(Icon::Pencil),
        Tool::Fill => Some(Icon::Fill),
        Tool::Brush => Some(Icon::Brushes),
        Tool::Picker => Some(Icon::Picker),
        _ => None,
    }
}

/// Draws a tool pointer so the tool's tip (where it paints) sits on `p`.
fn paint_tool_pointer(painter: &egui::Painter, icon: Icon, p: Pos2) {
    const SIZE: f32 = 24.0;
    // The tip on the icon's 20×20 grid.
    let tip = match icon {
        Icon::Pencil | Icon::Picker => vec2(2.5, 17.5),
        Icon::Brushes => vec2(3.0, 18.4),
        _ => vec2(17.9, 18.6), // the drop of paint pouring from the bucket
    };
    let min = p - tip * (SIZE / 20.0);
    icons::paint_halo(icon, painter, Rect::from_min_size(min, Vec2::splat(SIZE)));
}

/// Image pixels between gridlines: every pixel from 400% up, as in Paint; below
/// that every 8 pixels (so the grid grows as you zoom in), or a larger power of
/// two when zoomed out far enough that lines would be under 8 points apart.
fn grid_step(zoom: f32) -> u32 {
    if zoom >= 4.0 {
        return 1;
    }
    let mut step = 8;
    while (step as f32) * zoom < 8.0 {
        step *= 2;
    }
    step
}

fn text_handle_cursor(h: usize) -> CursorIcon {
    if h % 2 == 0 { CursorIcon::ResizeNwSe } else { CursorIcon::ResizeNeSw }
}

fn handle_rects(crect: Rect) -> [(Handle, Rect); 3] {
    let s = Vec2::splat(7.0);
    [
        (Handle::Right, Rect::from_center_size(pos2(crect.max.x + 4.0, crect.center().y), s)),
        (Handle::Bottom, Rect::from_center_size(pos2(crect.center().x, crect.max.y + 4.0), s)),
        (Handle::Corner, Rect::from_center_size(crect.max + vec2(4.0, 4.0), s)),
    ]
}

fn hit_handle(crect: Rect, p: Pos2) -> Option<Handle> {
    handle_rects(crect).into_iter().find(|(_, r)| r.expand(3.0).contains(p)).map(|(h, _)| h)
}

fn handle_cursor(h: Handle) -> CursorIcon {
    match h {
        Handle::Right => CursorIcon::ResizeHorizontal,
        Handle::Bottom => CursorIcon::ResizeVertical,
        Handle::Corner => CursorIcon::ResizeNwSe,
    }
}

fn rect_points(a: Pos2, b: Pos2) -> Vec<Pos2> {
    let r = Rect::from_two_pos(a, b);
    vec![r.left_top(), r.right_top(), r.right_bottom(), r.left_bottom()]
}

fn closed(mut pts: Vec<Pos2>) -> Vec<Pos2> {
    if let Some(&first) = pts.first() {
        pts.push(first);
    }
    pts
}

/// Paint-style "marching ants" outline.
fn ants(painter: &egui::Painter, pts: Vec<Pos2>, close: bool) {
    if pts.len() < 2 {
        return;
    }
    let pts = if close { closed(pts) } else { pts };
    painter.add(Shape::line(pts.clone(), Stroke::new(1.0, Color32::WHITE)));
    painter.extend(Shape::dashed_line(&pts, Stroke::new(1.0, Color32::from_gray(20)), 4.0, 4.0));
}

/// The basic colours of the Windows colour dialog.
const BASIC_COLORS: [u32; 48] = [
    0xFF8080, 0xFFFF80, 0x80FF80, 0x00FF80, 0x80FFFF, 0x0080FF, 0xFF80C0, 0xFF80FF, //
    0xFF0000, 0xFFFF00, 0x80FF00, 0x00FF40, 0x00FFFF, 0x0080C0, 0x8080C0, 0xFF00FF, //
    0x804040, 0xFF8040, 0x00FF00, 0x008080, 0x004080, 0x8080FF, 0x800040, 0xFF0080, //
    0x800000, 0xFF8000, 0x008000, 0x008040, 0x0000FF, 0x0000A0, 0x800080, 0x8000FF, //
    0x400000, 0x804000, 0x004000, 0x004040, 0x000080, 0x000040, 0x400040, 0x400080, //
    0x000000, 0x808000, 0x808040, 0x808080, 0x408080, 0xC0C0C0, 0x400040, 0xFFFFFF,
];

/// A Windows-style dialog: white title bar with a close button over a padded grey body.
/// Returns the body's result and whether the dialog was dismissed (✕, Esc or a click outside).
fn dialog<R>(ctx: &Context, id: &str, title: &str, width: f32, body: impl FnOnce(&mut Ui) -> R) -> (R, bool) {
    let mut closed = false;
    let frame = egui::Frame::new()
        .fill(Color32::from_rgb(0xF0, 0xF0, 0xF0))
        .stroke(Stroke::new(1.0, Color32::from_rgb(0x88, 0x88, 0x88)))
        .shadow(egui::Shadow { offset: [0, 8], blur: 28, spread: 0, color: Color32::from_black_alpha(70) });
    let modal = egui::Modal::new(Id::new(id)).backdrop_color(BACKDROP).frame(frame).show(ctx, |ui| {
        ui.spacing_mut().item_spacing = Vec2::ZERO;
        let (bar, _) = ui.allocate_exact_size(vec2(width, 32.0), Sense::hover());
        let inner = egui::Frame::new()
            .inner_margin(egui::Margin { left: 18, right: 18, top: 14, bottom: 16 })
            .show(ui, |ui| {
                ui.spacing_mut().item_spacing = vec2(6.0, 5.0);
                ui.set_width(width - 36.0);
                body(ui)
            })
            .inner;
        // The body may have grown wider than asked; the title bar follows it.
        let bar = Rect::from_min_max(bar.min, pos2(ui.min_rect().max.x, bar.max.y));
        let painter = ui.painter();
        painter.rect_filled(bar, 0.0, Color32::WHITE);
        painter.text(bar.left_center() + vec2(12.0, 0.0), Align2::LEFT_CENTER, title, FontId::proportional(12.5), Color32::BLACK);
        let close_rect = Rect::from_min_max(pos2(bar.max.x - 46.0, bar.min.y), bar.max);
        let close = ui.interact(close_rect, Id::new((id, "close")), Sense::click());
        let x_color = if close.hovered() {
            ui.painter().rect_filled(close_rect, 0.0, Color32::from_rgb(0xE8, 0x11, 0x23));
            Color32::WHITE
        } else {
            Color32::BLACK
        };
        let c = close_rect.center();
        for d in [vec2(5.0, 5.0), vec2(5.0, -5.0)] {
            ui.painter().line_segment([c - d, c + d], Stroke::new(1.0, x_color));
        }
        closed = close.clicked();
        inner
    });
    let dismissed = closed || modal.should_close();
    (modal.inner, dismissed)
}

/// A Windows group box: a caption over a framed area.
fn group_box(ui: &mut Ui, title: &str, width: f32, add: impl FnOnce(&mut Ui)) {
    ui.vertical(|ui| {
        ui.label(RichText::new(title).color(Color32::from_rgb(0x00, 0x33, 0x99)));
        egui::Frame::new().stroke(Stroke::new(1.0, Color32::from_rgb(0xDC, 0xDC, 0xDC))).inner_margin(8).show(ui, |ui| {
            ui.set_width(width - 18.0);
            add(ui);
        });
    });
}

const BACKDROP: Color32 = Color32::from_black_alpha(28);

fn hsl_to_rgb([h, s, l]: [f32; 3]) -> Rgba8 {
    let c = (1.0 - (2.0 * l - 1.0).abs()) * s;
    let hp = (h.rem_euclid(1.0)) * 6.0;
    let x = c * (1.0 - (hp % 2.0 - 1.0).abs());
    let (r, g, b) = match hp as u32 {
        0 => (c, x, 0.0),
        1 => (x, c, 0.0),
        2 => (0.0, c, x),
        3 => (0.0, x, c),
        4 => (x, 0.0, c),
        _ => (c, 0.0, x),
    };
    let m = l - c / 2.0;
    let to = |v: f32| ((v + m) * 255.0).round().clamp(0.0, 255.0) as u8;
    [to(r), to(g), to(b), 255]
}

fn rgb_to_hsl(c: Rgba8) -> [f32; 3] {
    let (r, g, b) = (c[0] as f32 / 255.0, c[1] as f32 / 255.0, c[2] as f32 / 255.0);
    let (max, min) = (r.max(g).max(b), r.min(g).min(b));
    let l = (max + min) / 2.0;
    let d = max - min;
    if d == 0.0 {
        return [0.0, 0.0, l];
    }
    let s = d / (1.0 - (2.0 * l - 1.0).abs());
    let h = if max == r {
        ((g - b) / d).rem_euclid(6.0)
    } else if max == g {
        (b - r) / d + 2.0
    } else {
        (r - g) / d + 4.0
    } / 6.0;
    [h, s.clamp(0.0, 1.0), l]
}

/// The Windows colour dialog's spectrum: hue across, saturation up, at mid luminance.
fn spectrum(ui: &mut Ui, hsl: &mut [f32; 3]) {
    let (rect, resp) = ui.allocate_exact_size(vec2(180.0, 170.0), Sense::click_and_drag());
    if let Some(pos) = resp.interact_pointer_pos() {
        hsl[0] = ((pos.x - rect.min.x) / rect.width()).clamp(0.0, 1.0);
        hsl[1] = (1.0 - (pos.y - rect.min.y) / rect.height()).clamp(0.0, 1.0);
        if hsl[2] <= 0.0 || hsl[2] >= 1.0 {
            hsl[2] = 0.5;
        }
    }
    let (nx, ny) = (36u32, 8u32);
    let mut mesh = egui::Mesh::default();
    for j in 0..=ny {
        for i in 0..=nx {
            let (u, v) = (i as f32 / nx as f32, j as f32 / ny as f32);
            let pos = pos2(rect.min.x + u * rect.width(), rect.min.y + v * rect.height());
            mesh.colored_vertex(pos, c32(hsl_to_rgb([u, 1.0 - v, 0.5])));
        }
    }
    for j in 0..ny {
        for i in 0..nx {
            let a = j * (nx + 1) + i;
            mesh.add_triangle(a, a + 1, a + nx + 1);
            mesh.add_triangle(a + 1, a + nx + 2, a + nx + 1);
        }
    }
    let painter = ui.painter();
    painter.add(mesh);
    painter.rect_stroke(rect, 0.0, Stroke::new(1.0, Color32::from_gray(120)), StrokeKind::Outside);
    // Windows' crosshair marker.
    let c = pos2(rect.min.x + hsl[0] * rect.width(), rect.min.y + (1.0 - hsl[1]) * rect.height());
    let s = Stroke::new(2.0, Color32::BLACK);
    for d in [vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0)] {
        painter.line_segment([c + d * 3.0, c + d * 9.0], s);
    }
}

/// The luminance bar beside the spectrum, with its arrow marker.
fn luminance_bar(ui: &mut Ui, hsl: &mut [f32; 3]) {
    let (rect, resp) = ui.allocate_exact_size(vec2(26.0, 170.0), Sense::click_and_drag());
    let bar = Rect::from_min_size(rect.min, vec2(14.0, rect.height()));
    if let Some(pos) = resp.interact_pointer_pos() {
        hsl[2] = (1.0 - (pos.y - rect.min.y) / rect.height()).clamp(0.0, 1.0);
    }
    let mut mesh = egui::Mesh::default();
    let n = 16u32;
    for k in 0..=n {
        let t = k as f32 / n as f32;
        let color = c32(hsl_to_rgb([hsl[0], hsl[1], 1.0 - t]));
        let y = bar.min.y + t * bar.height();
        mesh.colored_vertex(pos2(bar.min.x, y), color);
        mesh.colored_vertex(pos2(bar.max.x, y), color);
        if k > 0 {
            let a = 2 * (k - 1);
            mesh.add_triangle(a, a + 1, a + 2);
            mesh.add_triangle(a + 1, a + 3, a + 2);
        }
    }
    let painter = ui.painter();
    painter.add(mesh);
    painter.rect_stroke(bar, 0.0, Stroke::new(1.0, Color32::from_gray(120)), StrokeKind::Outside);
    let y = bar.min.y + (1.0 - hsl[2]) * bar.height();
    let tip = pos2(bar.max.x + 2.0, y);
    painter.add(Shape::convex_polygon(vec![tip, tip + vec2(7.0, -5.0), tip + vec2(7.0, 5.0)], Color32::BLACK, Stroke::NONE));
}

/// Hue/Sat/Lum on Windows' 0–240 scale, and Red/Green/Blue.
fn color_fields(ui: &mut Ui, hsl: &mut [f32; 3]) {
    egui::Grid::new("color-fields").num_columns(4).spacing(vec2(6.0, 4.0)).show(ui, |ui| {
        let mut rgb = hsl_to_rgb(*hsl);
        let labels = [("Hue:", "Red:"), ("Sat:", "Green:"), ("Lum:", "Blue:")];
        for (i, (hl, rl)) in labels.into_iter().enumerate() {
            ui.label(hl);
            let max = if i == 0 { 239.0 } else { 240.0 };
            let mut v = (hsl[i] * max).round();
            if ui.add(egui::DragValue::new(&mut v).range(0.0..=max).speed(1.0).max_decimals(0)).changed() {
                hsl[i] = v / max;
            }
            ui.label(rl);
            if ui.add(egui::DragValue::new(&mut rgb[i]).range(0..=255).speed(1.0)).changed() {
                *hsl = rgb_to_hsl(rgb);
            }
            ui.end_row();
        }
    });
}

/// White-on-transparent brush sample for the Brushes gallery (tinted when drawn).
fn brush_preview(kind: BrushKind) -> ColorImage {
    let side = 96u32;
    let mut img = RgbaImage::new(side, side);
    let mut mask = Mask::new(side, side);
    let mut rng = 0x2545_F491_4F6C_DD1D;
    // An "S" stroke across the tile.
    let pts: Vec<Pos2> = (0..=40)
        .map(|i| {
            let t = i as f32 / 40.0;
            pos2(14.0 + t * 68.0, 48.0 - (t * std::f32::consts::TAU).sin() * 22.0 + (t - 0.5) * 20.0)
        })
        .collect();
    for seg in pts.windows(2) {
        kind.segment(&mut mask, seg[0], seg[1], 11.0, &mut rng);
    }
    let all = IRect::of_image(&img);
    raster::blend_mask(&mut img, &mask, WHITE, kind.opacity(), all);
    ColorImage::from_rgba_unmultiplied([side as usize, side as usize], img.as_raw())
}

/// The app icon, rendered for the size it is shown at.
fn paint_logo(painter: &egui::Painter, rect: Rect) {
    let ppp = painter.ctx().pixels_per_point();
    let px = (rect.width() * ppp).round().max(1.0) as u32;
    let tex = icons::cached_texture(painter.ctx(), "app-logo", px, |px| {
        let img = crate::logo::render(px);
        ColorImage::from_rgba_unmultiplied([px as usize, px as usize], img.as_raw())
    });
    let min = ((rect.min.to_vec2() * ppp).round() / ppp).to_pos2();
    painter.image(tex.id(), Rect::from_min_size(min, Vec2::splat(px as f32 / ppp)), Rect::from_min_max(Pos2::ZERO, pos2(1.0, 1.0)), Color32::WHITE);
}

fn file_meta(path: &std::path::Path) -> Option<(u64, Option<std::time::SystemTime>)> {
    let m = std::fs::metadata(path).ok()?;
    Some((m.len(), m.modified().ok()))
}

fn format_bytes(b: u64) -> String {
    match b {
        0..1024 => format!("{b} bytes"),
        1024..1_048_576 => format!("{:.1}KB", b as f64 / 1024.0),
        _ => format!("{:.1}MB", b as f64 / 1_048_576.0),
    }
}

/// Local date and time, e.g. "10/10/2026 13:45".
fn format_time(t: std::time::SystemTime) -> String {
    let secs = t.duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs() as i64);
    let secs = secs + local_utc_offset(secs);
    let days = secs.div_euclid(86_400);
    let tod = secs.rem_euclid(86_400);
    // Civil-from-days (Howard Hinnant).
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!("{d:02}/{m:02}/{y} {:02}:{:02}", tod / 3600, tod % 3600 / 60)
}

#[cfg(unix)]
fn local_utc_offset(secs: i64) -> i64 {
    #[repr(C)]
    struct Tm {
        sec: i32,
        min: i32,
        hour: i32,
        mday: i32,
        mon: i32,
        year: i32,
        wday: i32,
        yday: i32,
        isdst: i32,
        gmtoff: std::ffi::c_long,
        zone: *const std::ffi::c_char,
    }
    unsafe extern "C" {
        fn localtime_r(t: *const i64, out: *mut Tm) -> *mut Tm;
    }
    let mut tm = std::mem::MaybeUninit::<Tm>::zeroed();
    // SAFETY: localtime_r only writes the struct we hand it.
    let ok = unsafe { !localtime_r(&secs, tm.as_mut_ptr()).is_null() };
    if ok { unsafe { tm.assume_init().gmtoff as i64 } } else { 0 }
}

#[cfg(not(unix))]
fn local_utc_offset(_secs: i64) -> i64 {
    0
}

/// Where Paint keeps what it remembers between runs (and its last crash report).
pub fn settings_dir() -> Option<PathBuf> {
    // Tests keep their own: they change settings (asking friends first, for one),
    // which must never land in the settings of whoever runs them.
    if cfg!(test) {
        return Some(std::env::temp_dir().join("paint-tests"));
    }
    if cfg!(windows) {
        return std::env::var_os("APPDATA").map(|d| PathBuf::from(d).join("Paint (Rust)"));
    }
    let home = std::env::var_os("HOME")?;
    Some(if cfg!(target_os = "macos") {
        std::path::Path::new(&home).join("Library/Application Support/Paint (Rust)")
    } else {
        std::path::Path::new(&home).join(".config/paint-rust")
    })
}

fn recent_file() -> Option<PathBuf> {
    settings_dir().map(|d| d.join("recent.txt"))
}

fn me_file() -> Option<PathBuf> {
    settings_dir().map(|d| d.join("me.txt"))
}

/// My name and colour for drawing together, and whether friends ask to join
/// drawings I start, as last chosen.
fn load_me() -> Option<(String, [u8; 3], bool)> {
    let text = std::fs::read_to_string(me_file()?).ok()?;
    let mut lines = text.lines();
    let name = names::tidy(lines.next()?);
    let c: Vec<u8> = lines.next()?.split(',').filter_map(|v| v.trim().parse().ok()).collect();
    let ask_first = lines.next() == Some("ask first");
    (c.len() == 3).then(|| (name, [c[0], c[1], c[2]], ask_first))
}

fn save_me(name: &str, [r, g, b]: [u8; 3], ask_first: bool) {
    let Some(file) = me_file() else { return };
    if let Some(dir) = file.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let door = if ask_first { "ask first" } else { "let in" };
    let _ = std::fs::write(file, format!("{name}\n{r},{g},{b}\n{door}\n"));
}

fn load_recent() -> Vec<PathBuf> {
    let Some(file) = recent_file() else { return Vec::new() };
    std::fs::read_to_string(file).map(|s| s.lines().filter(|l| !l.is_empty()).map(PathBuf::from).collect()).unwrap_or_default()
}

fn save_recent(list: &[PathBuf]) {
    let Some(file) = recent_file() else { return };
    if let Some(dir) = file.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let text: String = list.iter().map(|p| format!("{}\n", p.display())).collect();
    let _ = std::fs::write(file, text);
}

/// Whether the V key is physically down. Only used together with ⌘.
#[cfg(target_os = "macos")]
fn paste_key_down() -> bool {
    #[link(name = "CoreGraphics", kind = "framework")]
    unsafe extern "C" {
        fn CGEventSourceKeyState(state_id: i32, key: u16) -> bool;
    }
    const COMBINED_SESSION_STATE: i32 = 0;
    const KEY_V: u16 = 9;
    // SAFETY: a read-only query of the keyboard state with valid constant arguments.
    unsafe { CGEventSourceKeyState(COMBINED_SESSION_STATE, KEY_V) }
}

#[cfg(not(target_os = "macos"))]
fn paste_key_down() -> bool {
    false
}

#[cfg(test)]
mod tests {
    //! Drive the tools through the same begin/update/end path mouse input takes.

    use super::*;

    const RED: Rgba8 = [237, 28, 36, 255];
    const BLUE: Rgba8 = [0, 0, 255, 255];

    fn app() -> PaintApp {
        let mut app = PaintApp::with_context(Context::default(), None);
        app.img = RgbaImage::from_pixel(200, 120, Rgba(WHITE));
        app.view_rect = Rect::from_min_size(Pos2::ZERO, vec2(800.0, 600.0));
        app
    }

    fn drag(app: &mut PaintApp, button: PointerButton, pts: &[(f32, f32)]) {
        let pts: Vec<Pos2> = pts.iter().map(|&(x, y)| pos2(x, y)).collect();
        app.begin(pts[0], button, Vec2::ZERO);
        app.update(&pts[1..], false, 1.0 / 60.0);
        app.end();
    }

    fn px(app: &PaintApp, x: u32, y: u32) -> Rgba8 {
        app.img.get_pixel(x, y).0
    }

    #[test]
    fn pencil_draws_and_undo_redo_restore() {
        let mut a = app();
        drag(&mut a, PointerButton::Primary, &[(10.0, 10.0), (60.0, 10.0)]);
        assert_eq!(px(&a, 35, 10), BLACK);
        assert_eq!(px(&a, 35, 12), WHITE);
        assert!(a.is_dirty());
        a.undo();
        assert_eq!(px(&a, 35, 10), WHITE);
        assert!(!a.is_dirty());
        a.redo();
        assert_eq!(px(&a, 35, 10), BLACK);
    }

    #[test]
    fn right_button_paints_with_color_2() {
        let mut a = app();
        a.secondary = RED;
        drag(&mut a, PointerButton::Secondary, &[(10.0, 20.0), (60.0, 20.0)]);
        assert_eq!(px(&a, 30, 20), RED);
    }

    #[test]
    fn every_brush_changes_pixels_and_undoes_cleanly() {
        for kind in BrushKind::ALL {
            let mut a = app();
            let original = a.img.clone();
            a.tool = Tool::Brush;
            a.brush = kind;
            a.size_brush = 14.0;
            drag(&mut a, PointerButton::Primary, &[(20.0, 60.0), (90.0, 60.0), (160.0, 70.0)]);
            assert_ne!(a.img, original, "{kind:?} drew nothing");
            a.undo();
            assert_eq!(a.img, original, "{kind:?} undo");
        }
    }

    #[test]
    fn eraser_paints_color_2() {
        let mut a = app();
        a.img = RgbaImage::from_pixel(200, 120, Rgba(BLUE));
        a.tool = Tool::Eraser;
        a.size_eraser = 10.0;
        drag(&mut a, PointerButton::Primary, &[(50.0, 50.0), (80.0, 50.0)]);
        assert_eq!(px(&a, 65, 50), WHITE);
        assert_eq!(px(&a, 65, 70), BLUE);
    }

    #[test]
    fn shapes_outline_with_color_1_and_fill_with_color_2() {
        let mut a = app();
        a.tool = Tool::Shape;
        a.shape = ShapeKind::Rectangle;
        a.shape_fill = true;
        a.secondary = RED;
        a.size_shape = 2.0;
        drag(&mut a, PointerButton::Primary, &[(20.0, 20.0), (60.0, 50.0), (100.0, 80.0)]);
        assert_eq!(px(&a, 20, 50), BLACK, "left edge");
        assert_eq!(px(&a, 60, 50), RED, "interior");
        assert_eq!(px(&a, 140, 50), WHITE, "outside");
        // The intermediate drag position left nothing behind.
        assert_eq!(px(&a, 60, 35), RED);
        // Still editable until committed.
        assert!(a.live_shape.is_some());
        assert_eq!(a.history.state_id(), 0);
        a.commit_all();
        assert!(a.live_shape.is_none());
        assert_eq!(a.history.state_id(), 1);
        assert_eq!(px(&a, 60, 50), RED);
    }

    fn rect_shape(a: &mut PaintApp) {
        a.tool = Tool::Shape;
        a.shape = ShapeKind::Rectangle;
        a.size_shape = 2.0;
        drag(a, PointerButton::Primary, &[(20.0, 20.0), (60.0, 50.0)]);
        a.render_live_shape();
    }

    #[test]
    fn picking_another_shape_stamps_the_live_one_and_draws_anew() {
        let mut a = app();
        rect_shape(&mut a);
        a.pick_shape(ShapeKind::Oval);
        assert!(a.live_shape.is_none(), "the rectangle is stamped");
        assert_eq!(a.history.state_id(), 1);
        assert_eq!(px(&a, 20, 20), BLACK, "still a rectangle: its corner is drawn");
        drag(&mut a, PointerButton::Primary, &[(100.0, 100.0), (160.0, 140.0)]);
        assert_eq!(a.live_shape.as_ref().map(|l| l.kind), Some(ShapeKind::Oval));
        assert_eq!(px(&a, 20, 20), BLACK, "the rectangle stays");
    }

    #[test]
    fn live_shape_moves_by_dragging_its_body() {
        let mut a = app();
        rect_shape(&mut a);
        assert_eq!(px(&a, 20, 35), BLACK);
        drag(&mut a, PointerButton::Primary, &[(40.0, 35.0), (90.0, 65.0)]);
        a.render_live_shape();
        assert_eq!(px(&a, 20, 35), WHITE, "old position cleared");
        assert_eq!(px(&a, 70, 65), BLACK, "drawn at the new position");
        a.commit_all();
        a.undo();
        assert_eq!(a.img, RgbaImage::from_pixel(200, 120, Rgba(WHITE)), "one undo step removes it");
    }

    #[test]
    fn live_shape_resizes_from_a_handle_and_follows_the_ribbon() {
        let mut a = app();
        rect_shape(&mut a);
        // Bottom-right handle out to (100, 90).
        drag(&mut a, PointerButton::Primary, &[(60.0, 50.0), (100.0, 90.0)]);
        a.render_live_shape();
        assert_eq!(px(&a, 100, 70), BLACK, "right edge moved");
        assert_eq!(px(&a, 60, 50), WHITE, "old corner cleared");
        // Restyle from the ribbon: thicker red outline.
        a.primary = RED;
        a.size_shape = 6.0;
        a.render_live_shape();
        assert_eq!(px(&a, 102, 70), RED);
        // Choosing another tool stamps it.
        a.set_tool(Tool::Pencil);
        assert!(a.live_shape.is_none());
        assert_eq!(a.history.state_id(), 1);
    }

    #[test]
    fn delete_removes_the_live_shape_and_clicking_away_commits_it() {
        let mut a = app();
        rect_shape(&mut a);
        a.key(Key::Delete, Modifiers::NONE);
        assert!(a.live_shape.is_none());
        assert_eq!(a.img, RgbaImage::from_pixel(200, 120, Rgba(WHITE)));
        rect_shape(&mut a);
        // A new shape elsewhere commits the first.
        drag(&mut a, PointerButton::Primary, &[(120.0, 80.0), (180.0, 110.0)]);
        a.render_live_shape();
        assert_eq!(a.history.state_id(), 1);
        assert_eq!(px(&a, 20, 35), BLACK);
        assert!(a.live_shape.is_some());
    }

    #[test]
    fn click_without_drag_draws_no_shape() {
        let mut a = app();
        a.tool = Tool::Shape;
        let original = a.img.clone();
        drag(&mut a, PointerButton::Primary, &[(20.0, 20.0)]);
        assert_eq!(a.img, original);
        assert!(!a.history.can_undo());
    }

    #[test]
    fn fill_stays_inside_outline() {
        let mut a = app();
        a.tool = Tool::Shape;
        a.shape = ShapeKind::Rectangle;
        drag(&mut a, PointerButton::Primary, &[(20.0, 20.0), (80.0, 80.0)]);
        a.tool = Tool::Fill;
        a.primary = RED;
        drag(&mut a, PointerButton::Primary, &[(50.0, 50.0)]);
        assert_eq!(px(&a, 50, 50), RED);
        assert_eq!(px(&a, 120, 50), WHITE);
    }

    #[test]
    fn fill_leaves_no_pale_ring_inside_an_antialiased_outline() {
        let mut a = app();
        a.tool = Tool::Shape;
        a.shape = ShapeKind::Oval;
        a.size_shape = 2.0;
        drag(&mut a, PointerButton::Primary, &[(20.0, 20.0), (120.0, 100.0)]);
        a.commit_all();
        a.tool = Tool::Fill;
        a.primary = RED;
        drag(&mut a, PointerButton::Primary, &[(70.0, 60.0)]);
        // Inside the outline's centre line, every pixel is fill, outline, or a blend of
        // the two — never the old white showing through (high green).
        let (cx, cy, rx, ry) = (70.0, 60.0, 50.0, 40.0);
        let mut pale = 0;
        for y in 20..100 {
            for x in 20..120 {
                let (dx, dy) = ((x as f32 + 0.5 - cx) / rx, (y as f32 + 0.5 - cy) / ry);
                if dx * dx + dy * dy < 1.0 && px(&a, x, y)[1] > 60 {
                    pale += 1;
                }
            }
        }
        assert_eq!(pale, 0, "pale pixels between the fill and the outline");
        // And nothing leaked past the outline.
        let mut leaked = 0;
        for y in 0..120 {
            for x in 0..200 {
                let (dx, dy) = ((x as f32 + 0.5 - cx) / (rx + 3.0), (y as f32 + 0.5 - cy) / (ry + 3.0));
                if dx * dx + dy * dy > 1.0 && px(&a, x, y) != WHITE {
                    leaked += 1;
                }
            }
        }
        assert_eq!(leaked, 0, "fill escaped the outline");
    }

    #[test]
    fn picker_sets_color_and_returns_to_previous_tool() {
        let mut a = app();
        a.img.put_pixel(5, 5, Rgba(RED));
        a.set_tool(Tool::Brush);
        a.set_tool(Tool::Picker);
        drag(&mut a, PointerButton::Secondary, &[(5.5, 5.5)]);
        assert_eq!(a.secondary, RED);
        assert_eq!(a.tool, Tool::Brush);
    }

    #[test]
    fn moving_a_selection_is_one_undo_step() {
        let mut a = app();
        raster::fill_rect(&mut a.img, IRect::new(10, 10, 30, 30), BLUE);
        let original = a.img.clone();
        a.tool = Tool::Select;
        drag(&mut a, PointerButton::Primary, &[(10.0, 10.0), (30.0, 30.0)]);
        assert_eq!(a.sel.as_ref().map(|s| s.rect), Some(IRect::new(10, 10, 30, 30)));
        drag(&mut a, PointerButton::Primary, &[(15.0, 15.0), (65.0, 15.0)]);
        assert_eq!(px(&a, 70, 20), BLUE, "moved pixels");
        assert_eq!(px(&a, 20, 20), WHITE, "hole filled with Color 2");
        a.commit_selection();
        assert_eq!(a.history.state_id(), 1);
        a.undo();
        assert_eq!(a.img, original);
    }

    #[test]
    fn free_form_delete_only_clears_inside_the_lasso() {
        let mut a = app();
        a.img = RgbaImage::from_pixel(200, 120, Rgba(BLUE));
        a.tool = Tool::FreeSelect;
        drag(&mut a, PointerButton::Primary, &[(20.0, 20.0), (80.0, 20.0), (20.0, 80.0), (20.0, 20.0)]);
        assert!(a.sel.as_ref().is_some_and(|s| s.lasso.is_some()));
        a.delete_selection();
        assert_eq!(px(&a, 30, 30), WHITE, "inside triangle");
        assert_eq!(px(&a, 75, 75), BLUE, "inside bounding box but outside triangle");
    }

    #[test]
    fn transparent_selection_skips_color_2() {
        let mut a = app();
        raster::fill_rect(&mut a.img, IRect::new(10, 10, 20, 20), BLUE);
        raster::fill_rect(&mut a.img, IRect::new(60, 0, 120, 120), RED);
        a.transparent_selection = true;
        a.tool = Tool::Select;
        drag(&mut a, PointerButton::Primary, &[(5.0, 5.0), (25.0, 25.0)]);
        drag(&mut a, PointerButton::Primary, &[(15.0, 15.0), (75.0, 15.0)]);
        assert_eq!(px(&a, 75, 15), BLUE);
        assert_eq!(px(&a, 66, 6), RED, "white margin of the selection is see-through");
        a.transparent_selection = false;
        a.refresh_float();
        assert_eq!(px(&a, 66, 6), WHITE, "opaque again once toggled off");
    }

    #[test]
    fn rotating_a_selection_only_touches_the_selection() {
        let mut a = app();
        raster::fill_rect(&mut a.img, IRect::new(10, 10, 50, 20), BLUE);
        a.img.put_pixel(150, 100, Rgba(RED));
        a.tool = Tool::Select;
        drag(&mut a, PointerButton::Primary, &[(10.0, 10.0), (50.0, 20.0)]);
        a.transform(imageops::rotate90);
        assert_eq!(a.sel.as_ref().map(|s| (s.rect.width(), s.rect.height())), Some((10, 40)));
        assert_eq!(px(&a, 15, 45), BLUE);
        assert_eq!(px(&a, 45, 15), WHITE);
        assert_eq!(px(&a, 150, 100), RED);
    }

    #[test]
    fn paste_larger_than_canvas_grows_it_and_undo_shrinks_it() {
        let mut a = app();
        a.paste_image(RgbaImage::from_pixel(300, 50, Rgba(RED)));
        assert_eq!(a.dims(), (300, 120));
        assert_eq!(px(&a, 250, 10), RED);
        a.commit_selection();
        a.undo();
        assert_eq!(a.dims(), (200, 120));
    }

    #[test]
    fn text_is_rendered_and_committed() {
        let mut a = app();
        a.tool = Tool::Text;
        a.text_style.size_pt = 24.0;
        let original = a.img.clone();
        drag(&mut a, PointerButton::Primary, &[(10.0, 10.0)]);
        a.text.as_mut().unwrap().text = "Hello".into();
        a.update_text_preview();
        assert_ne!(a.img, original);
        a.set_tool(Tool::Pencil);
        assert!(a.text.is_none());
        assert!(a.history.can_undo());
        a.undo();
        assert_eq!(a.img, original);
    }

    #[test]
    fn canvas_resize_handle_and_crop() {
        let mut a = app();
        a.op = Op::ResizeCanvas { button: PointerButton::Primary, handle: Handle::Corner, size: (200, 120) };
        a.update(&[pos2(250.4, 90.0)], false, 0.0);
        a.end();
        assert_eq!(a.dims(), (250, 90));
        a.tool = Tool::Select;
        drag(&mut a, PointerButton::Primary, &[(10.0, 10.0), (50.0, 40.0)]);
        a.crop();
        assert_eq!(a.dims(), (40, 30));
        a.undo();
        assert_eq!(a.dims(), (250, 90));
    }

    #[test]
    fn hsl_round_trips_and_text_tab_follows_the_text_tool() {
        for c in [[0, 120, 215, 255], [255, 255, 255, 255], [0, 0, 0, 255], [237, 28, 36, 255], [127, 127, 127, 255]] {
            assert_eq!(hsl_to_rgb(rgb_to_hsl(c)), c);
        }
        let mut a = app();
        a.set_tool(Tool::Text);
        assert_eq!(a.tab, Tab::Text);
        a.set_tool(Tool::Pencil);
        assert_eq!(a.tab, Tab::Home);
    }

    /// Feed real input events through the app's keyboard handling, one frame per call.
    fn type_events(a: &mut PaintApp, events: Vec<Event>) {
        let ctx = a.ctx.clone();
        let screen = Rect::from_min_size(Pos2::ZERO, vec2(1000.0, 700.0));
        let input = egui::RawInput { screen_rect: Some(screen), events, ..Default::default() };
        let mut out = ctx.run_ui(input, |ui| a.handle_keys(ui.ctx()));
        // No renderer here to upload textures.
        out.textures_delta.clear();
    }

    fn key(key: Key, modifiers: Modifiers) -> Event {
        Event::Key { key, physical_key: None, pressed: true, repeat: false, modifiers }
    }

    fn typed(a: &PaintApp) -> &str {
        a.text.as_ref().map_or("", |t| t.text.as_str())
    }

    #[test]
    fn zoom_keys_zoom_the_picture_not_the_buttons() {
        let mut a = app();
        type_events(&mut a, vec![key(Key::Plus, Modifiers::COMMAND)]);
        type_events(&mut a, vec![key(Key::Plus, Modifiers::COMMAND)]);
        assert!(a.zoom > 1.0);
        assert_eq!(a.ctx.zoom_factor(), 1.0, "the ribbon and status bar stay their size");
        type_events(&mut a, vec![key(Key::Minus, Modifiers::COMMAND)]);
        assert_eq!(a.ctx.zoom_factor(), 1.0);
    }

    #[test]
    fn pencil_fill_and_brush_use_their_own_pointer() {
        let mut a = app();
        let ctx = a.ctx.clone();
        let screen = Rect::from_min_size(Pos2::ZERO, vec2(1000.0, 700.0));
        let frame = |a: &mut PaintApp, events: Vec<Event>| {
            let input = egui::RawInput { screen_rect: Some(screen), events, ..Default::default() };
            let mut out = ctx.run_ui(input, |ui| a.canvas(ui));
            out.textures_delta.clear();
            out.platform_output.cursor_icon
        };
        frame(&mut a, vec![]);
        let over = a.canvas_rect.min + vec2(20.0, 20.0);
        for (tool, icon) in [(Tool::Pencil, Icon::Pencil), (Tool::Fill, Icon::Fill), (Tool::Brush, Icon::Brushes), (Tool::Picker, Icon::Picker)] {
            a.tool = tool;
            assert_eq!(frame(&mut a, vec![Event::PointerMoved(over)]), CursorIcon::None, "{tool:?}");
            assert_eq!(a.tool_cursor, Some((icon, over)));
        }
        // The eraser shows just its footprint: no crosshair, no icon.
        a.tool = Tool::Eraser;
        assert_eq!(frame(&mut a, vec![Event::PointerMoved(over)]), CursorIcon::None);
        assert_eq!(a.tool_cursor, None);
        a.tool = Tool::Shape;
        assert_eq!(frame(&mut a, vec![Event::PointerMoved(over)]), CursorIcon::Crosshair);
        assert_eq!(a.tool_cursor, None);
        // Off the canvas the normal pointer comes back.
        a.tool = Tool::Pencil;
        frame(&mut a, vec![Event::PointerMoved(pos2(990.0, 690.0))]);
        assert_eq!(a.tool_cursor, None);
    }

    #[test]
    fn gridlines_show_at_every_zoom() {
        assert_eq!(grid_step(1.0), 8);
        assert_eq!(grid_step(0.5), 16);
        assert_eq!(grid_step(2.0), 8);
        assert_eq!(grid_step(3.0), 8);
        assert_eq!(grid_step(4.0), 1);
        assert_eq!(grid_step(8.0), 1);
        assert_eq!(grid_step(0.125), 64);
    }

    #[test]
    fn typing_goes_straight_onto_the_canvas() {
        let mut a = app();
        a.tool = Tool::Text;
        drag(&mut a, PointerButton::Primary, &[(40.0, 30.0)]);
        let blank = a.img.clone();
        type_events(&mut a, vec![Event::Text("Helo".into())]);
        assert_ne!(a.img, blank, "rendered on the canvas as you type");
        type_events(&mut a, vec![key(Key::ArrowLeft, Modifiers::NONE), Event::Text("l".into())]);
        assert_eq!(typed(&a), "Hello");
        type_events(&mut a, vec![key(Key::End, Modifiers::NONE), key(Key::Enter, Modifiers::NONE), Event::Text("World".into())]);
        assert_eq!(typed(&a), "Hello\nWorld");
        // ⌥⇧← (Ctrl+Shift+← off the Mac) selects the last word; typing replaces it; ⌘Z brings it back.
        let word_select = if cfg!(target_os = "macos") { Modifiers::ALT | Modifiers::SHIFT } else { Modifiers::COMMAND | Modifiers::SHIFT };
        type_events(&mut a, vec![key(Key::ArrowLeft, word_select)]);
        type_events(&mut a, vec![Event::Text("Paint".into())]);
        assert_eq!(typed(&a), "Hello\nPaint");
        type_events(&mut a, vec![key(Key::Z, Modifiers::COMMAND)]);
        assert_eq!(typed(&a), "Hello\nWorld");
        // Undo put the caret back where that edit began; End, then ↑ keeps the column.
        type_events(&mut a, vec![key(Key::End, Modifiers::NONE), key(Key::ArrowUp, Modifiers::NONE), key(Key::Backspace, Modifiers::NONE)]);
        assert_eq!(typed(&a), "Hell\nWorld");
        // An input method can report a keystroke twice; it is typed once.
        type_events(&mut a, vec![Event::Text("é".into()), Event::Ime(egui::ImeEvent::Commit("é".into()))]);
        assert_eq!(typed(&a), "Hellé\nWorld");
        // Pasting text inserts it at the caret.
        type_events(&mut a, vec![Event::Paste("!".into())]);
        assert_eq!(typed(&a), "Hellé!\nWorld");
        // Esc stops typing; the arrows then move the text instead.
        type_events(&mut a, vec![key(Key::Escape, Modifiers::NONE)]);
        assert!(!a.text.as_ref().unwrap().editing);
        let before = a.text.as_ref().unwrap().pos;
        type_events(&mut a, vec![key(Key::ArrowRight, Modifiers::NONE)]);
        assert_eq!(a.text.as_ref().unwrap().pos, before + vec2(1.0, 0.0));
        assert_eq!(typed(&a), "Hellé!\nWorld");
    }

    #[test]
    fn clicking_in_the_text_puts_the_caret_there() {
        let mut a = app();
        a.tool = Tool::Text;
        drag(&mut a, PointerButton::Primary, &[(20.0, 20.0)]);
        type_events(&mut a, vec![Event::Text("Hello".into()), key(Key::Escape, Modifiers::NONE)]);
        let tb = a.text.as_ref().unwrap();
        let target = tb.pos + tb.layout.caret(2) + vec2(0.4, tb.layout.line_h * 0.5);
        drag(&mut a, PointerButton::Primary, &[(target.x, target.y)]);
        let tb = a.text.as_ref().unwrap();
        assert!(tb.editing);
        assert_eq!(tb.cursor.cursor, 2);
        type_events(&mut a, vec![Event::Text("_".into())]);
        assert_eq!(typed(&a), "He_llo");
    }

    #[test]
    fn placed_text_can_be_moved_reopened_and_scaled() {
        let mut a = app();
        a.tool = Tool::Text;
        a.text_style.size_pt = 20.0;
        drag(&mut a, PointerButton::Primary, &[(10.0, 10.0)]);
        a.text.as_mut().unwrap().text = "Hi".into();
        a.update_text_preview();
        let old = a.text.as_ref().unwrap().bounds;
        // Drag it by its body.
        drag(&mut a, PointerButton::Primary, &[(15.0, 15.0), (65.0, 45.0)]);
        let tb = a.text.as_ref().unwrap();
        assert_eq!(tb.pos, pos2(60.0, 40.0));
        let ink = |a: &PaintApp, r: Rect| {
            (r.min.y as u32..r.max.y as u32).any(|y| (r.min.x as u32..r.max.x as u32).any(|x| px(a, x, y) != WHITE))
        };
        assert!(!ink(&a, old), "old spot cleared");
        assert!(ink(&a, tb.bounds), "drawn at the new spot");
        // A click without a drag reopens the editor.
        a.text.as_mut().unwrap().editing = false;
        drag(&mut a, PointerButton::Primary, &[(70.0, 50.0)]);
        assert!(a.text.as_ref().unwrap().editing);
        // Dragging the bottom-right handle out to twice the size doubles the font size.
        let b = a.text.as_ref().unwrap().bounds;
        let corner = b.expand(3.0).right_bottom();
        drag(&mut a, PointerButton::Primary, &[(corner.x, corner.y), (b.min.x + 2.0 * b.width(), b.min.y + 2.0 * b.height())]);
        assert!((a.text_style.size_pt - 40.0).abs() <= 1.0, "size {}", a.text_style.size_pt);
        assert_eq!(a.text.as_ref().unwrap().pos, b.min, "scaled about the opposite corner");
        a.commit_all();
        assert!(a.text.is_none());
        assert_eq!(a.history.state_id(), 1);
    }

    #[test]
    fn zoom_keeps_the_anchor_point_fixed() {
        let mut a = app();
        a.scroll_offset = vec2(100.0, 50.0);
        let anchor = vec2(300.0, 200.0);
        let before = (a.scroll_offset + anchor - Vec2::splat(MARGIN)) / a.zoom;
        a.zoom_at(4.0, Some(anchor));
        let after = (a.pending_scroll.unwrap() + anchor - Vec2::splat(MARGIN)) / a.zoom;
        assert!((before - after).length() < 1e-3, "{before:?} vs {after:?}");
    }

    /// Several kids in one room, with every message in flight held in queues so a
    /// test decides exactly when each one arrives.
    mod together {
        use std::collections::VecDeque;

        use super::*;
        use crate::collab::hub::{Hub, To};
        use crate::collab::protocol::PeerId;

        struct Room {
            hub: Hub,
            kids: Vec<(PeerId, PaintApp)>,
            /// Each kid's messages on their way to the hub, and the hub's on their way to each kid.
            up: Vec<VecDeque<ToHub>>,
            down: Vec<VecDeque<FromHub>>,
            /// Friends' edits that arrived mid-stroke, over a live shape, text box
            /// or floating selection: the cases where rebasing matters.
            busy: [u32; 4],
            /// Advances with every send, so previews and pointers go out each time.
            clock: f64,
        }

        impl Room {
            fn new(n: usize) -> Room {
                let mut room =
                    Room { hub: Hub::new(app().img), kids: Vec::new(), up: Vec::new(), down: Vec::new(), busy: [0; 4], clock: 0.0 };
                for _ in 0..n {
                    room.join();
                }
                room
            }

            /// A new kid arrives (each from their own computer), and comes in or
            /// waits to be let in; returns their index.
            fn join(&mut self) -> usize {
                let id = self.kids.len() as PeerId + 1;
                let out = self.hub.join(id, format!("Kid {id}"), [200, 50, 50], format!("10.0.0.{id}:47100"));
                self.kids.push((id, app()));
                self.up.push(VecDeque::new());
                self.down.push(VecDeque::new());
                self.route(out);
                let i = self.kids.len() - 1;
                self.kid_gets(i);
                i
            }

            fn route(&mut self, out: Vec<(To, FromHub)>) {
                for (to, msg) in out {
                    for (i, (id, _)) in self.kids.iter().enumerate() {
                        let wanted = match to {
                            To::One(k) => k == *id,
                            To::All => self.hub.is_in(*id),
                            To::Others(k) => k != *id && self.hub.is_in(*id),
                            To::Everyone => true,
                        };
                        if wanted {
                            self.down[i].push_back(msg.clone());
                        }
                    }
                }
            }

            fn kid(&mut self, i: usize) -> &mut PaintApp {
                &mut self.kids[i].1
            }

            /// Hand the hub kid `i`'s next message.
            fn hub_takes(&mut self, i: usize) -> bool {
                self.clock += 0.05;
                let out = self.kids[i].1.net_outbox_at(self.clock);
                self.up[i].extend(out);
                let Some(msg) = self.up[i].pop_front() else {
                    return false;
                };
                let out = self.hub.handle(self.kids[i].0, msg);
                self.route(out);
                true
            }

            /// Hand kid `i` the hub's next message for them.
            fn kid_gets(&mut self, i: usize) -> bool {
                let Some(msg) = self.down[i].pop_front() else {
                    return false;
                };
                let a = &self.kids[i].1;
                if matches!(&msg, FromHub::Sequenced { from, .. } if *from != self.kids[i].0) {
                    let float = a.sel.as_ref().is_some_and(|s| s.float.is_some());
                    for (n, busy) in
                        [matches!(a.op, Op::Stroke { .. }), a.live_shape.is_some(), a.text.is_some(), float].into_iter().enumerate()
                    {
                        self.busy[n] += busy as u32;
                    }
                }
                self.kids[i].1.net_receive(msg);
                true
            }

            /// Deliver everything in flight.
            fn quiesce(&mut self) {
                loop {
                    let mut moved = false;
                    for i in 0..self.kids.len() {
                        while self.hub_takes(i) {
                            moved = true;
                        }
                        while self.kid_gets(i) {
                            moved = true;
                        }
                    }
                    if !moved {
                        break;
                    }
                }
            }

            /// Everyone finishes what they're doing; then every screen in the room
            /// must match the hub's picture exactly.
            fn assert_converged(&mut self) {
                for i in 0..self.kids.len() {
                    self.kid(i).settle();
                }
                self.quiesce();
                let truth = self.hub.picture().clone();
                for (i, (_, a)) in self.kids.iter().enumerate().filter(|(_, (id, _))| self.hub.is_in(*id)) {
                    let s = a.session.as_ref().unwrap();
                    assert_eq!(s.unconfirmed(), 0, "kid {i} still waiting on the hub");
                    assert!(s.confirmed() == &truth, "kid {i}: confirmed picture differs from the hub's");
                    assert!(s.base == truth, "kid {i}: base differs from the hub's");
                    assert!(a.img == truth, "kid {i}: screen differs from the hub's");
                    assert!(!s.has_previews(), "kid {i} still shows a friend's unfinished drawing");
                }
            }
        }

        fn line(a: &mut PaintApp, tool: Tool, color: Rgba8, from: (f32, f32), to: (f32, f32)) {
            a.set_tool(tool);
            a.primary = color;
            drag(a, PointerButton::Primary, &[from, to]);
        }

        #[test]
        fn joining_gets_the_hosts_picture() {
            let mut room = Room::new(1);
            line(room.kid(0), Tool::Pencil, RED, (10.0, 10.0), (100.0, 10.0));
            room.quiesce();
            room.join();
            assert_eq!(px(room.kid(1), 50, 10), RED);
            room.assert_converged();
        }

        #[test]
        fn a_friends_stroke_goes_under_the_one_im_drawing() {
            let mut room = Room::new(2);
            // Kid 0 is halfway through a red line when kid 1's blue one arrives.
            room.kid(0).set_tool(Tool::Pencil);
            room.kid(0).primary = RED;
            room.kid(0).size_pencil = 5.0;
            room.kid(0).begin(pos2(10.0, 50.0), PointerButton::Primary, Vec2::ZERO);
            room.kid(0).update(&[pos2(150.0, 50.0)], false, 1.0 / 60.0);
            room.kid(1).size_pencil = 5.0;
            line(room.kid(1), Tool::Pencil, BLUE, (80.0, 10.0), (80.0, 110.0));
            room.quiesce();
            assert_eq!(px(room.kid(0), 80, 20), BLUE, "the friend's line shows up");
            assert_eq!(px(room.kid(0), 80, 50), RED, "my unfinished line stays on top");
            room.kid(0).update(&[pos2(160.0, 50.0)], false, 1.0 / 60.0);
            room.kid(0).end();
            room.assert_converged();
            assert_eq!(px(room.kid(1), 80, 50), RED);
            assert_eq!(px(room.kid(1), 80, 20), BLUE);
        }

        #[test]
        fn edits_crossing_on_the_network_end_up_identical() {
            let mut room = Room::new(3);
            // Everyone draws before seeing anyone else's work.
            room.kid(0).size_pencil = 7.0;
            room.kid(1).size_pencil = 7.0;
            line(room.kid(0), Tool::Pencil, RED, (10.0, 60.0), (190.0, 60.0));
            line(room.kid(1), Tool::Pencil, BLUE, (100.0, 5.0), (100.0, 115.0));
            room.kid(2).set_tool(Tool::Fill);
            room.kid(2).primary = [0, 160, 0, 255];
            drag(room.kid(2), PointerButton::Primary, &[(5.0, 5.0)]);
            // Kid 2's fill reaches the hub first, then kid 1's line, then kid 0's.
            room.hub_takes(2);
            room.hub_takes(1);
            room.hub_takes(0);
            room.assert_converged();
            assert_eq!(px(room.kid(0), 100, 60), RED, "the last edit in the hub's order wins");
        }

        #[test]
        fn undo_takes_back_only_my_own_pixels() {
            let mut room = Room::new(2);
            room.kid(0).size_pencil = 5.0;
            room.kid(1).size_pencil = 5.0;
            line(room.kid(0), Tool::Pencil, RED, (10.0, 50.0), (150.0, 50.0));
            room.quiesce();
            line(room.kid(1), Tool::Pencil, BLUE, (80.0, 10.0), (80.0, 110.0));
            room.quiesce();
            room.kid(0).undo();
            room.assert_converged();
            for i in 0..2 {
                assert_eq!(px(room.kid(i), 40, 50), WHITE, "my line is gone");
                assert_eq!(px(room.kid(i), 80, 50), BLUE, "where a friend drew over it, theirs stays");
            }
            room.kid(0).redo();
            room.assert_converged();
            assert_eq!(px(room.kid(1), 40, 50), RED);
            assert_eq!(px(room.kid(1), 80, 50), BLUE);
        }

        #[test]
        fn a_whole_picture_change_can_only_be_undone_until_friends_draw() {
            let mut room = Room::new(2);
            line(room.kid(0), Tool::Pencil, RED, (10.0, 10.0), (60.0, 10.0));
            room.kid(0).transform(imageops::rotate180);
            room.quiesce();
            room.kid(0).undo();
            room.assert_converged();
            assert_eq!(px(room.kid(1), 30, 10), RED, "undone before anyone drew");
            room.kid(0).redo();
            room.quiesce();
            line(room.kid(1), Tool::Pencil, BLUE, (10.0, 60.0), (60.0, 60.0));
            room.quiesce();
            room.kid(0).undo();
            assert!(room.kid(0).error.is_some(), "too late");
            room.kid(0).error = None;
            room.assert_converged();
            assert_eq!(px(room.kid(1), 30, 60), BLUE);
        }

        #[test]
        fn a_friend_rotating_the_picture_finishes_what_im_drawing_first() {
            let mut room = Room::new(2);
            room.kid(0).set_tool(Tool::Pencil);
            room.kid(0).begin(pos2(10.0, 10.0), PointerButton::Primary, Vec2::ZERO);
            room.kid(0).update(&[pos2(50.0, 10.0)], false, 1.0 / 60.0);
            room.kid(1).transform(imageops::rotate90);
            room.quiesce();
            assert!(matches!(room.kid(0).op, Op::Idle));
            assert_eq!(room.kid(0).dims(), (120, 200));
            room.assert_converged();
        }

        #[test]
        fn a_friend_drawing_under_my_text_shows_through() {
            let mut room = Room::new(2);
            room.kid(0).set_tool(Tool::Text);
            room.kid(0).text_style.size_pt = 24.0;
            drag(room.kid(0), PointerButton::Primary, &[(20.0, 30.0)]);
            type_events(room.kid(0), vec![Event::Text("Hello".into())]);
            let bounds = room.kid(0).text.as_ref().unwrap().bounds;
            let y = bounds.center().y;
            room.kid(1).size_pencil = 3.0;
            line(room.kid(1), Tool::Pencil, BLUE, (0.0, y), (199.0, y));
            room.quiesce();
            // Still typing: the friend's line runs through the text box.
            assert!(room.kid(0).text.as_ref().unwrap().editing);
            assert_eq!(px(room.kid(0), 5, y as u32), BLUE);
            room.assert_converged();
        }

        #[test]
        fn moving_a_selection_while_a_friend_draws_under_it() {
            let mut room = Room::new(2);
            room.kid(0).size_pencil = 8.0;
            line(room.kid(0), Tool::Pencil, RED, (10.0, 20.0), (60.0, 20.0));
            room.quiesce();
            // Pick the red bar up and start dragging it down.
            room.kid(0).set_tool(Tool::Select);
            drag(room.kid(0), PointerButton::Primary, &[(5.0, 10.0), (66.0, 31.0)]);
            room.kid(0).begin(pos2(30.0, 20.0), PointerButton::Primary, Vec2::ZERO);
            room.kid(0).update(&[pos2(30.0, 80.0)], false, 1.0 / 60.0);
            // A friend's diagonal line crosses where the bar was.
            room.kid(1).size_pencil = 3.0;
            line(room.kid(1), Tool::Pencil, BLUE, (20.0, 0.0), (60.0, 119.0));
            room.quiesce();
            let crossing = (20.0 + 40.0 * 20.0 / 119.0) as u32;
            assert_eq!(px(room.kid(0), 50, 20), WHITE, "the hole stays empty, with no ghost of the bar");
            assert_eq!(px(room.kid(0), crossing, 20), BLUE, "the friend's line through the hole stays");
            room.kid(0).end();
            room.assert_converged();
            for i in 0..2 {
                assert_eq!(px(room.kid(i), 50, 20), WHITE);
                assert_eq!(px(room.kid(i), crossing, 20), BLUE);
                assert_eq!(px(room.kid(i), 45, 80), RED, "the bar moved");
            }
        }

        #[test]
        fn friends_see_what_im_drawing_before_i_finish() {
            let mut room = Room::new(2);
            room.kid(0).set_tool(Tool::Pencil);
            room.kid(0).primary = RED;
            room.kid(0).size_pencil = 5.0;
            room.kid(0).begin(pos2(10.0, 50.0), PointerButton::Primary, Vec2::ZERO);
            room.kid(0).update(&[pos2(150.0, 50.0)], false, 1.0 / 60.0);
            room.quiesce();
            let s = room.kid(1).session.as_ref().unwrap();
            assert!(s.has_previews());
            assert_eq!(s.overlay.get_pixel(100, 50).0, RED, "shown over the picture");
            assert_eq!(px(room.kid(1), 100, 50), WHITE, "but not part of it yet");
            room.kid(0).update(&[pos2(150.0, 100.0)], false, 1.0 / 60.0);
            room.quiesce();
            assert_eq!(room.kid(1).session.as_ref().unwrap().overlay.get_pixel(150, 90).0, RED, "follows along");
            room.kid(0).end();
            room.assert_converged();
            assert_eq!(px(room.kid(1), 100, 50), RED);
            assert_eq!(room.kid(1).session.as_ref().unwrap().overlay.get_pixel(100, 50).0, [0; 4]);
        }

        #[test]
        fn friends_drawing_is_merged_into_the_picture_on_screen() {
            let img = RgbaImage::from_pixel(4, 2, Rgba(WHITE));
            let mut over = RgbaImage::new(4, 2);
            over.put_pixel(2, 1, Rgba(RED));
            let shown = with_friends(&img, Some(&over), IRect::new(1, 0, 4, 2));
            assert_eq!(shown.size, [3, 2]);
            let px = |x: usize, y: usize| shown.pixels[y * 3 + x].to_array();
            assert_eq!(px(1, 1), RED, "the friend's pixel replaces mine");
            assert_eq!(px(0, 1), WHITE, "elsewhere the picture shows through");
            assert_eq!(px(2, 0), WHITE);
        }

        #[test]
        fn the_picture_on_screen_is_the_picture_with_its_transparency() {
            let img = RgbaImage::from_fn(5, 3, |x, y| Rgba([x as u8 * 50, y as u8 * 90, 7, (x * 60 + y * 5) as u8]));
            let r = IRect::new(1, 1, 5, 3);
            let src = &img;
            let crop: Vec<u8> = (1..3).flat_map(|y| (1..5).flat_map(move |x| src.get_pixel(x, y).0)).collect();
            let shown = with_friends(&img, None, r);
            assert_eq!(shown.pixels, ColorImage::from_rgba_unmultiplied([4, 2], &crop).pixels);
        }

        #[test]
        fn a_text_box_thrown_away_disappears_for_friends_too() {
            let mut room = Room::new(2);
            room.kid(0).set_tool(Tool::Text);
            drag(room.kid(0), PointerButton::Primary, &[(20.0, 30.0)]);
            type_events(room.kid(0), vec![Event::Text("Hi".into())]);
            room.quiesce();
            assert!(room.kid(1).session.as_ref().unwrap().has_previews());
            room.kid(0).discard_text();
            room.quiesce();
            assert!(!room.kid(1).session.as_ref().unwrap().has_previews());
            room.assert_converged();
        }

        #[test]
        fn kids_coming_and_going_are_announced() {
            let mut room = Room::new(1);
            room.join();
            room.quiesce();
            assert!(room.kid(0).toasts.iter().any(|t| t.text == "Kid 2 joined"));
            let out = room.hub.leave(2);
            room.route(out);
            room.quiesce();
            assert!(room.kid(0).toasts.iter().any(|t| t.text == "Kid 2 left"));
        }

        #[test]
        fn a_crowd_arriving_is_one_notice() {
            let mut room = Room::new(1);
            for _ in 0..5 {
                room.join();
            }
            room.quiesce();
            let texts: Vec<&str> = room.kid(0).toasts.iter().map(|t| t.text.as_str()).collect();
            assert_eq!(texts, ["5 friends joined"]);
        }

        fn toasts(a: &PaintApp) -> Vec<&str> {
            a.toasts.iter().map(|t| t.text.as_str()).collect()
        }

        #[test]
        fn the_host_can_kick_a_friend_out_and_everyone_hears_why() {
            let mut room = Room::new(3);
            room.quiesce();
            for i in 0..3 {
                room.kid(i).toasts.clear();
            }
            // Only the host can.
            room.kid(1).kick(3);
            room.quiesce();
            assert_eq!(room.kid(2).session.as_ref().map(|s| s.kids.len()), Some(3));

            line(room.kid(2), Tool::Pencil, RED, (10.0, 10.0), (60.0, 10.0));
            room.quiesce();
            // Still on its way when the kick lands: it stays theirs alone.
            line(room.kid(2), Tool::Pencil, BLUE, (10.0, 20.0), (60.0, 20.0));
            room.kid(0).kick(3);
            room.quiesce();
            let out = room.kid(2);
            assert!(out.session.is_none(), "out of the room");
            assert_eq!(out.error.as_deref(), Some("You have been kicked out by Kid 1.\n\nThe picture stays with you."));
            assert_eq!((px(out, 30, 10), px(out, 30, 20)), (RED, BLUE), "they keep the picture");
            assert_eq!(toasts(room.kid(1)), ["Kid 3 has been kicked out by Kid 1"]);
            assert_eq!(toasts(room.kid(0)), ["Kid 3 has been kicked out by you"]);
            assert_eq!(room.kid(1).session.as_ref().unwrap().kids.len(), 2);
            room.assert_converged();
            assert_eq!((px(room.kid(1), 30, 10), px(room.kid(1), 30, 20)), (RED, WHITE));
        }

        #[test]
        fn when_the_host_asks_first_friends_wait_until_let_in() {
            let mut room = Room::new(2);
            // Saved, but in the tests' own settings, never those of whoever runs them.
            assert!(settings_dir().is_some_and(|d| d.starts_with(std::env::temp_dir())));
            room.kid(0).set_ask_first(true);
            line(room.kid(0), Tool::Pencil, RED, (10.0, 10.0), (60.0, 10.0));
            room.quiesce();
            assert!(room.kid(1).session.as_ref().unwrap().ask_first, "everyone knows, for a new host to carry on");

            let maya = room.join();
            room.quiesce();
            assert!(room.kid(maya).session.is_none());
            assert_eq!(room.kid(maya).asking.as_deref(), Some("Kid 1"));
            let waiting: Vec<PeerId> = room.kid(0).session.as_ref().unwrap().waiting.iter().map(|k| k.id).collect();
            assert_eq!(waiting, [3]);
            assert!(room.kid(1).session.as_ref().unwrap().waiting.is_empty(), "only the host is asked");
            // Waiting, they see nothing of the drawing.
            line(room.kid(1), Tool::Pencil, BLUE, (10.0, 20.0), (60.0, 20.0));
            room.quiesce();
            assert!(room.down[maya].is_empty());

            room.kid(0).let_in(&[3], true);
            room.quiesce();
            assert!(room.kid(maya).asking.is_none());
            assert_eq!(px(room.kid(maya), 30, 20), BLUE);
            assert!(room.kid(0).session.as_ref().unwrap().waiting.is_empty());
            room.assert_converged();

            let sam = room.join();
            room.kid(0).let_in(&[4], false);
            room.quiesce();
            assert!(room.kid(sam).session.is_none() && room.kid(sam).asking.is_none());
            assert_eq!(room.kid(sam).error.as_deref(), Some("Kid 1 didn't let you in this time."));
            assert_eq!(room.kid(1).session.as_ref().unwrap().kids.len(), 3);
        }

        #[test]
        fn friends_see_my_pointer_and_tool() {
            let mut room = Room::new(2);
            room.kid(0).set_tool(Tool::Fill);
            room.kid(0).pointer = Some(pos2(42.0, 17.0));
            room.quiesce();
            let me = room.kids[0].0;
            let (p, _) = room.kid(1).session.as_ref().unwrap().pointers[&me];
            assert_eq!(p.at, Some((42.0, 17.0)));
            assert_eq!(p.tool, Held::Fill);
            // The eraser goes with its size and Color 2; the colour picker by name.
            room.kid(0).set_tool(Tool::Eraser);
            room.kid(0).size_eraser = 8.0;
            room.quiesce();
            let (p, _) = room.kid(1).session.as_ref().unwrap().pointers[&me];
            assert_eq!((p.tool, p.size, p.color), (Held::Eraser, 8.0, WHITE));
            room.kid(0).set_tool(Tool::Picker);
            room.quiesce();
            assert_eq!(room.kid(1).session.as_ref().unwrap().pointers[&me].0.tool, Held::Picker);
        }

        /// Run every app's network side until `done` holds (or fail after a while).
        fn run_until(kids: &mut [PaintApp], what: &str, done: impl Fn(&[PaintApp]) -> bool) {
            for _ in 0..1500 {
                for k in kids.iter_mut() {
                    k.pump_network();
                    k.flush_network();
                }
                if done(kids) {
                    return;
                }
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
            panic!("timed out waiting until {what}");
        }

        #[test]
        fn when_the_host_leaves_the_next_kid_takes_over() {
            // Real sockets on this computer: kid 0 hosts, kids 1 and 2 join.
            let mut kids: Vec<PaintApp> = (0..3).map(|_| app()).collect();
            kids[0].start_together();
            let port = kids[0].invite.expect("hosting").port();
            run_until(&mut kids, "the host is in", |k| k[0].session.is_some());
            for i in 1..3 {
                kids[i].join_together(format!("127.0.0.1:{port}"));
                run_until(&mut kids, "the kid is in", |k| k[i].session.is_some());
            }
            run_until(&mut kids, "everyone sees three kids", |k| k.iter().all(|a| a.session.as_ref().unwrap().kids.len() == 3));
            line(&mut kids[1], Tool::Pencil, RED, (10.0, 20.0), (100.0, 20.0));
            run_until(&mut kids, "kid 1's line reaches everyone", |k| k.iter().all(|a| a.img.get_pixel(50, 20).0 == RED));

            // The host leaves; kid 1 (the first to join) takes over and kid 2 follows.
            kids[0].leave_together();
            run_until(&mut kids, "kid 1 is the host and kid 2 is with them", |k| {
                let (a, b) = (k[1].session.as_ref().unwrap(), k[2].session.as_ref().unwrap());
                a.host == a.me && b.host == a.me && b.kids.len() == 2 && k[2].handover.is_none()
            });
            assert!(kids[1].toasts.iter().any(|t| t.text == "You're the host now"));
            assert_eq!(kids[2].img.get_pixel(50, 20).0, RED, "the picture carried over");

            // Drawing carries on, both ways.
            line(&mut kids[2], Tool::Pencil, BLUE, (10.0, 60.0), (100.0, 60.0));
            line(&mut kids[1], Tool::Pencil, RED, (10.0, 90.0), (100.0, 90.0));
            run_until(&mut kids, "both lines reach both", |k| {
                k[1..].iter().all(|a| a.img.get_pixel(50, 60).0 == BLUE && a.img.get_pixel(50, 90).0 == RED)
            });
            assert!(kids[1].img == kids[2].img);
        }

        /// A tiny random generator, so each seed replays the same session.
        struct Dice(u64);

        impl Dice {
            fn roll(&mut self, n: u64) -> u64 {
                self.0 ^= self.0 << 13;
                self.0 ^= self.0 >> 7;
                self.0 ^= self.0 << 17;
                self.0 % n
            }

            fn point(&mut self, a: &PaintApp) -> Pos2 {
                let (w, h) = a.dims();
                pos2(self.roll(w as u64) as f32 + 0.5, self.roll(h as u64) as f32 + 0.5)
            }
        }

        /// One random thing a kid might do.
        fn act(a: &mut PaintApp, dice: &mut Dice) {
            if !matches!(a.op, Op::Idle) {
                if dice.roll(3) == 0 {
                    a.end();
                } else {
                    let p = dice.point(a);
                    a.update(&[p], false, 1.0 / 60.0);
                }
                return;
            }
            let ([r, g, b], _) = PALETTE[dice.roll(PALETTE.len() as u64) as usize];
            a.primary = [r, g, b, 255];
            let p = dice.point(a);
            match dice.roll(14) {
                0..=2 => {
                    a.set_tool(Tool::Pencil);
                    a.size_pencil = 1.0 + dice.roll(8) as f32;
                    a.begin(p, PointerButton::Primary, Vec2::ZERO);
                }
                3..=4 => {
                    a.set_tool(Tool::Brush);
                    a.brush = BrushKind::ALL[dice.roll(9) as usize];
                    a.begin(p, PointerButton::Primary, Vec2::ZERO);
                }
                5 => {
                    a.set_tool(Tool::Fill);
                    drag(a, PointerButton::Primary, &[(p.x, p.y)]);
                }
                6 => {
                    a.set_tool(Tool::Shape);
                    a.shape = ShapeKind::ALL[dice.roll(21) as usize];
                    a.begin(p, PointerButton::Primary, Vec2::ZERO);
                }
                7 => {
                    a.set_tool(Tool::Text);
                    drag(a, PointerButton::Primary, &[(p.x, p.y)]);
                    type_events(a, vec![Event::Text("Hi".into())]);
                }
                8 => {
                    // Select a box, then start dragging it somewhere else.
                    a.set_tool(Tool::Select);
                    let q = dice.point(a);
                    drag(a, PointerButton::Primary, &[(p.x, p.y), (q.x, q.y)]);
                    if let Some(r) = a.sel.as_ref().map(|s| s.rect) {
                        let inside = pos2((r.x0 + r.x1) as f32 / 2.0, (r.y0 + r.y1) as f32 / 2.0);
                        a.begin(inside, PointerButton::Primary, Vec2::ZERO);
                    }
                }
                9 => a.commit_all(),
                10 => a.undo(),
                11 => a.redo(),
                12 => match dice.roll(6) {
                    0 => a.transform(imageops::rotate90),
                    1 => a.transform(imageops::rotate180),
                    2 => a.clear_image(),
                    _ => a.transform(raster::invert),
                },
                _ => {}
            }
            a.error = None;
        }

        #[test]
        fn random_sessions_always_end_up_identical() {
            let mut busy = [0; 4];
            for seed in 1..=24u64 {
                let mut dice = Dice(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1);
                let mut room = Room::new(3);
                for _ in 0..120 {
                    let i = dice.roll(3) as usize;
                    match dice.roll(5) {
                        0 | 1 => act(room.kid(i), &mut dice),
                        2 => _ = room.hub_takes(i),
                        _ => _ = room.kid_gets(i),
                    }
                }
                room.assert_converged();
                for (total, n) in busy.iter_mut().zip(room.busy) {
                    *total += n;
                }
            }
            // Make sure the hard cases really happened.
            println!("friends' edits arriving mid-stroke / over a live shape / text / floating selection: {busy:?}");
            assert!(busy.iter().all(|&n| n >= 10), "{busy:?}");
        }
    }
}

/// A friend's coloured badge with their initial.
fn paint_avatar(ui: &mut Ui, name: &str, [r, g, b]: [u8; 3], size: f32) {
    let (rect, _) = ui.allocate_exact_size(Vec2::splat(size), Sense::hover());
    ui.painter().circle_filled(rect.center(), size / 2.0, Color32::from_rgb(r, g, b));
    let initial: String = name.chars().next().map(|c| c.to_uppercase().collect()).unwrap_or_default();
    ui.painter().text(rect.center(), Align2::CENTER_CENTER, initial, FontId::proportional(size * 0.6), Color32::WHITE);
}

/// One line in the Friends list. For the host, `kick` names the friend, and a
/// cross shows on hover to send them out; true when it's clicked.
fn kid_row(ui: &mut Ui, name: &str, color: [u8; 3], text: Color32, kick: Option<&str>) -> bool {
    let row = ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = 5.0;
        paint_avatar(ui, name, color, 16.0);
        ui.label(RichText::new(name).color(text));
        kick.map(|who| {
            let (rect, resp) = ui.allocate_exact_size(Vec2::splat(14.0), Sense::click());
            (rect, resp.on_hover_text(format!("Kick out {who}")).on_hover_cursor(CursorIcon::PointingHand))
        })
    });
    let Some((rect, resp)) = row.inner else {
        return false;
    };
    if ui.rect_contains_pointer(row.response.rect) {
        let red = Color32::from_rgb(0xC4, 0x2B, 0x1C);
        if resp.hovered() {
            ui.painter().circle_filled(rect.center(), 7.0, red.gamma_multiply(0.15));
        }
        let (c, d) = (rect.center(), 3.0);
        let stroke = Stroke::new(1.5, if resp.hovered() { red } else { text.gamma_multiply(0.6) });
        ui.painter().line_segment([c - vec2(d, d), c + vec2(d, d)], stroke);
        ui.painter().line_segment([c + vec2(-d, d), c + vec2(d, -d)], stroke);
    }
    resp.clicked()
}

/// A notice at the door, at the bottom of the picture: maybe a friend's badge,
/// what's happening, then a round green tick (when there's a yes to give) and
/// red cross. Returns the answer clicked.
fn door_pill(ui: &mut Ui, who: Option<(&str, [u8; 3])>, text: &str, tick: Option<&str>, cross: &str) -> Option<bool> {
    let mut answer = None;
    let frame = egui::Frame::new()
        .fill(Color32::from_rgba_unmultiplied(40, 40, 48, 235))
        .corner_radius(18.0)
        .inner_margin(egui::Margin { left: if who.is_some() { 8 } else { 16 }, right: 6, top: 6, bottom: 6 });
    frame.show(ui, |ui| {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 8.0;
            if let Some((name, color)) = who {
                paint_avatar(ui, name, color, 22.0);
            }
            let label = egui::Label::new(RichText::new(text).size(13.0).color(Color32::WHITE)).selectable(false).extend();
            ui.add(label);
            ui.add_space(2.0);
            ui.spacing_mut().item_spacing.x = 5.0;
            if let Some(tip) = tick
                && answer_button(ui, true, tip).clicked()
            {
                answer = Some(true);
            }
            if answer_button(ui, false, cross).clicked() {
                answer = Some(false);
            }
        });
    });
    answer
}

/// A round tick (green) or cross (red) button.
fn answer_button(ui: &mut Ui, yes: bool, tip: &str) -> Response {
    let (rect, resp) = ui.allocate_exact_size(Vec2::splat(24.0), Sense::click());
    let resp = resp.on_hover_text(tip).on_hover_cursor(CursorIcon::PointingHand);
    let (base, lit) = if yes {
        (Color32::from_rgb(0x2E, 0x9E, 0x44), Color32::from_rgb(0x3D, 0xB8, 0x56))
    } else {
        (Color32::from_rgb(0xD2, 0x3A, 0x32), Color32::from_rgb(0xE8, 0x4E, 0x45))
    };
    let (painter, c) = (ui.painter(), rect.center());
    painter.circle_filled(c, 12.0, if resp.hovered() { lit } else { base });
    let stroke = Stroke::new(2.2, Color32::WHITE);
    if yes {
        painter.add(egui::Shape::line(vec![c + vec2(-5.0, 0.5), c + vec2(-1.5, 4.0), c + vec2(5.0, -3.5)], stroke));
    } else {
        painter.line_segment([c - vec2(4.0, 4.0), c + vec2(4.0, 4.0)], stroke);
        painter.line_segment([c + vec2(-4.0, 4.0), c + vec2(4.0, -4.0)], stroke);
    }
    resp
}

/// A drawing in the Join dialog's list: the host's badge, name and how many are drawing.
fn nearby_row(ui: &mut Ui, n: &crate::collab::discovery::Nearby, pal: &Palette) -> Response {
    let (rect, resp) = ui.allocate_exact_size(vec2(ui.available_width(), 30.0), Sense::click());
    let resp = resp.on_hover_cursor(if n.compatible { CursorIcon::PointingHand } else { CursorIcon::NotAllowed });
    let painter = ui.painter();
    if resp.hovered() && n.compatible {
        painter.rect(rect, 4.0, pal.hover, Stroke::new(1.0, pal.hover_border), egui::StrokeKind::Inside);
    }
    let initial: String = n.name.chars().next().map(|c| c.to_uppercase().collect()).unwrap_or_default();
    let badge = rect.left_center() + vec2(16.0, 0.0);
    painter.circle_filled(badge, 10.0, Color32::from_rgb(0x4C, 0x9B, 0xE8));
    painter.text(badge, Align2::CENTER_CENTER, initial, FontId::proportional(12.0), Color32::WHITE);
    let kids = if n.kids == 1 { "1 kid".to_owned() } else { format!("{} kids", n.kids) };
    let text = if n.compatible {
        format!("{}'s drawing  ·  {kids}", n.name)
    } else {
        format!("{}'s drawing  (different version of Paint)", n.name)
    };
    let color = if n.compatible { pal.text } else { pal.text_weak };
    painter.text(rect.left_center() + vec2(34.0, 0.0), Align2::LEFT_CENTER, text, FontId::proportional(13.0), color);
    resp
}

/// A narrow button at the end of a short list, like the "more" arrow on Paint's
/// galleries, with how many are hidden above it.
fn more_button(ui: &mut Ui, count: &str, pal: &Palette) -> Response {
    let (rect, resp) = ui.allocate_exact_size(vec2(26.0, CONTENT_H - 8.0), Sense::click());
    let resp = resp.on_hover_cursor(CursorIcon::PointingHand);
    let painter = ui.painter();
    if resp.hovered() || resp.is_pointer_button_down_on() {
        painter.rect_filled(rect, ribbon::HIGHLIGHT_RADIUS, pal.hover);
        painter.rect_stroke(rect, ribbon::HIGHLIGHT_RADIUS, Stroke::new(1.0, pal.hover_border), StrokeKind::Inside);
    } else {
        painter.rect_stroke(rect, ribbon::HIGHLIGHT_RADIUS, Stroke::new(1.0, pal.ribbon_border), StrokeKind::Inside);
    }
    painter.text(rect.center_top() + vec2(0.0, 16.0), Align2::CENTER_CENTER, count, FontId::proportional(11.0), pal.text);
    icons::paint(Icon::GalleryMore, painter, icons::square(rect.center_bottom() - vec2(0.0, 14.0), 12.0), true);
    resp
}
