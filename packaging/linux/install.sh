#!/bin/sh
# Install Paint for the current user (no root needed): the binary, its icon and a
# desktop launcher. Run it from the extracted release folder.
set -e
cd "$(dirname "$0")"
BIN="${XDG_BIN_HOME:-$HOME/.local/bin}"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"

install -Dm755 paint "$BIN/paint"
install -Dm644 paint.png "$DATA/icons/hicolor/256x256/apps/paint.png"
mkdir -p "$DATA/applications"
# Launchers don't always have ~/.local/bin on their PATH, so point at the binary directly.
sed "s|^Exec=paint|Exec=$BIN/paint|" paint.desktop > "$DATA/applications/paint.desktop"
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$DATA/applications" || true
fi
echo "Installed Paint to $BIN/paint (and added it to your app launcher)."
