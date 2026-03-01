project('TQVaultC', 'c',
  version: '0.1',
  default_options: ['warning_level=3'])

# 1. Define Dependencies
gtk_dep = dependency('gtk4')
json_dep = dependency('json-c')
zlib_dep = dependency('zlib')
magick_dep = dependency('MagickWand')
m_dep = meson.get_compiler('c').find_library('m', required : false)

# 2. Version info
version_conf = configuration_data()
version_conf.set_quoted('TQVAULTC_VERSION', meson.project_version())
configure_file(input: 'src/version.h.in', output: 'version.h', configuration: version_conf)

build_number = custom_target('build_number',
  output: 'build_number.h',
  command: [find_program('bash'), meson.project_source_root() / 'build_number.sh', '@OUTPUT@'],
  build_always_stale: true)

gnome = import('gnome')
app_resources = gnome.compile_resources('tqvaultc-resources',
  'assets/tqvaultc.gresource.xml',
  source_dir: 'assets')

# 3. Source Files
app_sources = [
  'src/main.c',
  'src/ui.c',
  'src/character.c',
  'src/arz.c',
  'src/vault.c',
  'src/config.c',
  'src/arc.c',
  'src/texture.c',
  'src/asset_lookup.c',
  'src/translation.c',
  'src/item_stats.c',
  'src/affix_table.c',
  'src/ui_manage.c',
  'src/ui_context_menu.c',
  'src/ui_affix_dialog.c',
  'src/ui_draw.c',
  'src/ui_tooltip.c',
  'src/ui_dnd.c',
  'src/ui_stats.c',
  'src/ui_settings.c',
  'src/prefetch.c',
  'src/stash.c'
]

# 4. Build the GUI Application
executable('tqvaultc',
  app_sources + [build_number, app_resources],
  dependencies: [gtk_dep, json_dep, zlib_dep, magick_dep, m_dep],
  install: true)

# 4. Build the CLI Character Parser
# Keeping it for testing purposes
executable('tq-stats',
  'src/utils/tq_stats.c',
  install: true)

# 5. Build the DBR/ARC Inspection Tool
executable('tq-dbr-tool',
  ['src/utils/tq_dbr_tool.c', 'src/arz.c', 'src/arc.c'],
  dependencies: [gtk_dep, zlib_dep, m_dep],
  install: true)

# 6. Build the Player.chr Debugging Tool
executable('tq-chr-tool',
  ['src/utils/tq_chr_tool.c', 'src/character.c'],
  dependencies: [m_dep],
  install: true)

# 7. Build the Texture Extractor
executable('extract-textures',
  ['src/extract_textures.c', 'src/arc.c', 'src/texture.c', 'src/asset_lookup.c', 'src/arz.c', 'src/config.c'],
  dependencies: [gtk_dep, json_dep, zlib_dep, magick_dep],
  install: true)
