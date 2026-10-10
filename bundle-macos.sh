#!/bin/bash
# Build a release binary and wrap it in target/release/Paint.app so it can be
# launched from Finder, the Dock, or `open target/release/Paint.app`.
# PAINT_BIN=path uses an already-built binary instead (the release workflow
# passes a universal one).
set -euo pipefail
cd "$(dirname "$0")"

if [ -z "${PAINT_BIN:-}" ]; then
    cargo build --release
    PAINT_BIN=target/release/paint
fi
VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)

APP=target/release/Paint.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp "$PAINT_BIN" "$APP/Contents/MacOS/paint"

# App icon (.icns) for Finder and the Dock.
ICONSET="$(mktemp -d)/AppIcon.iconset"
mkdir -p "$ICONSET" "$APP/Contents/Resources"
"$APP/Contents/MacOS/paint" --export-icon "$ICONSET/icon_512x512@2x.png"
for s in 16 32 128 256 512; do
    sips -z $s $s "$ICONSET/icon_512x512@2x.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    if [ $s -lt 512 ]; then
        sips -z $((s * 2)) $((s * 2)) "$ICONSET/icon_512x512@2x.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
    fi
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
rm -rf "$(dirname "$ICONSET")"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Paint</string>
    <key>CFBundleDisplayName</key><string>Paint</string>
    <key>CFBundleIdentifier</key><string>io.github.stephenkingston.paint-together</string>
    <key>CFBundleExecutable</key><string>paint</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <!-- Drawing together finds and joins friends' drawings on the local network. -->
    <key>NSLocalNetworkUsageDescription</key><string>Paint finds and joins drawings by friends on the same Wi-Fi.</string>
    <key>NSBonjourServices</key><array><string>_paintparty._tcp</string></array>
</dict>
</plist>
PLIST
echo "Built $APP"
