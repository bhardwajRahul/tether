#!/bin/bash

set -e
echo "Building Tether Extensions..."

# Allow overriding build directory from environment
BUILD_DIR="${BUILD_DIR:-build/extension}"
BROWSER_DIR="$BUILD_DIR/browser"
MAIL_DIR="$BUILD_DIR/mail"
CHROME_DIR="$BUILD_DIR/chromium"

# ubuntu, tries to add a second esbuild and then rejects the lockfile for not containing it.
[ -d extension/node_modules ] || npm --prefix extension ci --legacy-peer-deps

ESBUILD=extension/node_modules/.bin/esbuild

mkdir -p "$BROWSER_DIR/src/background" "$BROWSER_DIR/src/content"
mkdir -p "$MAIL_DIR/src/mail"
mkdir -p "$CHROME_DIR/src/background" "$CHROME_DIR/src/content"

# Bundle Firefox Browser Extension
echo "Bundling Firefox browser extension..."
"$ESBUILD" extension/src/background/background.js --bundle --outfile="$BROWSER_DIR/src/background/background.js"
"$ESBUILD" extension/src/content/autofill.js --bundle --outfile="$BROWSER_DIR/src/content/autofill.js"
cp extension/manifest-browser.json "$BROWSER_DIR/manifest.json"
if [ -d "extension/icons" ]; then cp -R extension/icons "$BROWSER_DIR/"; fi
(cd "$BROWSER_DIR" && zip -qr ../tether-browser-extension.zip .)

# Bundle Mail Extension
echo "Bundling mail extension..."
"$ESBUILD" extension/src/mail/extractor.js --bundle --outfile="$MAIL_DIR/src/mail/extractor.js"
cp extension/manifest-mail.json "$MAIL_DIR/manifest.json"
if [ -d "extension/icons" ]; then cp -R extension/icons "$MAIL_DIR/"; fi
(cd "$MAIL_DIR" && zip -qr ../tether-mail-extension.xpi .)

# Bundle Chromium Extension (Web Store upload)
echo "Bundling Chromium extension..."
"$ESBUILD" extension/src/background/background.js --bundle --outfile="$CHROME_DIR/src/background/background.js"
"$ESBUILD" extension/src/content/autofill.js --bundle --outfile="$CHROME_DIR/src/content/autofill.js"
cp extension/manifest-browser.json "$CHROME_DIR/manifest.json"
if [ -d "extension/icons" ]; then cp -R extension/icons "$CHROME_DIR/"; fi
(cd "$CHROME_DIR" && zip -qr ../tether-chromium-extension.zip .)

echo "Extensions successfully built and packaged in $BUILD_DIR"
echo "  Firefox browser: $BUILD_DIR/tether-browser-extension.zip"
echo "  Thunderbird:     $BUILD_DIR/tether-mail-extension.xpi"
echo "  Chromium (Store):  $BUILD_DIR/tether-chromium-extension.zip"
