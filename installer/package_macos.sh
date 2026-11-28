#!/usr/bin/env bash
# Packages a built macOS NSW-Home binary into a self-contained NSW-Home.app
# bundle and a drag-to-Applications disk image (.dmg).
#
#   installer/package_macos.sh [version] [build_dir] [out_dir]
#
# Defaults:
#   version   = 0.1.0
#   build_dir = build/unix
#   out_dir   = build
set -euo pipefail

VERSION="${1:-0.1.0}"
BUILD_DIR="${2:-build/unix}"
OUT_DIR="${3:-build}"
ARCH="$(uname -m)"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$REPO_ROOT/$BUILD_DIR/nsw-home"
OUT_PATH="$REPO_ROOT/$OUT_DIR"
APP_BUNDLE="$OUT_PATH/NSW-Home.app"
DMG_PATH="$OUT_PATH/NSW-Home-macos-${ARCH}.dmg"

if [[ ! -x "$BINARY" ]]; then
    echo "error: built binary not found at $BINARY" >&2
    exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

rm -rf "$APP_BUNDLE" "$DMG_PATH"
mkdir -p \
    "$APP_BUNDLE/Contents/MacOS" \
    "$APP_BUNDLE/Contents/Frameworks" \
    "$APP_BUNDLE/Contents/Resources"

cp "$BINARY" "$APP_BUNDLE/Contents/MacOS/NSW-Home"
chmod 755 "$APP_BUNDLE/Contents/MacOS/NSW-Home"

cp "$REPO_ROOT/LICENSE" "$APP_BUNDLE/Contents/Resources/LICENSE.txt"
cp "$REPO_ROOT/THIRD_PARTY.md" "$APP_BUNDLE/Contents/Resources/THIRD_PARTY.md"

# Build AppIcon.icns from assets/app.ico using built-in macOS tools.
ICON_PNG="$TMP_DIR/app.png"
ICONSET="$TMP_DIR/AppIcon.iconset"
mkdir -p "$ICONSET"
sips -s format png "$REPO_ROOT/assets/app.ico" --out "$ICON_PNG" >/dev/null
for size in 16 32 128 256; do
    sips -z "$size" "$size" "$ICON_PNG" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
    size2x=$((size * 2))
    sips -z "$size2x" "$size2x" "$ICON_PNG" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP_BUNDLE/Contents/Resources/AppIcon.icns"

cat > "$APP_BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>NSW-Home</string>
    <key>CFBundleDisplayName</key>
    <string>NSW-Home</string>
    <key>CFBundleIdentifier</key>
    <string>io.github.daniel-madrid-07.NSW-Home</string>
    <key>CFBundleVersion</key>
    <string>${VERSION}</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleExecutable</key>
    <string>NSW-Home</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
EOF

# Recursively bundle all non-system dylibs into Contents/Frameworks and rewrite
# their load paths so the .app runs without Homebrew installed.
FRAMEWORKS="$APP_BUNDLE/Contents/Frameworks"
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"

