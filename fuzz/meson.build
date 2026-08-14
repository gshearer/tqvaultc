# libFuzzer harnesses for the parsers that consume input from outside the
# process.  Reached only from the root meson.build under -Dfuzzing=true; see
# BUILD.md for the configure-and-run recipe.

if not cc.has_argument('-fsanitize=fuzzer')
  error('-Dfuzzing=true needs clang -- gcc has no libFuzzer. Reconfigure with CC=clang.')
endif

# ASan and UBSan ride along with the fuzzer here: a fuzzer without a detector
# only finds the crashes that happen to segfault.  TSan is absent by design --
# it is incompatible with ASan, and none of these parsers are threaded.
fuzz_args = ['-fsanitize=fuzzer,address,undefined',
             '-fno-omit-frame-pointer',
             '-fno-sanitize-recover=undefined']

glib_dep = dependency('glib-2.0', include_type: 'system')
json_dep = dependency('json-c', include_type: 'system')
zlib_dep = dependency('zlib', include_type: 'system')
m_dep = cc.find_library('m', required: false)

# The save-file parsers take a path, not a buffer, so each harness stages the
# input through one reused temp file.
fuzz_tmpfile = files('fuzz_tmpfile.c')

foreach t : [
  ['character', 'fuzz_character.c'],
  ['stash',     'fuzz_stash.c'],
  ['quest',     'fuzz_quest.c'],
]
  executable('fuzz-' + t[0],
    [files(t[1]), fuzz_tmpfile, fuzz_parser_sources[t[0]]] + fuzz_save_support,
    include_directories: fuzz_inc,
    c_args: fuzz_args,
    link_args: fuzz_args,
    dependencies: [glib_dep, json_dep, zlib_dep, m_dep])
endforeach

# These three already take (const uint8_t *, size_t), so they need no staging
# file and no support sources.
foreach t : [
  ['mesh', 'fuzz_mesh.c'],
  ['anm',  'fuzz_anm.c'],
  ['dds',  'fuzz_dds.c'],
]
  executable('fuzz-' + t[0],
    [files(t[1]), fuzz_parser_sources[t[0]]],
    include_directories: fuzz_inc,
    c_args: fuzz_args,
    link_args: fuzz_args,
    dependencies: [m_dep])
endforeach
