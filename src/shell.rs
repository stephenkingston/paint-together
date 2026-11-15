//! The window around the app, drawn without a GPU.
//!
//! winit opens the window and reports input, egui-winit turns that into egui's input and
//! carries out what the app asks of the window (title, full screen, close, cursor,
//! clipboard), and `soft` draws egui's triangles into the window's pixels, which
//! softbuffer hands to the OS (X11, Wayland, Windows, macOS). This is what eframe did
//! for us, for one window, minus wgpu and the graphics driver.

use std::collections::VecDeque;
use std::num::NonZeroU32;
use std::sync::Arc;
use std::time::{Duration, Instant};

use egui::{ViewportBuilder, ViewportCommand, ViewportEvent, ViewportId, ViewportInfo};
use egui_winit::accesskit_winit;
use egui_winit::winit;
use winit::application::ApplicationHandler;
use winit::event::WindowEvent;
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop, EventLoopProxy};
use winit::window::{Window, WindowId};

use crate::app::PaintApp;
use crate::soft;

/// Before anything is drawn (egui's panels then cover the whole window).
const CLEAR: u32 = 0x0C0C0C;

enum UserEvent {
    /// egui asked for a frame at `when`, during pass `pass`.
    Repaint { when: Instant, pass: u64 },
    AccessKit(accesskit_winit::Event),
}

impl From<accesskit_winit::Event> for UserEvent {
    fn from(e: accesskit_winit::Event) -> Self {
        Self::AccessKit(e)
    }
}

type MakeApp = Box<dyn FnOnce(&egui::Context) -> PaintApp>;

