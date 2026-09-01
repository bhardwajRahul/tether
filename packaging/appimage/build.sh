#!/bin/bash
# Build a portable AppImage of tether: GTK app, CLI, daemon and pairing dialog
# in one file.
#
#   ./packaging/appimage/build.sh
#
# Build it on the oldest glibc we can

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT/build/appimage}
APPDIR=$BUILD_DIR/AppDir
ARCH=${ARCH:-$(uname -m)}
TOOLS=${TOOLS:-$BUILD_DIR/tools}

# AppImages cannot mount themselves inside a container without FUSE.
export APPIMAGE_EXTRACT_AND_RUN=1

cmake -B "$BUILD_DIR" -S "$ROOT" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD_DIR"

rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR"

VERSION=$("$APPDIR/usr/bin/tether" --version | awk '{print $2}')

# --bt-status reads Secure Connections through btmgmt, and a host without
# bluez-utils has none.
if BTMGMT=$(command -v btmgmt); then
    cp "$BTMGMT" "$APPDIR/usr/bin/"
else
    echo "warning: btmgmt not found, Secure Connections will report as unknown" >&2
fi

mkdir -p "$TOOLS"
fetch() {
    [ -x "$TOOLS/$1" ] && return 0
    curl -fsSL -o "$TOOLS/$1" "$2"
    chmod +x "$TOOLS/$1"
}
fetch linuxdeploy \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage"
fetch linuxdeploy-plugin-gtk.sh \
    "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"

export PATH="$TOOLS:$PATH"
export OUTPUT="tether-${VERSION}-${ARCH}.AppImage"

cd "$BUILD_DIR"
linuxdeploy --appdir "$APPDIR" \
    --plugin gtk \
    --custom-apprun "$ROOT/packaging/appimage/AppRun" \
    --desktop-file "$APPDIR/usr/share/applications/tether-gtk.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/tether.png" \
    --output appimage

echo "==> $BUILD_DIR/$OUTPUT"