resolve_dep() {
    local dep="$1"
    local src_dir="$2"
    case "$dep" in
        /System/Library/*|/usr/lib/*)
            return 1
            ;;
        @executable_path/*)
            return 1
            ;;
        @loader_path/*)
            local rel="${dep#@loader_path/}"
            if [[ -f "$src_dir/$rel" ]]; then
                echo "$src_dir/$rel"
                return 0
            fi
            return 1
            ;;
        @rpath/*)
            local rel="${dep#@rpath/}"
            for dir in "$src_dir" "$BREW_PREFIX/lib" /opt/homebrew/lib /usr/local/lib; do
                if [[ -f "$dir/$rel" ]]; then
                    echo "$dir/$rel"
                    return 0
                fi
            done
            return 1
            ;;
        /*)
            if [[ -f "$dep" ]]; then
                echo "$dep"
                return 0
            fi
            return 1
            ;;
        *)
            return 1
            ;;
    esac
}

# Track source directories for copied dylibs so @loader_path resolves cleanly.
MAP_FILE="$TMP_DIR/dylib_sources.txt"
: > "$MAP_FILE"

queue_dylib() {
    local src="$1"
    local base
    base="$(basename "$src")"
    if [[ ! -f "$FRAMEWORKS/$base" ]]; then
        cp -L "$src" "$FRAMEWORKS/$base"
        chmod 755 "$FRAMEWORKS/$base"
        install_name_tool -id "@loader_path/$base" "$FRAMEWORKS/$base"
        printf '%s\t%s\n' "$base" "$(dirname "$(realpath "$src")")" >> "$MAP_FILE"
    fi
}

rewrite_macho() {
    local target="$1"
    local prefix="$2"
    local src_dir="$3"
    local dep resolved dep_base

    while IFS= read -r dep; do
        [[ -z "$dep" ]] && continue
        if resolved="$(resolve_dep "$dep" "$src_dir")"; then
            dep_base="$(basename "$resolved")"
            queue_dylib "$resolved"
            install_name_tool -change "$dep" "${prefix}/${dep_base}" "$target"
        fi
    done < <(otool -L "$target" | tail -n +2 | awk '{print $1}')
}

# Start with the main executable.
rewrite_macho "$APP_BUNDLE/Contents/MacOS/NSW-Home" "@executable_path/../Frameworks" "$(dirname "$BINARY")"

# Homebrew's sdl2 package may be sdl2-compat, which dlopen()s @loader_path/libSDL3.dylib
# at runtime rather than linking it via LC_LOAD_DYLIB.
if [[ -f "$FRAMEWORKS/libSDL2-2.0.0.dylib" ]]; then
    SDL3_CANDIDATE=""
    for p in "$BREW_PREFIX/opt/sdl3/lib/libSDL3.0.dylib" "$BREW_PREFIX/lib/libSDL3.0.dylib"; do
        if [[ -f "$p" ]]; then
            SDL3_CANDIDATE="$p"
            break
        fi
    done
    if [[ -n "$SDL3_CANDIDATE" ]]; then
        queue_dylib "$SDL3_CANDIDATE"
        ln -sf "libSDL3.0.dylib" "$FRAMEWORKS/libSDL3.dylib"
    fi
fi

# Process queued dylibs until closure.
PROCESSED_FILE="$TMP_DIR/processed.txt"
: > "$PROCESSED_FILE"
while true; do
    PROGRESS=0
    while IFS=$'\t' read -r base src_dir; do
        [[ -z "$base" ]] && continue
        if ! grep -Fxq "$base" "$PROCESSED_FILE"; then
            echo "$base" >> "$PROCESSED_FILE"
            rewrite_macho "$FRAMEWORKS/$base" "@loader_path" "$src_dir"
            PROGRESS=1
        fi
    done < "$MAP_FILE"
    [[ "$PROGRESS" -eq 0 ]] && break
done

# Verify no non-system absolute paths remain.
LEAKED=0
for macho in "$APP_BUNDLE/Contents/MacOS/NSW-Home" "$FRAMEWORKS"/*.dylib; do
    [[ -L "$macho" ]] && continue
    while IFS= read -r dep; do
        case "$dep" in
            /System/Library/*|/usr/lib/*|@executable_path/*|@loader_path/*) ;;
            *)
                echo "error: unresolved non-system dependency in $(basename "$macho"): $dep" >&2
                LEAKED=1
                ;;
        esac
    done < <(otool -L "$macho" | tail -n +2 | awk '{print $1}')
done
if [[ "$LEAKED" -ne 0 ]]; then
    exit 1
fi

# Ad-hoc sign the bundle (required on Apple Silicon after install_name_tool).
for dylib in "$FRAMEWORKS"/*.dylib; do
    [[ -L "$dylib" ]] && continue
    codesign --force --sign - "$dylib" 2>/dev/null
done
codesign --force --deep --sign - "$APP_BUNDLE" 2>/dev/null

# Create compressed drag-to-Applications DMG.
DMG_STAGE="$TMP_DIR/dmg"
mkdir -p "$DMG_STAGE"
cp -R "$APP_BUNDLE" "$DMG_STAGE/"
ln -s /Applications "$DMG_STAGE/Applications"

hdiutil create \
    -volname "NSW-Home" \
    -srcfolder "$DMG_STAGE" \
    -ov \
    -format UDZO \
    "$DMG_PATH" >/dev/null

echo "Created $APP_BUNDLE"
echo "Created $DMG_PATH"