pub fn run(viewport: ViewportBuilder, make: impl FnOnce(&egui::Context) -> PaintApp + 'static) -> Result<(), String> {
    let event_loop = EventLoop::<UserEvent>::with_user_event().build().map_err(|e| e.to_string())?;
    let ctx = egui::Context::default();
    ctx.set_embed_viewports(false);
    // egui may run a second pass when the first changed the layout (Context::request_discard)
    ctx.options_mut(|o| o.max_passes = std::num::NonZeroUsize::MIN.saturating_add(1));
    let proxy = event_loop.create_proxy();
    let wake = proxy.clone();
    ctx.set_request_repaint_callback(move |info| {
        let when = Instant::now().checked_add(info.delay).unwrap_or_else(|| Instant::now() + Duration::from_secs(86400));
        let _ = wake.send_event(UserEvent::Repaint { when, pass: info.current_cumulative_pass_nr });
    });
    let threads = std::env::var("PAINT_RENDER_THREADS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or_else(|| std::thread::available_parallelism().map_or(1, |n| n.get()).min(8));
    let mut shell = Shell {
        ctx,
        proxy,
        viewport,
        make: Some(Box::new(make)),
        app: None,
        win: None,
        repaint_at: None,
        close: false,
        threads,
        stats: std::env::var_os("PAINT_FRAME_STATS").map(|_| Stats::default()),
    };
    event_loop.run_app(&mut shell).map_err(|e| e.to_string())
}

struct Win {
    window: Arc<Window>,
    state: egui_winit::State,
    surface: softbuffer::Surface<Arc<Window>, Arc<Window>>,
    renderer: soft::Renderer,
    info: ViewportInfo,
    /// The window starts hidden and is shown once its first frame is drawn.
    shown: bool,
    /// Size and scale of the last frame drawn
    drawn: Option<([usize; 2], f32)>,
    /// What changed in recent frames, newest first: a window buffer from `age` frames ago
    /// needs all of those redrawn.
    history: VecDeque<Vec<soft::Rect>>,
    /// The last frame, kept here where the platform doesn't keep it (softbuffer hands out a
    /// new, blank buffer every frame on macOS).
    kept: Vec<u32>,
    /// The next redraw is one we asked for. Otherwise the OS asked (the window was
    /// uncovered, say), and all of it is drawn again.
    asked: bool,
}

impl Win {
    fn redraw(&mut self) {
        self.asked = true;
        self.window.request_redraw();
    }
}

/// Keep the last frame ourselves (see [`Win::kept`]).
const KEEP_FRAME: bool = cfg!(target_os = "macos");

struct Shell {
    ctx: egui::Context,
    proxy: EventLoopProxy<UserEvent>,
    viewport: ViewportBuilder,
    make: Option<MakeApp>,
    app: Option<PaintApp>,
    win: Option<Win>,
    /// When egui wants its next frame.
    repaint_at: Option<Instant>,
    /// Closing was asked for and the app didn't cancel it.
    close: bool,
    threads: usize,
    stats: Option<Stats>,
}

/// `PAINT_FRAME_STATS=1`: time per frame, printed every 60 frames.
#[derive(Default)]
struct Stats {
    frames: u32,
    ui: Duration,
    tessellate: Duration,
    render: Duration,
    present: Duration,
    worst: Duration,
    /// Share of the window redrawn, summed over frames
    redrawn: f64,
    /// Frames where nothing on screen changed
    unchanged: u32,
}

impl ApplicationHandler<UserEvent> for Shell {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.win.is_none()
            && let Err(e) = self.open(event_loop)
        {
            eprintln!("Paint couldn't open its window: {e}");
            event_loop.exit();
        }
    }

    fn window_event(&mut self, event_loop: &ActiveEventLoop, _: WindowId, event: WindowEvent) {
        let Some(win) = &mut self.win else { return };
        match &event {
            WindowEvent::RedrawRequested => {
                self.paint(event_loop);
                return;
            }
            WindowEvent::CloseRequested => {
                if self.close {
                    event_loop.exit();
                    return;
                }
                // The app decides (it may ask to save first): it sees the request on the next frame.
                win.info.events.push(ViewportEvent::Close);
            }
            WindowEvent::Occluded(occluded) => win.info.occluded = Some(*occluded),
            WindowEvent::Resized(_) => win.redraw(),
            _ => {}
        }
        let response = win.state.on_window_event(&win.window, &event);
        if response.repaint || matches!(event, WindowEvent::CloseRequested | WindowEvent::Occluded(false)) {
            win.redraw();
        }
    }

    fn user_event(&mut self, _: &ActiveEventLoop, event: UserEvent) {
        let Some(win) = &mut self.win else { return };
        match event {
            UserEvent::Repaint { when, pass } => {
                // Ignore requests a frame since has already answered
                let now = self.ctx.cumulative_pass_nr_for(ViewportId::ROOT);
                if now == pass || now == pass + 1 {
                    self.repaint_at = Some(self.repaint_at.map_or(when, |t| t.min(when)));
                }
            }
            UserEvent::AccessKit(e) => match e.window_event {
                accesskit_winit::WindowEvent::InitialTreeRequested => {
                    self.ctx.enable_accesskit();
                    win.redraw();
                }
                accesskit_winit::WindowEvent::ActionRequested(request) => {
                    win.state.on_accesskit_action_request(request);
                    win.redraw();
                }
                accesskit_winit::WindowEvent::AccessibilityDeactivated => self.ctx.disable_accesskit(),
            },
        }
    }

    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {
        match (self.repaint_at, &mut self.win) {
            (Some(t), Some(win)) if t <= Instant::now() => {
                self.repaint_at = None;
                win.redraw();
                event_loop.set_control_flow(ControlFlow::Wait);
            }
            (Some(t), Some(_)) => event_loop.set_control_flow(ControlFlow::WaitUntil(t)),
            _ => event_loop.set_control_flow(ControlFlow::Wait),
        }
    }
}

impl Shell {
    fn open(&mut self, event_loop: &ActiveEventLoop) -> Result<(), String> {
        let builder = self.viewport.clone().with_visible(false);
        let window = Arc::new(egui_winit::create_window(&self.ctx, event_loop, &builder).map_err(|e| e.to_string())?);
        let context = softbuffer::Context::new(window.clone()).map_err(|e| e.to_string())?;
        let surface = softbuffer::Surface::new(&context, window.clone()).map_err(|e| e.to_string())?;
        let mut state = egui_winit::State::new(
            self.ctx.clone(),
            ViewportId::ROOT,
            &window,
            Some(window.scale_factor() as f32),
            window.theme(),
            Some(8192),
        );
        // (Before the window is shown, as AccessKit asks)
        state.init_accesskit(event_loop, &window, self.proxy.clone());
        let mut info = ViewportInfo::default();
        egui_winit::update_viewport_info(&mut info, &self.ctx, &window, true);
        if let Some(icon) = &self.viewport.icon {
            set_app_icon(&window, icon);
        }
        if let Some(make) = self.make.take() {
            self.app = Some(make(&self.ctx));
        }
        self.win = Some(Win {
            window,
            state,
            surface,
            renderer: soft::Renderer::default(),
            info,
            shown: false,
            drawn: None,
            history: VecDeque::new(),
            kept: Vec::new(),
            asked: false,
        });
        // Drawn now rather than on request: Windows sends no redraw to a window that isn't shown
        // yet, and it's shown once this first frame is drawn.
        self.paint(event_loop);
        Ok(())
    }

