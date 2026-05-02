# Building TQVaultC

## Dependencies

| Package | Used by |
|---|---|
| `gtk4` | GUI |
| `json-c` | vault file format |
| `zlib` | `.arc`/`.arz` decompression |
| `libm` | misc math |

### Arch Linux (native)

```
sudo pacman -S meson ninja gcc pkgconf gtk4 json-c zlib
```

### Arch Linux (mingw-w64 cross toolchain)

```
sudo pacman -S mingw-w64-gcc mingw-w64-binutils mingw-w64-crt mingw-w64-headers \
               mingw-w64-pkg-config mingw-w64-winpthreads \
               mingw-w64-gtk4 mingw-w64-json-c mingw-w64-zlib \
               mingw-w64-adwaita-icon-theme mingw-w64-hicolor-icon-theme \
               wine   # optional, for smoke-testing the .exe
```

(Most of the GTK4 runtime stack — cairo, pango, glib2, harfbuzz, gdk-pixbuf2,
libepoxy, etc. — is pulled in transitively by `mingw-w64-gtk4`.)

## Native Linux build

```
meson setup build
meson compile -C build
./build/tqvaultc           # GUI (--debug for verbose output)
./build/tq-stats <chr>     # character stat parser
./build/tq-dbr-tool        # DBR/ARC inspection (see --help)
./build/tq-chr-tool        # Player.chr inspection
./build/tq-quest-tool      # quest token inspection
./build/extract-textures   # bulk .tex → .png extractor
```

## Cross-compile to Windows (x86_64) from Linux

```
meson setup build-win --cross-file cross/mingw-w64-x86_64.ini
meson compile -C build-win
```

Outputs `build-win/tqvaultc.exe` plus the same set of CLI tools as `.exe`
files. The cross file at `cross/mingw-w64-x86_64.ini` points at the Arch
`x86_64-w64-mingw32-*` toolchain and uses `wine` as the meson `exe_wrapper`
so test/run-from-build commands work.

To smoke-test under wine:

```
wine build-win/tqvaultc.exe
```

### Cutting a GitHub release (automated)

Releases are built and published by `.github/workflows/release-windows.yml`.

**To ship a new version:**

1. Bump `version: '0.6'` in `meson.build` (so dev builds report the new version too).
2. Commit, push to `main`.
3. Tag and push the tag:
   ```
   git tag -a v0.6 -m "Release 0.6"
   git push origin v0.6
   ```

GitHub Actions will then:
- Spin up an `archlinux:base-devel` container
- Install the same mingw-w64 toolchain you use locally
- Run `./scripts/build-installer.sh` (with `RELEASE_VERSION=0.6` derived from the tag)
- Auto-generate release notes from commits since the previous tag
- Create a public Release with `tqvaultc-0.6-setup.exe` attached

End users see a "Latest release" link on the repo page and can download the
installer directly. No Windows VM, no manual steps.

The same workflow also runs on every push to `main` and uploads the
installer as a build artifact (without creating a Release) — so you catch
cross-compile breakage before tagging.

### One-command installer build

For end users, the right deliverable is a single `.exe` installer they can
double-click:

```
sudo pacman -S nsis           # one-time
./scripts/build-installer.sh
```

Produces `tqvaultc-<version>-setup.exe` (~12 MB, LZMA-compressed). The
script cross-compiles, stages the runtime tree, then wraps it with NSIS.
The installer presents a Welcome → Install Dir → Progress → Finish
wizard, drops Start Menu and Desktop shortcuts, and registers an uninstaller
in Add/Remove Programs.

NSIS sources: `installer/tqvaultc.nsi` (icon at `installer/tqvaultc.ico`).

### Packaging a runnable distribution (without an installer)

The `.exe` on its own won't launch on Windows — it needs ~40 GTK4-stack
DLLs (libwinpthread-1.dll, libgtk-4-1.dll, libcairo-2.dll, …), the compiled
GLib schemas, and an icon theme. Use the bundled script:

```
./scripts/package-windows.sh           # writes dist-win/
./scripts/package-windows.sh out/win   # custom output dir
```

The script walks the import table of `tqvaultc.exe` recursively, copying
every transitive DLL it finds in `/usr/x86_64-w64-mingw32/bin/`, then adds
`share/glib-2.0/schemas/gschemas.compiled` and the Adwaita/hicolor icon
themes. System DLLs (`KERNEL32.dll`, `api-ms-win-crt-*`) are skipped —
Windows ships those.

Output layout:

```
dist-win/
  bin/
    tqvaultc.exe
    tq-*.exe              # CLI tools (if built)
    *.dll                 # ~40 GTK4 runtime DLLs
  share/
    glib-2.0/schemas/gschemas.compiled
    icons/Adwaita/
    icons/hicolor/
```

Copy the entire `dist-win/` tree to the Windows host and run
`bin\tqvaultc.exe`. Total size ~40 MB.

## Portability shims

`src/ui.h` provides static-inline fallbacks for GNU extensions that are
missing on mingw:

- `strcasestr` — always uses the in-tree fallback (deterministic across
  platforms; only used outside hot loops)
- `strndup` — only on `_WIN32`

If you add a `.c` file that uses either symbol, include `ui.h` (or move the
shims to a dedicated `compat.h` first).
