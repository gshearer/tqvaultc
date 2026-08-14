#!/usr/bin/env bash
# Populate per-target libFuzzer seed corpora from testdata/.
#
# The seeds are game files: not redistributable, so testdata/ is gitignored and
# the corpora are generated here rather than committed.  Only minimised
# crashers go in the repo, under fuzz/regressions/.
#
# Usage: fuzz/seed-corpus.sh [outdir]     (default: build-fuzz/corpus)

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$root/build-fuzz/corpus}"
data="$root/testdata"

if [ ! -d "$data" ]; then
  echo "seed-corpus: $data not present -- nothing to seed from" >&2
  exit 1
fi

# Copy every match of a find pattern into a target's corpus, flattening the
# paths so same-named files from different characters do not collide.
seed() {
  local target="$1"; shift
  local dir="$out/$target"
  local n=0

  mkdir -p "$dir"

  while IFS= read -r -d '' f; do
    cp -- "$f" "$dir/$(printf '%s' "${f#"$data"/}" | tr '/' '_')"
    n=$((n + 1))
  done < <(find "$data" -type f "$@" -print0 2>/dev/null)

  echo "  $target: $n seed(s)"
}

echo "seeding corpora under $out"
seed character -name 'Player.chr'
seed stash     \( -name '*.dxb' -o -name '*.dxg' \)
seed quest     \( -name '*.myw' -o -name '*.que' \)
seed mesh      -name '*.msh'
seed anm       -name '*.anm'
seed dds       -name '*.dds'

# The mesh/anm/dds assets live inside the .arc archives rather than loose on
# disk, so those three corpora are usually empty here.  libFuzzer copes --
# it starts from a single zero byte -- but extracting a handful of real assets
# with `tq-dbr-tool` first gets it past the header checks far sooner.
for t in mesh anm dds; do
  if [ -z "$(ls -A "$out/$t" 2>/dev/null)" ]; then
    echo "  note: $t corpus is empty; extract assets with tq-dbr-tool for a real starting point" >&2
  fi
done
