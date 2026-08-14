project('TQVaultC', 'c',
  version: '0.7.8',
  # 1.1 is what the meson.options file (over meson_options.txt) requires.
  meson_version: '>= 1.1',
  # c_std is pinned rather than left to the compiler's default of the day.
  # gnu17 (not c17) because compat.h and the GLib/GTK headers rely on the
  # __USE_MISC / _DEFAULT_SOURCE surface that every -std=c* level hides.
  default_options: ['warning_level=3', 'c_std=gnu17', 'b_pie=true'])

cc = meson.get_compiler('c')

# 0. Fuzzing build.  libFuzzer is a clang feature -- gcc has no
# -fsanitize=fuzzer -- so the harnesses need their own build dir configured
# with CC=clang, and -Dfuzzing=true builds *those and nothing else*.  Pulling
# the app into a clang build would put warnings from a toolchain we do not
# otherwise use into a build that is meant to be warning-clean.
#
# The source lists are built here rather than in fuzz/meson.build because
# meson forbids a subdir from naming files above itself; file objects carry
# their location, so they pass down fine.  Same for the include dir.
if get_option('fuzzing')
  fuzz_inc = include_directories('src')

  # What the save parsers drag in behind them: config.c for tqvc_debug and the
  # config accessors, io_atomic.c for the write side, vault.c for the shared
  # item representation (vault_item_free_strings), and then vault.c's own
  # dependency on the DBR lookup layer -- asset_lookup.c, arz.c, arc.c and the
  # mmap shim.  None of it is exercised by these harnesses (no game database is
  # loaded); it is here to satisfy the linker.
  fuzz_save_support = files(
    'src/config.c', 'src/io_atomic.c', 'src/vault.c',
    'src/asset_lookup.c', 'src/arz.c', 'src/arc.c', 'src/platform_mmap.c')

  fuzz_parser_sources = {
    'character': files('src/character.c'),
    'stash':     files('src/stash.c'),
    'quest':     files('src/quest_tokens.c'),
    'mesh':      files('src/mesh.c'),
    'anm':       files('src/anm.c'),
    'dds':       files('src/dds_decode.c'),
  }

  subdir('fuzz')
  subdir_done()
endif

# Generated GResource bundles emit single string literals well past the
# C99 4095-char minimum; -Wno-overlength-strings keeps that out of our
# build log without affecting our hand-written code.
add_project_arguments('-Wno-overlength-strings', language: 'c')

# Hardening.  _FORTIFY_SOURCE needs -O1 or higher and warns at -O0, which
# would break the warning-clean rule, so gate the whole set on an optimizing
# build rather than applying it globally.  Every flag is probed first:
# -fstack-clash-protection in particular is unsupported on the mingw target.
if get_option('optimization') not in ['0', 'g']
  hardening_args = []

  foreach arg : ['-fstack-protector-strong', '-fstack-clash-protection']
    if cc.has_argument(arg)
      hardening_args += arg
    endif
  endforeach

  # -U first: distro gcc specs may already define _FORTIFY_SOURCE, and a
  # bare -D on top of that is a redefinition warning.
  if cc.has_multi_arguments(['-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3'])
    hardening_args += ['-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3']
  endif

  add_project_arguments(hardening_args, language: 'c')
endif

# 1. Define Dependencies
# include_type: 'system' converts -I to -isystem so pedantic warnings from
# third-party headers (e.g. gtkaccessiblehypertext.h's stray ';') are
# suppressed while still firing for our own code.
gtk_dep = dependency('gtk4', include_type: 'system')
json_dep = dependency('json-c', include_type: 'system')
zlib_dep = dependency('zlib', include_type: 'system')
m_dep = cc.find_library('m', required : false)

# Windows-only: psapi provides GetProcessMemoryInfo (config.c's peak-memory
# reporting for --first-run-build).  Added to every target that links config.c.
win_extra_deps = []
if host_machine.system() == 'windows'
  win_extra_deps += cc.find_library('psapi')
