# pop2

A static recompilation of the Macintosh build of *Prince of Persia 2: The
Shadow and the Flame* (1995, 68k) into native code for modern platforms.
Modeled on XenonRecomp / N64Recomp: 68k → C++ plus a Mac Toolbox runtime
layer on top of SDL2.

**This repository contains no game data.** You bring your own legally owned
copy (the StuffIt archive or the original CD); the tools unpack and
recompile it locally.

## Layout

- `pop2-analyzer/` — binary analysis: CODE 0 / jump-table parser, 68k
  decoder (`m68k.py`), recursive flow walk (`flow_analyze.py`), A-line trap
  census. Recon notes: `NOTES.md`.
- `pop2-recomp/` — the 68k → C++ recompiler.
- `pop2-runtime/` — runtime: memory, CPU context, trap dispatcher, Toolbox
  shims (QuickDraw / Sound / Events / Resources) on SDL2.
- `gen/` — recompiler output (one C++ file per CODE segment).
- `tools/` — resource fork dumper (AppleDouble aware), level / MDRV / font
  decoders.
- `docs/DEV-LOG.md` — chronological development log.

## Quick start

```sh
# 1. Unpack the game (needs unar)
unar -k visible -o extracted_sit Prince_Of_Persia2.SIT

# 2. Dump the application resources
python3 tools/dump_resources.py \
    "extracted_sit/Prince of Persia 2/Prince of Persia 2.rsrc" extracted/app

# 3. Analyze the code
python3 pop2-analyzer/flow_analyze.py extracted/app/CODE --json extracted/flow.json

# 4. Build and run
cmake -S . -B build -G Ninja && ninja -C build
./build/pop2 extracted/app/CODE
```

## Status

- [x] SIT / AppleDouble unpacking, resource dump
- [x] CODE 0 parser (A5 world, 1018-entry jump table)
- [x] 68k decoder (lengths / flow): 100k instructions, 0 errors
- [x] Trap census: 272 unique, Toolbox API map
- [x] 68k → C++ recompiler (gen/seg*.cpp)
- [x] Runtime (memory, traps, Resource Manager, files, saves)
- [x] QuickDraw → SDL2 (palettes, GWorlds, native 512x384 — fills the
      window, no border)
- [x] Sound Manager → SDL2 (HALESTORM MDRV pipeline)
- [x] Boots and is **playable from level 1 through 14**
- [x] All 14 levels verified visually (load screenshots:
      `docs_all14_levels_*.png`) — palettes clean, artwork correct
- [x] Final boss (the impostor doppelganger) is defeatable; the duel, the
      room-to-room chase and the Game-Over scenes all work
- [x] Scaling / filters (POP2_SCALE, POP2_FILTER, F8/F9/F11) and gamepad
      (d-pad / stick + A = Shift)
- [x] Level 8 father's-sword scene plays end to end, palette clean
- [x] Tunnel-crawl room crossing (r18→r17, level 6) verified
- [x] "Horse mount, level 10" clarified: the frm=77 "horses" are fencing
      enemies (the prince auto-en-gardes and lunges), the r22 horse is decor;
      there is no mount on level 10 — the level exit is the r19 doors
- [ ] Ending cutscene after victory (does not fire every run) — the only
      real remaining item; needs a frame-by-frame video reference for the
      exact choreography

## Runtime dev tools (env)

`POP2_NO_AUDIO`, `POP2_OPEN_SAVE=name`, `POP2_TELEPORT=ms:room:col:row[:dir]`
(with verify retries), `POP2_AUTOKEY=ms:vk:ch[:cmd|:shift][:hN|:dn|:up]`
(plus `R<room>.<col>.<row>[+ms]` waypoint conditions and relative `+ms`),
`POP2_POKE_STATE`, `POP2_POKE_A5`, `POP2_DUMP_FB=path:eN` (PPM screenshots),
`POP2_AUTOBATTLE=room` (finale combat autopilot), `POP2_WATCH`,
`POP2_DUMP_KID`, `POP2_DUMP_STATE`, `POP2_DUMP_TILE`.
