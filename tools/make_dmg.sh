#!/bin/zsh
# Builds the downloadable app and packages it as release/LaLuneDoree-macOS.dmg.
#
# The app in the DMG is different from a normal `cmake --build build`:
#   - SDL2 is built from source and linked in, so it runs without Homebrew.
#   - It's universal (Apple Silicon + Intel) and runs on macOS 11 or later.
#   - Games live in ~/Library/Application Support/LaLuneDoree/Games, because
#     the project's games/ folder doesn't exist on other people's Macs.
#
# The build happens in a temporary folder (compiler flags can't handle the
# spaces a project path may contain), from a copy of the sources. SDL is kept
# there between runs. Your normal build/ app is untouched.
# Usage: tools/make_dmg.sh
set -euo pipefail

ROOT="${0:A:h:h}"
WORK="${TMPDIR:-/tmp}"
WORK="${WORK%/}/lalunedoree-dmg"
OUT_DIR="$ROOT/release"
DMG_NAME="LaLuneDoree-macOS.dmg"   # keep this name stable: the README's download link uses it
VOLUME_NAME="LaLune Dorée Emulator"
MIN_MACOS="11.0"
ARCHS="arm64;x86_64"

SDL_VERSION="2.32.10"
SDL_SHA256="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"

if [[ "$WORK" == *" "* ]]; then
  echo "error: the temporary folder path has a space in it ($WORK); set TMPDIR to one without" >&2
  exit 1
fi
mkdir -p "$WORK" "$OUT_DIR"

# Compilers embed source file paths (for assert messages). Rewrite them so the
# published app doesn't contain this Mac's folder names.
PATH_FLAGS="-ffile-prefix-map=$WORK=."

# 1. SDL2 as a static library (downloaded and built once, then reused).
SDL_PREFIX="$WORK/sdl-$SDL_VERSION"
if [[ ! -f "$SDL_PREFIX/lib/libSDL2.a" ]]; then
  echo "==> Downloading SDL $SDL_VERSION"
  TARBALL="$WORK/SDL2-$SDL_VERSION.tar.gz"
  curl -fsSL --retry 3 -o "$TARBALL" "$SDL_URL"
  echo "$SDL_SHA256  $TARBALL" | shasum -a 256 -c - > /dev/null
  rm -rf "$WORK/SDL2-$SDL_VERSION" "$WORK/sdl-build"
  tar -xzf "$TARBALL" -C "$WORK"
  echo "==> Building SDL $SDL_VERSION (takes a minute or two)"
  {
    cmake -S "$WORK/SDL2-$SDL_VERSION" -B "$WORK/sdl-build" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="$ARCHS" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS" \
      -DCMAKE_C_FLAGS="$PATH_FLAGS" -DCMAKE_OBJC_FLAGS="$PATH_FLAGS" \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX"
    cmake --build "$WORK/sdl-build" -j8
    cmake --install "$WORK/sdl-build"
  } > "$WORK/sdl-build.log" 2>&1 || { echo "error: SDL build failed, see $WORK/sdl-build.log" >&2; exit 1; }
fi

# 2. The app, from a fresh copy of the sources.
echo "==> Building LaLune Dorée Emulator"
SOURCE="$WORK/source"
rm -rf "$SOURCE" "$WORK/app-build"
mkdir -p "$SOURCE"
cp -R "$ROOT/CMakeLists.txt" "$ROOT/src" "$ROOT/assets" "$SOURCE/"
{
  cmake -S "$SOURCE" -B "$WORK/app-build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCHS" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS" \
    -DCMAKE_CXX_FLAGS="$PATH_FLAGS" -DCMAKE_OBJCXX_FLAGS="$PATH_FLAGS" \
    -DCMAKE_PREFIX_PATH="$SDL_PREFIX" -DGAMES_DIR=""
  cmake --build "$WORK/app-build" -j8
} > "$WORK/app-build.log" 2>&1 || { echo "error: app build failed, see $WORK/app-build.log" >&2; exit 1; }
APP="$WORK/app-build/LaLuneDoree.app"
BIN="$APP/Contents/MacOS/LaLuneDoree"

# 3. Checks: it only depends on things every Mac has, and contains no paths
#    from this Mac.
if otool -L "$BIN" | grep -E "^\s" | grep -vE "^\s+(/System/Library/|/usr/lib/)"; then
  echo "error: the app links a library that isn't part of macOS (listed above)" >&2
  exit 1
fi
if strings "$BIN" | grep -qE "$HOME|$WORK|$ROOT"; then
  echo "error: the app contains a path from this Mac" >&2
  exit 1
fi

# 4. Sign. This is an ad-hoc signature (no Apple Developer ID), so people who
#    download the app have to allow it once in System Settings (see README).
#    - "--options runtime" turns on the hardened runtime: macOS then refuses to
#      load injected or unsigned libraries into the app or let other programs
#      attach a debugger to it.
#    - The entitlements turn on the App Sandbox (assets/LaLuneDoree.entitlements
#      lists the few things the app may still do).
#    - container-migration.plist moves data from earlier versions into the
#      sandbox on first launch.
cp "$ROOT/assets/container-migration.plist" "$APP/Contents/Resources/"
codesign --force --options runtime --entitlements "$ROOT/assets/LaLuneDoree.entitlements" --sign - "$APP" 2> /dev/null
codesign --verify --strict "$APP"
# (Captured first: with pipefail, `codesign | grep -q` can fail when grep
# stops reading early.)
SIGNATURE=$(codesign -dv "$APP" 2>&1)
ENTITLEMENTS=$(codesign -d --entitlements - "$APP" 2> /dev/null)
if [[ "$SIGNATURE" != *flags=*runtime* ]]; then
  echo "error: the app was signed without the hardened runtime" >&2
  exit 1
