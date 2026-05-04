project('TQVaultC', 'c',
  version: '0.6.1',
  default_options: ['warning_level=3'])

# Generated GResource bundles emit single string literals well past the
# C99 4095-char minimum; -Wno-overlength-strings keeps that out of our
# build log without affecting our hand-written code.
add_project_arguments('-Wno-overlength-strings', language: 'c')

# 1. Define Dependencies
# include_type: 'system' converts -I to -isystem so pedantic warnings from
# third-party headers (e.g. gtkaccessiblehypertext.h's stray ';') are
# suppressed while still firing for our own code.
gtk_dep = dependency('gtk4', include_type: 'system')
json_dep = dependency('json-c', include_type: 'system')
zlib_dep = dependency('zlib', include_type: 'system')
m_dep = meson.get_compiler('c').find_library('m', required : false)

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
  'src/character.c',
  'src/arz.c',
  'src/vault.c',
  'src/config.c',
  'src/arc.c',
  'src/texture.c',
  'src/dds_decode.c',
  'src/asset_lookup.c',
  'src/translation.c',
  'src/item_stats.c',
  'src/item_stats_format.c',
  'src/affix_table.c',
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
  'src/ui_skills_dialog.c',
  'src/ui_database_dialog.c'
]

# 5. Build the GUI Application
# On Windows, link as a GUI subsystem binary so launching the .exe doesn't
# spawn a console window. stdout/stderr from the GUI build go to the void;
# the CLI tools below stay on the console subsystem so --help etc. work.
executable('tqvaultc',
  app_sources + platform_sources + [version_h, build_number, app_resources],
  dependencies: [gtk_dep, json_dep, zlib_dep, m_dep],
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
  ['src/utils/tq_dbr_tool.c', 'src/arz.c', 'src/arc.c'] + platform_sources,
  dependencies: [gtk_dep, zlib_dep, m_dep],
  install: true)

# 8. Build the Player.chr Debugging Tool
executable('tq-chr-tool',
  ['src/utils/tq_chr_tool.c', 'src/character.c'],
  dependencies: [gtk_dep, m_dep],
  install: true)

# 9. Build the Quest Token Inspection Tool
executable('tq-quest-tool',
  ['src/utils/tq_quest_tool.c', 'src/quest_tokens.c'],
  dependencies: [gtk_dep],
  install: true)

# 10. Build the Texture Extractor
executable('extract-textures',
  ['src/extract_textures.c', 'src/arc.c', 'src/texture.c', 'src/dds_decode.c', 'src/asset_lookup.c', 'src/arz.c', 'src/config.c'] + platform_sources,
  dependencies: [gtk_dep, json_dep, zlib_dep],
  install: true)
