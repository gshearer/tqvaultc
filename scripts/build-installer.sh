#!/usr/bin/env bash
# End-to-end Windows installer build:
#   1. cross-compile to build-win/      (if missing)
#   2. stage runtime tree in dist-win/  (via package-windows.sh)
#   3. wrap with NSIS into tqvaultc-setup.exe
#
# Run from the repo root.

set -euo pipefail

cd "$(dirname "$0")/.."

CROSS_FILE=cross/mingw-w64-x86_64.ini
BUILD_DIR=build-win
DIST_DIR=dist-win
NSI_FILE=installer/tqvaultc.nsi

# Allow CI to override the version from a git tag (e.g. RELEASE_VERSION=0.6).
# Otherwise pull it from meson.build.
VERSION=${RELEASE_VERSION:-$(awk -F"'" '/^project\(/{f=1} f && /version:/ {print $2; exit}' meson.build)}
OUT_FILE="tqvaultc-${VERSION}-setup.exe"

# 1. Cross-compile if needed
if [ ! -d "$BUILD_DIR" ]; then
  meson setup "$BUILD_DIR" --cross-file "$CROSS_FILE"
fi
meson compile -C "$BUILD_DIR"

# 2. Stage dist tree
./scripts/package-windows.sh "$DIST_DIR"

# 3. Build the installer. NSIS resolves DIST_DIR relative to the .nsi file's
#    own directory, so we pass an absolute path.
DIST_ABS=$(readlink -f "$DIST_DIR")
OUT_ABS=$(readlink -f .)/${OUT_FILE}

makensis -V2 \
  -DAPP_VERSION="$VERSION" \
  -DDIST_DIR="$DIST_ABS" \
  -DOUTPUT_FILE="$OUT_ABS" \
  "$NSI_FILE"

size=$(du -h "$OUT_FILE" | cut -f1)
echo
echo "Built $OUT_FILE ($size)"
echo "Ship that single file to Windows users."
