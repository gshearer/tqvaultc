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

### Build configuration

`meson.build` pins `c_std=gnu17` (not `c17` — every `-std=c*` level hides the
`__USE_MISC` surface the GTK/GLib headers and `compat.h` rely on) and
`b_pie=true`.

Hardening — `-D_FORTIFY_SOURCE=3`, `-fstack-protector-strong`,
`-fstack-clash-protection` — is applied **only when `optimization` is not `0`
or `g`**, because `_FORTIFY_SOURCE` warns at `-O0` and that would break the
warning-clean rule. Every flag is probed with `cc.has_argument` first, so the
mingw target silently drops the ones it does not support. The default
`meson setup build` is a debug build and therefore has none of them; the
shipped installer is built `--buildtype=release` by
`scripts/build-installer.sh` and does.

> Existing build directories keep the options they were configured with —
> `meson setup --reconfigure` does **not** retroactively apply new
> `default_options`. Either `meson setup --wipe <dir>` or set them explicitly:
> `meson configure build -Dc_std=gnu17 -Db_pie=true`.

## Tests

Five self-tests plus a threading driver are wired as `test()` targets:

```
meson test -C build              # all of them
meson test -C build search-query # just one
```

| test | needs `testdata/`? | what it covers |
|---|---|---|
| `search-query` | no | the shared `SearchQuery` matcher: all four modes, phrase-vs-token-AND, `\|` alternatives, broken-regex fallback |
| `prefetch` | yes | the prefetch worker racing `asset_get_dbr()` against the main thread, with `asset_dbr_cache_clear()` pulling records out from under both |
| `stack-merge` | yes | relic/charm completion, the 100-item potion cap, overflow remainders |
| `db-cache` | yes | DB-browser disk-cache round-trip |
| `db-search` | yes | DB-browser content search |
| `db-sort` | yes | DB-browser per-category sort order |

`testdata/` is gitignored (it holds non-redistributable game files), so on a
fresh clone or in CI only `search-query` registers. The other five appear
automatically once `testdata/gamefiles/Database/database.arz` exists.

## Sanitizer builds

The point of the test targets is to run them under a sanitizer. ASan and TSan
cannot share one binary, so this is three build directories, not one flag set:

```
meson setup build-asan  -Db_sanitize=address,undefined
meson setup build-ubsan -Db_sanitize=undefined
meson setup build-tsan  -Db_sanitize=thread -Db_lundef=false

meson test -C build-asan     # ~4 min with testdata present
meson test -C build-ubsan
meson test -C build-tsan
```

Ship nothing that has not been through all three.

**TSan and GLib — read this before adding a suppression.** GLib implements
`GMutex` directly on futexes rather than on pthread primitives, and TSan only
intercepts the pthread ones. It therefore cannot see the happens-before edge a
`g_mutex_lock`/`unlock` pair establishes, and reports every correctly-locked
handoff as a race — for us, the DBR cache and the intern table, both of which
publish heap objects that way.

The fix is `src/tq_tsan.h`: `tq_mutex_lock`/`tq_mutex_unlock` wrap the GLib
calls and add `__tsan_acquire`/`__tsan_release` so the edge is visible.
Outside a TSan build they expand to the bare `g_mutex_lock`/`unlock` — no
cost, no behaviour change. **Use them for any new GMutex that guards data
crossing a thread boundary.**

There is deliberately **no suppression file**. Suppressing would have meant
blanket-ignoring `arz_record_get_var` and the glib containers, which is
exactly where a real race would surface. If you hit a report you believe is
this same artifact, confirm it the way it was confirmed on 2026-08-13 before
acting: temporarily swap the `GMutex` in question for a `pthread_mutex_t` and
re-run. If the report survives that, it is real.

`TSAN_OPTIONS` carries `halt_on_error=1:exitcode=66` — meson sets
`halt_on_error` for ASan/UBSan only, and without it TSan prints a race and
still exits 0, so a test would pass while reporting one.

## Fuzzing the parsers

Every parser that reads a file produced outside this process gets a libFuzzer
harness under `fuzz/`. libFuzzer is a clang feature — gcc has no
`-fsanitize=fuzzer` — so this is a fourth build directory, configured with
clang, and `-Dfuzzing=true` builds *only* the harnesses. The app itself is
never compiled by clang: it has not been vetted against that toolchain's
warnings, and dragging it in would put unrelated diagnostics into a build that
is supposed to be warning-clean.

```
CC=clang meson setup build-fuzz -Dfuzzing=true
meson compile -C build-fuzz

fuzz/seed-corpus.sh                     # populate build-fuzz/corpus/ from testdata/

cd build-fuzz/fuzz
./fuzz-character ../corpus/character -max_total_time=300 \
    -artifact_prefix=../artifacts/character-
```

Six targets, each running under ASan and UBSan as well as the fuzzer — a
fuzzer without a detector only finds the crashes that happen to segfault:

| target | parser | input |
| --- | --- | --- |
| `fuzz-character` | `character_load` | `Player.chr` |
| `fuzz-stash` | `stash_load` | `.dxb` / `.dxg`, desktop v5 and iOS v6 alike |
| `fuzz-quest` | `quest_tokens_load` | `QuestToken.myw` |
| `fuzz-mesh` | `tq_mesh_parse` | `.msh` |
| `fuzz-anm` | `tq_anm_parse` | `.anm` |
| `fuzz-dds` | `dds_decode` | DDS texture blobs |

The first three take a path rather than a buffer, so they stage each input
through one reused temp file (`fuzz/fuzz_tmpfile.c`) — creating and unlinking
a file per iteration would cost more than the parser under test.

**Corpora.** `fuzz/seed-corpus.sh` builds the seed corpora from `testdata/`.
They are generated rather than committed because they are game files, which
`testdata/` is gitignored for. Meshes, animations and textures live inside the
`.arc` archives with no loose copies on disk, so those three corpora come up
empty; `fuzz/dict/*.dict` supplies their magics and header tokens instead,
which is what gets the fuzzer past the header checks. Pass one with
`-dict=../../fuzz/dict/mesh.dict`.

**Regressions.** Minimised crashers are committed under
`fuzz/regressions/<target>/` and replayed by passing the file directly:

```
./fuzz-mesh ../../fuzz/regressions/mesh/*
```

Eight bugs from the first run are in there — an unbounded `.anm` frame
allocation, two undefined conversions in `.dxb` parsing, and five ownership
leaks across `.msh`, `.chr` and `.dxb`. Add the crasher to that directory with
any parser fix.

**Re-fuzz after every fix.** Half of those eight only appeared on the second
pass: a leak or an abort ends the run, hiding whatever lies behind it. A target
is clean when a fresh run over a fresh corpus finds nothing, not when the last
crasher stops reproducing.

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
