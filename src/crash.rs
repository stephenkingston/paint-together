//! When Paint breaks, say so. Started from Explorer, Finder or a desktop menu there's
//! no console to print to, so a crash would otherwise vanish without a word.

use std::path::PathBuf;
use std::sync::OnceLock;
use std::time::{SystemTime, UNIX_EPOCH};

/// What Paint draws the window with, for the report: most crashes on start are the graphics driver's.
static GRAPHICS: OnceLock<String> = OnceLock::new();

pub fn note_graphics(adapter: String) {
    let _ = GRAPHICS.set(adapter);
}

/// Keep the details of any panic, on any thread, in the crash report.
pub fn watch() {
    let print = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        print(info);
        let payload = info.payload();
        let what = payload.downcast_ref::<&str>().copied().or_else(|| payload.downcast_ref::<String>().map(String::as_str));
        let at = info.location().map_or(String::new(), |l| format!(" at {}:{}", l.file(), l.line()));
        let thread = std::thread::current();
        let backtrace = std::backtrace::Backtrace::force_capture();
        save(&format!("Thread '{}' panicked{at}:\n{}\n\n{backtrace}", thread.name().unwrap_or("unnamed"), what.unwrap_or("(no message)")));
    }));
}

fn report_file() -> Option<PathBuf> {
    crate::app::settings_dir().map(|d| d.join("crash.txt"))
}

/// Write the crash report (replacing the last one).
pub fn save(details: &str) {
    let Some(file) = report_file() else { return };
    if let Some(dir) = file.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let (os, arch, version) = (std::env::consts::OS, std::env::consts::ARCH, env!("CARGO_PKG_VERSION"));
    let when = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs());
    let graphics = GRAPHICS.get().map_or("not started", String::as_str);
    let _ = std::fs::write(file, format!("Paint {version} on {os} {arch}, {when} seconds into 1970\nGraphics: {graphics}\n\n{details}\n"));
}

/// Tell whoever is in front of the screen, pointing at the crash report.
pub fn tell(what: &str) {
    let more = report_file().filter(|f| f.exists()).map(|f| format!("\n\nThe details are in {}", f.display()));
    rfd::MessageDialog::new()
        .set_level(rfd::MessageLevel::Error)
        .set_title("Paint")
        .set_description(format!("{what}{}", more.unwrap_or_default()))
        .show();
}
