#!/usr/bin/env bash
# Package a Windows distribution: collect tqvaultc.exe, all transitive DLL
# dependencies, and the GTK4 runtime data files (icon theme + compiled
# GLib schemas) into dist-win/.
#
# Usage:
#   meson setup build-win --cross-file cross/mingw-w64-x86_64.ini
#   meson compile -C build-win
#   ./scripts/package-windows.sh           # → dist-win/
#   ./scripts/package-windows.sh path/out  # custom output dir

set -euo pipefail

# Auto-detect the mingw-w64 environment we're running in:
#   - MSYS2 mingw64 (CI / native Windows builds): /mingw64/, native objdump
#   - Arch Linux cross-compile:                   /usr/x86_64-w64-mingw32/,
#                                                 x86_64-w64-mingw32-objdump
# Override either with MINGW_ROOT / OBJDUMP env vars if needed.
if [ "${MSYSTEM:-}" = "MINGW64" ] || [ "${MSYSTEM:-}" = "UCRT64" ]; then
  MINGW_ROOT="${MINGW_ROOT:-/mingw64}"
  OBJDUMP="${OBJDUMP:-objdump}"
  SRC_BIN="${SRC_BIN:-build-win/tqvaultc.exe}"
else
  MINGW_ROOT="${MINGW_ROOT:-/usr/x86_64-w64-mingw32}"
  OBJDUMP="${OBJDUMP:-x86_64-w64-mingw32-objdump}"
  SRC_BIN="${SRC_BIN:-build-win/tqvaultc.exe}"
fi

MINGW_BIN="$MINGW_ROOT/bin"
OUT_DIR=${1:-dist-win}

if [ ! -x "$SRC_BIN" ]; then
  echo "error: $SRC_BIN missing — run 'meson compile -C build-win' first" >&2
  exit 1
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/bin" "$OUT_DIR/share/glib-2.0/schemas" "$OUT_DIR/share/icons"

cp "$SRC_BIN" "$OUT_DIR/bin/"
# Optional CLI tools
for tool in tq-stats tq-dbr-tool tq-chr-tool tq-quest-tool extract-textures; do
  [ -x "build-win/$tool.exe" ] && cp "build-win/$tool.exe" "$OUT_DIR/bin/"
done

# Walk the import table recursively, copying any DLL we can find under
# the mingw bin dir. System DLLs (KERNEL32, api-ms-win-crt-*) are skipped
# because Windows ships them.
declare -A seen
queue=("$OUT_DIR/bin/tqvaultc.exe")

while [ ${#queue[@]} -gt 0 ]; do
  f="${queue[0]}"; queue=("${queue[@]:1}")
  for dll in $("$OBJDUMP" -p "$f" 2>/dev/null | awk '/DLL Name:/{print $3}'); do
    [ -n "${seen[$dll]:-}" ] && continue
    if [ -f "$MINGW_BIN/$dll" ]; then
      seen[$dll]=1
      cp "$MINGW_BIN/$dll" "$OUT_DIR/bin/"
      queue+=("$MINGW_BIN/$dll")
    fi
  done
done

# GTK4 needs: compiled GLib schemas + an icon theme.
cp "$MINGW_ROOT/share/glib-2.0/schemas/gschemas.compiled" \
   "$OUT_DIR/share/glib-2.0/schemas/"
cp -r "$MINGW_ROOT/share/icons/Adwaita" "$OUT_DIR/share/icons/"
cp -r "$MINGW_ROOT/share/icons/hicolor" "$OUT_DIR/share/icons/" 2>/dev/null || true

dll_count=$(ls "$OUT_DIR/bin/"*.dll | wc -l)
total_size=$(du -sh "$OUT_DIR" | cut -f1)
echo "Packaged $OUT_DIR ($dll_count DLLs, $total_size total)"
echo "Launch with: wine $OUT_DIR/bin/tqvaultc.exe   (or copy to a Windows host)"