endif

# 2. Version info
version_conf = configuration_data()
version_conf.set_quoted('TQVAULTC_VERSION', meson.project_version())
version_h = custom_target('version_header',
  output: 'version.h',
  command: [find_program('bash'), '-c',
    'cat > "$1" <<EOF\n#ifndef VERSION_H\n#define VERSION_H\n#define TQVAULTC_VERSION "' + meson.project_version() + '"\n#endif\nEOF',
    '--', '@OUTPUT@'],
  build_always_stale: true)

build_number = custom_target('build_number',
  output: 'build_number.h',
  command: [find_program('bash'), meson.project_source_root() / 'build_number.sh', '@OUTPUT@'],
  build_always_stale: true)

gnome = import('gnome')
app_resources = gnome.compile_resources('tqvaultc-resources',
  'assets/tqvaultc.gresource.xml',
  source_dir: 'assets')

# 3. Platform abstraction
platform_sources = ['src/platform_mmap.c']

# 4. Source Files
app_sources = [
  'src/main.c',
  'src/ui.c',
  'src/ui_build.c',
  'src/character.c',
  'src/arz.c',
  'src/vault.c',
  'src/config.c',
  'src/io_atomic.c',
  'src/parse_num.c',
  'src/arc.c',
  'src/texture.c',
  'src/dds_decode.c',
  'src/asset_lookup.c',
  'src/translation.c',
  'src/item_stats.c',
  'src/item_stats_format.c',
  'src/item_stats_expr.c',
  'src/affix_table.c',
  'src/db_loot.c',
  'src/db_creatures.c',
  'src/db_quests.c',
  'src/ui_manage.c',
  'src/ui_context_menu.c',
  'src/ui_bag_menu.c',
  'src/ui_affix_dialog.c',
  'src/ui_draw.c',
  'src/ui_tooltip.c',
  'src/ui_io.c',
  'src/ui_dnd.c',
  'src/ui_stats.c',
  'src/ui_settings.c',
  'src/prefetch.c',
  'src/stash.c',
  'src/quest_tokens.c',
  'src/ui_checklist_dialog.c',
  'src/ui_stats_dialog.c',
  'src/ui_skills_tree.c',
  'src/ui_skills_data.c',
  'src/ui_skills_draw.c',
  'src/ui_skills_layout.c',
  'src/ui_database_dialog.c',
  'src/ui_db_browser.c',
  'src/ui_db_browser_index.c',
  'src/ui_db_browser_detail.c',
  'src/db_browser_cache.c',
  'src/mesh.c',
  'src/mesh_render.c',
  'src/mesh_skin.c',
  'src/anm.c',
  'src/creature_thumbs.c'
]

# 5. Build the GUI Application
# On Windows, link as a GUI subsystem binary so launching the .exe doesn't
# spawn a console window. stdout/stderr from the GUI build go to the void;
# the CLI tools below stay on the console subsystem so --help etc. work.
tqvaultc_exe = executable('tqvaultc',
  app_sources + platform_sources + [version_h, build_number, app_resources],
  dependencies: [gtk_dep, json_dep, zlib_dep, m_dep] + win_extra_deps,
  # On Windows, link as a GUI subsystem binary so launching the .exe
  # doesn't spawn a console window.
  win_subsystem: 'windows',
  install: true)

# 6. Build the CLI Character Parser
# Keeping it for testing purposes
executable('tq-stats',
  'src/utils/tq_stats.c',
  install: true)

# 7. Build the DBR/ARC Inspection Tool
executable('tq-dbr-tool',
  ['src/utils/tq_dbr_tool.c', 'src/arz.c', 'src/arc.c',
   'src/db_loot.c', 'src/db_creatures.c', 'src/db_quests.c',
   'src/texture.c', 'src/dds_decode.c', 'src/asset_lookup.c', 'src/config.c', 'src/io_atomic.c',
   'src/parse_num.c',
   'src/mesh.c', 'src/mesh_render.c', 'src/mesh_skin.c', 'src/anm.c'] + platform_sources,
  dependencies: [gtk_dep, json_dep, zlib_dep, m_dep] + win_extra_deps,
  install: true)