    fn paint(&mut self, event_loop: &ActiveEventLoop) {
        let (Some(win), Some(app)) = (&mut self.win, &mut self.app) else { return };
        egui_winit::update_viewport_info(&mut win.info, &self.ctx, &win.window, false);
        if win.shown && !win.info.visible().unwrap_or(true) {
            // Minimized or hidden: nothing to draw until it's back (winit tells us).
            return;
        }
        let t0 = Instant::now();
        let mut input = win.state.take_egui_input(&win.window);
        input.viewports = std::iter::once((ViewportId::ROOT, win.info.clone())).collect();
        let close_requested = input.viewport().close_requested();
        let out = self.ctx.run_ui(input, |ui| app.ui(ui));
        win.info.events.clear();
        let commands = out.viewport_output.get(&ViewportId::ROOT).map(|v| v.commands.clone()).unwrap_or_default();
        if close_requested && !commands.contains(&ViewportCommand::CancelClose) {
            self.close = true;
        }
        win.state.handle_platform_output_with_event_loop(&win.window, event_loop, out.platform_output);

        let t1 = Instant::now();
        let prims = self.ctx.tessellate(out.shapes, out.pixels_per_point);
        win.renderer.set_textures(&out.textures_delta);
        let t2 = Instant::now();
        let mut t3 = t2;
        let size = win.window.inner_size();
        let asked_by_os = !std::mem::take(&mut win.asked);
        let mut redrawn = None;
        if let (Some(w), Some(h)) = (NonZeroU32::new(size.width), NonZeroU32::new(size.height)) {
            let (px, ppp) = ([w.get() as usize, h.get() as usize], out.pixels_per_point);
            let whole = soft::Rect::of_size(px);
            // Only what looks different from the last frame is drawn and shown again
            let mut changed = win.renderer.damage(&prims, &out.textures_delta, px, ppp);
            if asked_by_os || win.drawn != Some((px, ppp)) {
                changed = vec![whole];
            }
            if !changed.is_empty() && win.surface.resize(w, h).is_ok() {
                win.history.push_front(changed.clone());
                win.history.truncate(4);
                let result = if KEEP_FRAME {
                    if win.kept.len() != px[0] * px[1] {
                        win.kept = vec![CLEAR; px[0] * px[1]];
                        changed = vec![whole];
                    }
                    win.renderer.render(&mut win.kept, px, ppp, CLEAR, &prims, &changed, self.threads);
                    t3 = Instant::now();
                    win.surface.buffer_mut().and_then(|mut buf| {
                        buf.copy_from_slice(&win.kept);
                        buf.present()
                    })
                } else {
                    win.surface.buffer_mut().and_then(|mut buf| {
                        // The buffer shows the frame from `age` frames ago (0: unknown)
                        let age = buf.age() as usize;
                        if age == 0 || age > win.history.len() {
                            changed = vec![whole];
                        } else if age > 1 {
                            changed = soft::merge(win.history.iter().take(age).flatten().copied().collect(), whole);
                        }
                        win.renderer.render(&mut buf, px, ppp, CLEAR, &prims, &changed, self.threads);
                        t3 = Instant::now();
                        let damage: Vec<softbuffer::Rect> = changed
                            .iter()
                            .filter_map(|r| {
                                Some(softbuffer::Rect {
                                    x: r.x0 as u32,
                                    y: r.y0 as u32,
                                    width: NonZeroU32::new((r.x1 - r.x0) as u32)?,
                                    height: NonZeroU32::new((r.y1 - r.y0) as u32)?,
                                })
                            })
                            .collect();
                        buf.present_with_damage(&damage)
                    })
                };
                match result {
                    Ok(()) => win.drawn = Some((px, ppp)),
                    Err(e) => {
                        eprintln!("Paint couldn't show its window: {e}");
                        win.drawn = None;
                    }
                }
                redrawn = Some(changed.iter().map(soft::Rect::area).sum::<i64>() as f64 / whole.area() as f64);
            }
        }
        win.renderer.free_textures(&out.textures_delta);
        if let Some(s) = &mut self.stats {
            let t4 = Instant::now();
            s.add(t1 - t0, t2 - t1, t3 - t2, t4 - t3, redrawn);
        }
        if !win.shown {
            win.shown = true;
            win.window.set_visible(true);
        }

        let mut actions = Vec::new();
        egui_winit::process_viewport_commands(&self.ctx, &mut win.info, commands, &win.window, &mut actions);
        for action in actions {
            match action {
                egui_winit::ActionRequested::Cut => win.state.egui_input_mut().events.push(egui::Event::Cut),
                egui_winit::ActionRequested::Copy => win.state.egui_input_mut().events.push(egui::Event::Copy),
                egui_winit::ActionRequested::Paste => {
                    if let Some(text) = win.state.clipboard_text().map(|t| t.replace("\r\n", "\n"))
                        && !text.is_empty()
                    {
                        win.state.egui_input_mut().events.push(egui::Event::Paste(text));
                    }
                }
                egui_winit::ActionRequested::Screenshot(_) => {}
            }
        }
        if !win.info.events.is_empty() {
            // e.g. the app asked to close: it sees that next frame
            win.redraw();
        }
        if self.close {
            event_loop.exit();
        }
    }
}

