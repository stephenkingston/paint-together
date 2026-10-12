Classic Microsoft Paint (the Windows 7/10 ribbon edition), rebuilt in Rust, where kids on the same Wi-Fi draw on one picture together in real time: start a drawing on the **Together** tab, and friends join it in a tap. See the [README](https://github.com/stephenkingston/paint-together#readme) for the full tour.

Also on crates.io: `cargo install paint-together` installs the `paint` command.

### New in this release

- **Graphics memory is set aside as in 0.3.0 again.** 0.3.1 asked the graphics driver for memory in small blocks, to start up smaller; Paint is back to the usual larger blocks. Everything else that made 0.3.1 lighter stays: the smaller app, fonts loaded only once, and up to 256 MB of undo history.
- Draws together with 0.3.0 and 0.3.1.

For everything else that changed recently, see the [0.3.1 release notes](https://github.com/stephenkingston/paint-together/releases/tag/v0.3.1).

### Linux (x86_64)

```sh
tar xzf paint-*-linux-x86_64.tar.gz
cd paint-*-linux-x86_64
./paint                 # run it from here, or
./install.sh            # install for your user: ~/.local/bin/paint plus an app-launcher entry
```

Requirements: an x86_64 distribution with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, Arch and later), X11 or Wayland, Vulkan or OpenGL graphics drivers, fontconfig (for the Text tool's fonts) and an XDG desktop portal (for the Open/Save dialogs; standard on desktop setups).

Check the download with `sha256sum -c paint-*-linux-x86_64.tar.gz.sha256`.

### Windows (x86_64)

Unzip `paint-*-windows-x86_64.zip` and run `paint.exe`. It needs nothing else installed. The app isn't code-signed, so the first time Windows SmartScreen may say it "protected your PC": choose **More info**, then **Run anyway**. For drawing together, allow Paint on private networks when Windows Firewall asks.

### macOS (Apple Silicon and Intel)

Unzip `paint-*-macos-universal.zip` and drag **Paint.app** into Applications. It needs macOS 11 or later. The app isn't notarized, so the first time, right-click it and choose **Open** (or use **Open Anyway** in System Settings › Privacy & Security). For drawing together, allow access to the local network when asked.

Check any download with `sha256sum -c <file>.sha256` (on macOS: `shasum -a 256 -c <file>.sha256`).
