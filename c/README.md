# Paint in C

A port of Paint (the Rust app in `../src`) to pure C, for **Linux, Windows and macOS**, built to be small and light:

| | Binary (stripped) | Memory, idle window |
|---|---|---|
| Linux x86_64 | 381 KB, needs only libc + libm | 11.8 MB RSS (Rust: 139 MB) |
| Windows x64 | 407 KB, one .exe, system DLLs only (Windows 7 and later; the 32-bit build runs on XP SP3) | 24.3 MB working set, 11.4 MB private (Rust 0.3.1: 78 MB) |
| macOS arm64 | 373 KB | 34 MB footprint (a bare Cocoa window is 16 MB) |

Undo steps are stored packed (runs, and the change against the picture before), so a whole-picture fill on a
4000×3000 picture costs kilobytes instead of 96 MB, with the same results as the Rust version's undo. A stroke, a
shape, the text box or a moved selection keeps the picture under it only in the 64×64 tiles it has drawn on (the Rust
keeps a whole copy each), and font and picture files are mapped rather than read into memory.

It draws the whole window into a software framebuffer with its own drawing code and widget kit, and a thin host per
platform shows it: the X11 wire protocol over the socket (no Xlib, MIT-SHM; runs under XWayland on Wayland desktops),
Win32 + GDI, and Cocoa through the Objective-C runtime. The only third-party code is the vendored
[stb](https://github.com/nothings/stb) headers (`third_party/stb`, with two hooks added to `stb_image.h`). Drawing together is wire-compatible with the Rust
Paint: C and Rust Paints find each other's drawings and draw together.

## Build

```sh
make                      # Linux: build/linux/paint
make TARGET=win64         # Windows x64 (cross, x86_64-w64-mingw32): build/win64/paint.exe, Windows 7 and later
make TARGET=win32         # Windows 32-bit (i686-w64-mingw32): XP SP3 and later, no SSE needed
make TARGET=win32 check   # the .exe imports nothing newer than XP (win64: than Windows 7)
make TARGET=mac           # on a Mac (Apple clang): build/mac/paint; MAC_ARCH=x86_64 for Intel
make TARGET=mac bundle    # build/mac/Paint.app
make test                 # the tests (they draw into memory; no display needed)
make size                 # bytes per object
make bench                # timings of what a user feels (what decides the -O2 files in the Makefile's HOT)
make DEBUG=1              # with symbols
make NOASM=1              # the kernels' C instead of their assembly (build/<target>-noasm)
```

Files named `*_linux.c`, `*_posix.c`, `*_win.c` and `*_mac.c` are built only for those platforms.

## Layout

| Folder | What's in it |
|---|---|
| `src/os/` | Threads, time, sockets, files, settings folder (`os_posix.c`, `os_win.c`) |
| `src/core/` | RGBA pictures, picture files (stb + small ICO/TIFF/GIF codecs), raw deflate; the kernels (below) |
| `src/paint/` | The pixel engine: masks, brushes, shapes, live shapes, text, undo (Rust `raster.rs` … `history.rs`) |
| `src/collab/` | Drawing together: patches, bincode-compatible messages, hub, network, mDNS (Rust `collab/`) |
| `src/ui/` | Drawing into the framebuffer, fonts (stb_truetype), icons, the immediate-mode widget kit and ribbon |
| `src/host/` | The platform hosts: X11 (+ XDG portal pickers over D-Bus), Win32, Cocoa, headless (tests) |
| `src/app/` | The app: state and behaviour (`app*.c`) and what it draws (`view_*.c`) |
| `tests/` | `test_*.c` run by `make test`; `demo_*.c` and `render_kit.c` are run by hand; `bench.c` by `make bench` |

## The kernels: assembly

Everything is built for size (`-Oz`) but for a few files of per-pixel loops (the Makefile's `HOT`). What kept stb's
picture code built for speed is now a handful of loops in assembly, `src/core/kernels_x86_64.S` (Linux, Windows x64,
Intel Macs) and `src/core/kernels_arm64.S` (Apple silicon): deflate, CRC-32 and Adler-32, PNG's row filters both ways,
and inflate. PNGs and zlib streams are written by `img.c` around them, the same bytes as stb_image_write's, and
stb_image reads through them by two hooks. `kernels.c` holds the same loops in C, for other CPUs (the 32-bit Windows
build) and for `tests/test_kernels.c`, which checks the assembly against the C and both against stb as it came: the
same PNG and zlib bytes, and the same pixels and errors reading back, also from damaged streams.

## Differences from the Rust version

- No input methods on X11 (keyboard layouts work; IBus/fcitx composition doesn't).
- WebP pictures can't be opened or saved.
- On X11 the pointer shapes are the classic X cursors, not the desktop's cursor theme.
- `PAINT_CONFIG_DIR` overrides the settings folder (the tests use it); `PAINT_SCALE=150` sets the UI scale on X11.
- Settings are kept in `~/.config/paint` (Linux), `%APPDATA%\Paint` (Windows) and `~/.config/paint` (macOS).
