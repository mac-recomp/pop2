#!/usr/bin/env bash
# Build the data archive for the web file-picker build (POP2_WASM_NO_PRELOAD).
# A player drops the resulting pop2-data.tar.gz onto the page; the shell unpacks
# it into MEMFS at /data/{app,data,pop2} and starts the game. Nothing is shipped
# with the engine, so the engine stays free of copyrighted game content.
#
#   tools/make-web-data.sh [raw-volume-dir] [output.tar.gz]
#
# The three trees mirror the native asset layout (see CMakeLists.txt):
#   app/  = extracted/app            -> /data/app   (CODE + app resources)
#   data/ = extracted/data           -> /data/data  (resource dumps)
#   pop2/ = the raw game volume      -> /data/pop2   (.dat files, Level N saves)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
app="$root/extracted/app"
data="$root/extracted/data"
vol="${1:-$root/../extracted_sit/Prince of Persia 2}"
out="${2:-pop2-data.tar.gz}"

for d in "$app" "$data" "$vol"; do
  [ -d "$d" ] || { echo "error: missing directory: $d" >&2; exit 1; }
done

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
ln -s "$app"  "$stage/app"
ln -s "$data" "$stage/data"
ln -s "$vol"  "$stage/pop2"

# ustar so any name >100 bytes uses the prefix field (the shell's tar reader
# handles ustar, not GNU LongLink); -h dereferences the staging symlinks.
tar --format=ustar -czhf "$out" -C "$stage" app data pop2
echo "wrote $out ($(du -h "$out" | cut -f1))"