impl Stats {
    fn add(&mut self, ui: Duration, tessellate: Duration, render: Duration, present: Duration, redrawn: Option<f64>) {
        self.frames += 1;
        match redrawn {
            Some(r) => self.redrawn += r,
            None => self.unchanged += 1,
        }
        self.ui += ui;
        self.tessellate += tessellate;
        self.render += render;
        self.present += present;
        self.worst = self.worst.max(ui + tessellate + render + present);
        if self.frames == 60 {
            let ms = |d: Duration| d.as_secs_f64() * 1000.0 / 60.0;
            eprintln!(
                "frame ms (avg of 60): ui {:.2}  tessellate {:.2}  render {:.2}  present {:.2}  | worst total {:.2} | redrawn {:.1}% of the window, {} frames unchanged",
                ms(self.ui),
                ms(self.tessellate),
                ms(self.render),
                ms(self.present),
                self.worst.as_secs_f64() * 1000.0,
                self.redrawn * 100.0 / 60.0,
                self.unchanged
            );
            *self = Self::default();
        }
    }
}

/// The icon in the taskbar (Windows) or the Dock (macOS, when not run from the app bundle);
/// the window's own icon is set by egui-winit.
fn set_app_icon(window: &Window, icon: &egui::IconData) {
    #[cfg(windows)]
    {
        use winit::platform::windows::WindowExtWindows as _;
        if let Ok(i) = winit::window::Icon::from_rgba(icon.rgba.clone(), icon.width, icon.height) {
            window.set_taskbar_icon(Some(i));
        }
    }
    #[cfg(target_os = "macos")]
    mac::set_dock_icon(icon);
    let _ = (window, icon);
}

#[cfg(target_os = "macos")]
mod mac {
    use objc2::AnyThread as _;
    use objc2::MainThreadMarker;
    use objc2_app_kit::{NSApplication, NSBitmapImageRep, NSDeviceRGBColorSpace, NSImage};
    use objc2_foundation::NSSize;

    pub fn set_dock_icon(icon: &egui::IconData) {
        let Some(mtm) = MainThreadMarker::new() else { return };
        let app = NSApplication::sharedApplication(mtm);
        let (w, h) = (icon.width as isize, icon.height as isize);
        if icon.rgba.len() != (w * h * 4) as usize {
            return;
        }
        // SAFETY: with no planes given, the image rep allocates its own w × h × 4 bytes,
        // which are filled before it's used.
        unsafe {
            let Some(rep) = NSBitmapImageRep::initWithBitmapDataPlanes_pixelsWide_pixelsHigh_bitsPerSample_samplesPerPixel_hasAlpha_isPlanar_colorSpaceName_bytesPerRow_bitsPerPixel(
                NSBitmapImageRep::alloc(),
                std::ptr::null_mut(),
                w,
                h,
                8,
                4,
                true,
                false,
                NSDeviceRGBColorSpace,
                w * 4,
                32,
            ) else {
                return;
            };
            let data = rep.bitmapData();
            if data.is_null() {
                return;
            }
            std::ptr::copy_nonoverlapping(icon.rgba.as_ptr(), data, icon.rgba.len());
            let image = NSImage::initWithSize(NSImage::alloc(), NSSize::new(w as f64, h as f64));
            image.addRepresentation(&rep);
            app.setApplicationIconImage(Some(&image));
        }
    }
}