# 8. Build the Player.chr Debugging Tool
executable('tq-chr-tool',
  ['src/utils/tq_chr_tool.c', 'src/character.c', 'src/io_atomic.c'],
  dependencies: [gtk_dep, m_dep],
  install: true)

# 9. Build the Quest Token Inspection Tool
executable('tq-quest-tool',
  ['src/utils/tq_quest_tool.c', 'src/quest_tokens.c', 'src/io_atomic.c'],
  dependencies: [gtk_dep],
  install: true)

# 10. Build the disk-full test harness (POSIX only: no Windows rlimits)
if host_machine.system() != 'windows'
  executable('tq-limitrun',
    'src/utils/tq_limitrun.c',
    install: true)
endif

# 11. Build the Texture Extractor
executable('extract-textures',
  ['src/extract_textures.c', 'src/arc.c', 'src/texture.c', 'src/dds_decode.c', 'src/asset_lookup.c', 'src/arz.c', 'src/config.c', 'src/io_atomic.c'] + platform_sources,
  dependencies: [gtk_dep, json_dep, zlib_dep] + win_extra_deps,
  install: true)

# 12. Tests
# The self-tests are flags on the tqvaultc binary rather than separate
# executables, so `meson test` just invokes it.  The point of wiring them up
# here is the sanitizer build dirs: `meson test -C build-asan` is what actually
# exercises the parsers under ASan/UBSan/TSan.  See BUILD.md.
#
# Only the matcher test is self-contained.  The other four load the real game
# database, which lives in the gitignored testdata/ tree, so they are
# registered only when that tree is present -- absent on a fresh clone and in
# CI, where they would otherwise fail for want of data rather than a defect.
#
# TSAN_OPTIONS is inert outside build-tsan, but meson sets halt_on_error for
# ASan/UBSan only -- without it TSan prints a race and still exits 0, so a test
# would pass while reporting one.  No suppression file: src/tq_tsan.h annotates
# the GMutex handoffs TSan cannot see, so the run is clean without blanketing
# our own functions.
fs = import('fs')

test_env = {'TSAN_OPTIONS': 'halt_on_error=1:exitcode=66'}

test('search-query', tqvaultc_exe,
  args: ['--search-query-selftest'],
  env: test_env,
  timeout: 60)

testdata_dir = meson.project_source_root() / 'testdata'

if fs.exists(testdata_dir / 'gamefiles' / 'Database' / 'database.arz')
  testdata_conf = configuration_data()
  testdata_conf.set('GAME_FOLDER', testdata_dir / 'gamefiles')
  testdata_conf.set('SAVE_FOLDER', testdata_dir / 'saves')
  testdata_conf.set('VAULT_FOLDER', testdata_dir / 'vaults')

  testdata_config = configure_file(
    input: 'tests/testdata-config.json.in',
    output: 'testdata-config.json',
    configuration: testdata_conf)

  # Each builds the full DBR index first, so the wall-clock floor is tens of
  # seconds natively and several times that under a sanitizer.
  foreach t : [
    ['db-cache',    ['--db-cache-selftest']],
    ['db-search',   ['--db-search-selftest', 'vitality damage']],
    ['db-sort',     ['--db-sort-selftest']],
    ['stack-merge', ['--stack-merge-selftest']],
    ['prefetch',    ['--prefetch-selftest', testdata_dir / 'saves/_soothie/Player.chr']],
  ]
    test(t[0], tqvaultc_exe,
      args: t[1] + [testdata_config],
      env: test_env,
      timeout: 900)
  endforeach

else
  message('testdata/gamefiles not present -- registering only the self-contained tests')
endif
