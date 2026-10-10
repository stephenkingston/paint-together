//! Paint: a from-scratch Rust reimplementation of the Windows Paint app.

mod app;
mod brush;
mod collab;
mod history;
mod icons;
mod live_shape;
mod logo;
mod raster;
mod ribbon;
mod shapes;
#[cfg(test)]
mod tests;
mod text;
mod text_edit;
mod theme;

use eframe::egui;

fn main() -> eframe::Result {
    let mut args = std::env::args_os().skip(1);
    let first = args.next();
    // `--export-icon PATH [SIZE]`: used by the packaging scripts.
    if first.as_deref() == Some(std::ffi::OsStr::new("--export-icon")) {
        let out = args.next().map(std::path::PathBuf::from).unwrap_or_else(|| "AppIcon.png".into());
        let size = args.next().and_then(|s| s.to_str()?.parse().ok()).unwrap_or(1024);
        if let Err(e) = logo::render(size).save(&out) {
            eprintln!("couldn't write {}: {e}", out.display());
            std::process::exit(1);
        }
        return Ok(());
    }
    // `--join ADDRESS`: join a friend's drawing straight away; otherwise a picture to open.
    let (initial, join) = if first.as_deref() == Some(std::ffi::OsStr::new("--join")) {
        (None, args.next().and_then(|a| a.into_string().ok()))
    } else {
        (first.map(std::path::PathBuf::from), None)
    };
    let icon = logo::render(256);
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("Untitled - Paint")
            .with_app_id("paint")
            .with_inner_size([1460.0, 900.0])
            .with_min_inner_size([760.0, 480.0])
            .with_drag_and_drop(true)
            // Draw our own title bar under the traffic lights, like modern Mac apps.
            .with_fullsize_content_view(true)
            .with_titlebar_shown(false)
            .with_title_shown(false)
            .with_icon(egui::IconData { width: icon.width(), height: icon.height(), rgba: icon.into_raw() }),
        ..Default::default()
    };
    eframe::run_native("Paint", options, Box::new(move |cc| Ok(Box::new(app::PaintApp::new(cc, initial, join)))))
}
