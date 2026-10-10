A from-scratch Rust reimplementation of classic Microsoft Paint (the Windows 7/10 ribbon edition). See the [README](https://github.com/stephenkingston/paint#readme) for the full feature tour.

### Linux (x86_64)

```sh
tar xzf paint-*-linux-x86_64.tar.gz
cd paint-*-linux-x86_64
./paint                 # run it from here, or
./install.sh            # install for your user: ~/.local/bin/paint plus an app-launcher entry
```

Requirements: an x86_64 distribution with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, Arch and later), X11 or Wayland, Vulkan or OpenGL graphics drivers, fontconfig (for the Text tool's fonts) and an XDG desktop portal (for the Open/Save dialogs; standard on desktop setups).

Check the download with `sha256sum -c paint-*-linux-x86_64.tar.gz.sha256`.
