#!/usr/bin/env bash
# Turn a raw "Prince of Persia 2" StuffIt archive into the three asset trees the
# (web or native) build consumes — WITHOUT committing any game data to the repo.
# Used by the GitHub Pages CI to bake the data into the deploy artifact only.
#
#   tools/prep-web-data.sh <Prince_Of_Persia2.sit> <out_root>
#
# Produces:
#   <out_root>/app    -> app resources + CODE   (POP2_APP_DIR  / /data/app)
#   <out_root>/data   -> resource dumps         (POP2_RSRC_DIR / /data/data)
#   <out_root>/vol    -> raw game volume        (POP2_VOL_DIR  / /data/pop2)
#
# Mirrors the manual steps in README.md (unar + dump_resources.py), made
# deterministic and path-explicit. Needs: unar (The Unarchiver), python3.
set -euo pipefail

sit="${1:?usage: prep-web-data.sh <sit> <out_root>}"
out="${2:?usage: prep-web-data.sh <sit> <out_root>}"
root="$(cd "$(dirname "$0")/.." && pwd)"

for bin in unar python3; do
  command -v "$bin" >/dev/null || { echo "error: '$bin' not found in PATH" >&2; exit 1; }
done
[ -f "$sit" ] || { echo "error: no such file: $sit" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "[prep] unstuffing $(basename "$sit")…"
unar -force-overwrite -quiet -k visible -o "$work" "$sit"
src="$(find "$work" -maxdepth 2 -type d -name "Prince of Persia 2" | head -1)"
[ -n "$src" ] || { echo "error: 'Prince of Persia 2' folder not found in archive" >&2; exit 1; }

# Move the unstuffed volume into the output so it survives the temp-dir cleanup;
# this becomes POP2_VOL_DIR (raw .dat sound banks + any shipped saves).
rm -rf "$out"; mkdir -p "$out/data"
mv "$src" "$out/vol"
vol="$out/vol"

echo "[prep] dumping app resources + CODE…"
python3 "$root/tools/dump_resources.py" "$vol/Prince of Persia 2.rsrc" "$out/app" >/dev/null

# The HALESTORM MIDI synth ships encrypted+packed in MDRV id 11; decode it into
# CODE segment 25 (gen/seg25.cpp is the recompiled form, but the runtime also
# loads this raw image for its data reads). Without it MIDI playback is dead.
echo "[prep] decoding MDRV MIDI synth -> CODE segment 25…"
mdrv="$(ls "$out/app/MDRV/"11_*.bin 2>/dev/null | head -1)"
[ -n "$mdrv" ] || { echo "error: MDRV id 11 resource not found in app dump" >&2; exit 1; }
python3 "$root/tools/decode_mdrv.py" "$mdrv" "$out/app/CODE/25_MIDISynth.bin" >/dev/null

echo "[prep] dumping data resource files…"
shopt -s nullglob
for f in "$vol/Data/"*.rsrc; do
  base="$(basename "$f")"      # e.g. Rooftops.rsrc.rsrc, Prince2.opt.rsrc, DigiSnd.dat.rsrc
  mac="${base%.rsrc}"          # strip unar's AppleDouble suffix -> Rooftops.rsrc / DigiSnd.dat
  case "$mac" in *.dat) continue ;; esac   # .dat sound banks are served raw from the volume
  dir="${mac%.*}"              # strip the Mac extension -> Rooftops / Prince2 / Mohawk
  python3 "$root/tools/dump_resources.py" "$f" "$out/data/$dir" >/dev/null
done

echo "[prep] done: $(ls "$out/data" | wc -l) data dirs, app=$(ls "$out/app" | wc -l) types -> $out/{app,data,vol}"