fi
if [[ "$ENTITLEMENTS" != *com.apple.security.app-sandbox* ]]; then
  echo "error: the app was signed without the App Sandbox" >&2
  exit 1
fi

# 5. The disk image: the app, a shortcut to Applications to drag it onto, and
#    a background with an arrow between them (assets/dmg-background.tiff, drawn
#    by tools/make_dmg_background.py).
echo "==> Creating $DMG_NAME"
APP_IN_DMG="LaLune Dorée Emulator.app"
STAGE="$WORK/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/.background"
cp -R "$APP" "$STAGE/$APP_IN_DMG"
ln -s /Applications "$STAGE/Applications"
cp "$ROOT/assets/dmg-background.tiff" "$STAGE/.background/background.tiff"

# Build a writable image first so Finder can save the window layout into it.
RW_DMG="$WORK/rw.dmg"
rm -f "$RW_DMG"
hdiutil create -volname "$VOLUME_NAME" -srcfolder "$STAGE" -fs HFS+ -format UDRW -ov "$RW_DMG" > /dev/null 2>&1
MOUNT=$(hdiutil attach -readwrite -noverify -noautoopen "$RW_DMG" 2> /dev/null | awk -F'\t' '/Apple_HFS/{print $NF}')
DISK_NAME="${MOUNT:t}"  # "LaLune Dorée Emulator", or "LaLune Dorée Emulator 1" if one is already mounted

# Finder keeps each window's layout in a hidden .DS_Store file. This checks
# that it holds our layout: big icons, both icons placed, and the background.
layout_saved() {
  python3 - "$1" <<'PY'
import plistlib, struct, sys
try:
    data = open(sys.argv[1], "rb").read()
    i = data.find(b"icvpblob")
    size = struct.unpack(">I", data[i + 8:i + 12])[0]
    view = plistlib.loads(data[i + 12:i + 12 + size])
    ok = i >= 0 and view.get("iconSize") == 128 and "backgroundImageAlias" in view and data.count(b"Iloc") >= 2
except Exception:
    ok = False
sys.exit(0 if ok else 1)
PY
}

# Window layout (sizes in points; must match tools/make_dmg_background.py).
# Finder's bounds include the ~28-point title bar.
LAYOUT_OK=0
arrange_window() {
  osascript > /dev/null 2>> "$WORK/finder.log" <<EOF
tell application "Finder"
  tell disk "$DISK_NAME"
    open
    delay 1
    set current view of container window to icon view
    set toolbar visible of container window to false
    set statusbar visible of container window to false
    set the bounds of container window to {200, 120, 800, 548}
    set viewOptions to the icon view options of container window
    set arrangement of viewOptions to not arranged
    set icon size of viewOptions to 128
    set text size of viewOptions to 13
    set background picture of viewOptions to file ".background:background.tiff"
    set position of item "$APP_IN_DMG" of container window to {160, 190}
    set position of item "Applications" of container window to {440, 190}
    close
  end tell
end tell
EOF
}
# Finder writes the layout a few seconds after the window closes, and the
# first time it sees a new disk it may overwrite it with defaults. So: try a
# few times, and keep a copy as soon as the file is right.
rm -f "$WORK/finder.log"
for attempt in 1 2 3; do
  arrange_window || continue
  for _ in {1..30}; do
    if layout_saved "$MOUNT/.DS_Store"; then
      cp "$MOUNT/.DS_Store" "$WORK/layout.DS_Store"
      LAYOUT_OK=1
      break 2
    fi
    sleep 0.5
  done
done
hdiutil detach "$MOUNT" > /dev/null 2>&1 || { sleep 2; hdiutil detach -force "$MOUNT" > /dev/null; }

# Mount again, hidden from Finder so it can't touch the layout, and put the
# saved copy back.
MOUNT=$(hdiutil attach -readwrite -nobrowse -noverify -noautoopen "$RW_DMG" 2> /dev/null | awk -F'\t' '/Apple_HFS/{print $NF}')
if [[ $LAYOUT_OK == 1 ]]; then
  cp "$WORK/layout.DS_Store" "$MOUNT/.DS_Store"
  rm -f "$WORK/layout.DS_Store"
else
  echo "warning: couldn't get Finder to save the window layout (if macOS asked, allow" >&2
  echo "  control of Finder in System Settings > Privacy & Security > Automation, then run" >&2
  echo "  this again). The DMG still works, just without the background and arrow." >&2
  [[ -s "$WORK/finder.log" ]] && sed 's/^/  Finder: /' "$WORK/finder.log" >&2
fi
chmod -Rf go-w "$MOUNT"
rm -rf "$MOUNT/.fseventsd" "$MOUNT/.Trashes"
sync
hdiutil detach "$MOUNT" > /dev/null 2>&1 || { sleep 2; hdiutil detach -force "$MOUNT" > /dev/null; }

rm -f "$OUT_DIR/$DMG_NAME"
hdiutil convert "$RW_DMG" -format UDZO -imagekey zlib-level=9 -o "$OUT_DIR/$DMG_NAME" > /dev/null 2>&1
rm -f "$RW_DMG"
hdiutil verify "$OUT_DIR/$DMG_NAME" > /dev/null 2>&1

echo
echo "Created release/$DMG_NAME ($(du -h "$OUT_DIR/$DMG_NAME" | cut -f1 | tr -d ' '))"
echo "  Architectures: $(lipo -archs "$BIN")"
echo "  Minimum macOS: $(vtool -show-build "$BIN" | awk '/minos/{print $2; exit}')"
echo "  SHA-256:       $(shasum -a 256 "$OUT_DIR/$DMG_NAME" | cut -d' ' -f1)"
