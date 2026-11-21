#!/bin/sh
# Paint (C port) - wrap the macOS binary in Paint.app, so it starts from the Finder,
# the Dock or `open Paint.app`, opens pictures from the Finder, and asks for the
# local network (drawing together) under its own name.
#
#   tools/mac_bundle.sh <binary> [<path>/Paint.app] [version]     (make TARGET=mac bundle)
#
# For a universal app, `lipo -create` the arm64 and x86_64 binaries first and pass
# that. The icon comes from `<binary> --export-icon <png> 1024` when the binary
# has that option (as Paint's Rust packaging does); without it there is no .icns
# and the Dock shows the icon the app sets at run time.
set -eu
BIN=$1
APP=${2:-$(dirname "$BIN")/Paint.app}
VERSION=${3:-0.0}
SHORT=$(printf '%s' "$VERSION" | sed 's/^v//; s/-.*//')
case "$SHORT" in
    [0-9]*) ;;
    *) SHORT=0.0 ;;
esac

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/paint"

ICON=
if grep -aq -- '--export-icon' "$BIN"; then     # (otherwise running it would open the window)
    TMP=$(mktemp -d)
    mkdir "$TMP/AppIcon.iconset"
    "$BIN" --export-icon "$TMP/icon.png" 1024
    for s in 16 32 128 256 512; do
        sips -z $s $s "$TMP/icon.png" --out "$TMP/AppIcon.iconset/icon_${s}x${s}.png" >/dev/null
        sips -z $((s * 2)) $((s * 2)) "$TMP/icon.png" --out "$TMP/AppIcon.iconset/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$TMP/AppIcon.iconset" -o "$APP/Contents/Resources/AppIcon.icns"
    rm -rf "$TMP"
    ICON='<key>CFBundleIconFile</key><string>AppIcon</string>'
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Paint</string>
    <key>CFBundleDisplayName</key><string>Paint</string>
    <key>CFBundleIdentifier</key><string>io.github.stephenkingston.paint-together</string>
    <key>CFBundleExecutable</key><string>paint</string>
    $ICON
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$SHORT</string>
    <key>CFBundleVersion</key><string>$SHORT</string>
    <key>LSMinimumSystemVersion</key><string>10.13</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.graphics-design</string>
    <key>NSPrincipalClass</key><string>NSApplication</string>
    <key>NSHighResolutionCapable</key><true/>
    <!-- The window is drawn in software: no need to wake a second graphics card. -->
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
    <!-- Pictures it opens from the Finder (Open With, dropping on the Dock icon). -->
    <key>CFBundleDocumentTypes</key>
    <array>
        <dict>
            <key>CFBundleTypeName</key><string>Picture</string>
            <key>CFBundleTypeRole</key><string>Editor</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key>
            <array>
                <string>public.png</string>
                <string>public.jpeg</string>
                <string>com.microsoft.bmp</string>
                <string>com.compuserve.gif</string>
                <string>public.tiff</string>
            </array>
        </dict>
    </array>
    <!-- Drawing together finds and joins friends' drawings on the local network. -->
    <key>NSLocalNetworkUsageDescription</key><string>Paint finds and joins drawings by friends on the same Wi-Fi.</string>
    <key>NSBonjourServices</key><array><string>_paintparty._tcp</string></array>
</dict>
</plist>
PLIST

# An ad-hoc signature over the whole bundle (Info.plist included), which arm64 needs to run. An ad-hoc
# signature has no certificate to hold, so little room is kept for one (codesign keeps 16 KB by default)
codesign --force --sign - --signature-size 256 "$APP" >/dev/null 2>&1 || codesign --force --sign - "$APP" >/dev/null 2>&1 || true
echo "built $APP ($SHORT)"
