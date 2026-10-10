#!/bin/bash
# Build a release binary and wrap it in target/release/Paint.app so it can be
# launched from Finder, the Dock, or `open target/release/Paint.app`.
set -euo pipefail
cd "$(dirname "$0")"

cargo build --release

APP=target/release/Paint.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp target/release/paint "$APP/Contents/MacOS/paint"

# App icon (.icns) for Finder and the Dock.
ICONSET="$(mktemp -d)/AppIcon.iconset"
mkdir -p "$ICONSET" "$APP/Contents/Resources"
target/release/paint --export-icon "$ICONSET/icon_512x512@2x.png"
for s in 16 32 128 256 512; do
    sips -z $s $s "$ICONSET/icon_512x512@2x.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    if [ $s -lt 512 ]; then
        sips -z $((s * 2)) $((s * 2)) "$ICONSET/icon_512x512@2x.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
    fi
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
rm -rf "$(dirname "$ICONSET")"
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Paint</string>
    <key>CFBundleDisplayName</key><string>Paint</string>
    <key>CFBundleIdentifier</key><string>local.rust.paint</string>
    <key>CFBundleExecutable</key><string>paint</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1.0</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
echo "Built $APP"
