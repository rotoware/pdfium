#!/bin/sh
# Ghostscript's source, for reading its separation pipeline (SEPARATIONS.md
# cites it). Downloaded once to knowledge/.cache/gs; the version of the gs
# the suites compare against.
set -e
v=$(gs --version)
dir=$(dirname "$0")/../.cache/gs
src=$dir/ghostscript-$v
[ -d "$src" ] && { echo "$src"; exit 0; }
mkdir -p "$dir"
curl -fsSL -o "$dir/gs.tar.xz" \
  "https://github.com/ArtifexSoftware/ghostpdl-downloads/releases/download/gs$(echo "$v" | tr -d .)/ghostscript-$v.tar.xz"
tar xf "$dir/gs.tar.xz" -C "$dir" && rm "$dir/gs.tar.xz"
echo "$src"
