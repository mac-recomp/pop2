# PoP2 Mac binary recon (v1.0, 1995)

## Artifact

- `Prince_Of_Persia2.SIT` → StuffIt 5, containing `Prince of Persia 2` (the
  application, type `APPL`, creator `PRNc`, empty data fork) plus `Data/`
  (25 rsrc files of levels and sprites, 4 .dat: DigiSnd/MIDISnd/NISDIGI/
  NISMIDI, Mohawk.ini, Prince2.opt).
- unar unpacks the forks in **AppleDouble** format (magic 0x00051607); the
  resource fork is entry id 2. `tools/dump_resources.py` strips the wrapper
  itself.
- **Architecture: pure 68k** (24 CODE resources, ~398 KB; no CFM/PEF).
- Version from `vers 1`: 1.0.

## Compiler / runtime

- **Symantec Think C** (resource types `_STD`, `_STI`, `STRS`, `DATA`,
  `DREL`, `ZERO`; CODE 1 loads the `'STRS'` resource and initializes the A5
  world).
- Global initialization: the recompiled CODE 1 does it itself (DATA 0 + DREL
  relocations + ZERO bss) — the packing format need not be reversed; the
  runtime only needs memory + a Resource Manager + basic traps.

## A5 world (from CODE 0)

- aboveA5 = 8176 (32-byte appParams + 8144-byte jump table = 1018 entries × 8).
- belowA5 = 22266 bytes of globals.
- All 1018 JT entries are in the "unloaded" format (`off / 3F3C seg / A9F0`);
  the per-segment counts match the segment headers.

## Segments

| seg | name | size | purpose |
|---|---|---|---|
| 1 | (unnamed) | 1 KB | Think C startup |
| 2 | Main | 29 KB | main loop |
| 3,5 | Modular Set 1/2 | 25 KB | shared game code |
| 4,6 | Characters 1/2 | 24–26 KB | characters / animation |
| 7–10,18,23 | Ruins/Caverns/Final Battle/Temple/Desert/Rooftops | 4–15 KB | level code |
| 11,20–22 | Mohawk Libs 1–4 | 19–30 KB | Mohawk engine libraries |
| 12–14 | Editor 1–3 | 4 bytes | stripped editor (stubs) |
| 15 | NISs | 30 KB | cutscenes |
| 16 | Anim & Audio | 12 KB | animation / audio |
| 17 | PoP Tools | 29 KB | utilities |
| 19 | Mac Libs 2 | 20 KB | Mac glue |

## Control flow (flow_analyze.py)

- 100,102 instructions from the 1018 JT entries, **0 decoder errors**; 1808
  functions.
- 28–96% coverage per segment; the shortfall = data-in-code plus the targets
  of 553 indirect jmp/jsr (pointer tables — to be picked up later).
- NISs (28%) — much code is reachable only through pointers (cutscene scripts?).

## Traps (exact census: 1293 calls, 272 unique)

- Top: `_HUnlock` 96, `_HLock` 82, `_HPurge` 70, `_DisposePtr` 37,
  `_GetResource` 37, `_TickCount` 29, `_HNoPurge` 27, `_OpenResFile` 26.
- **Time Manager** (`_InsTime`/`_RmvTime`/`_PrimeTime`) — timing (music? MDRV).
- **`_SwapMMUMode` (14×)** — direct VRAM access in 32-bit mode — the confirmed
  risk from the plan (section 6.2 item 1): something draws around QuickDraw.
- **`_QDExtensions` 0xAB1D (20×)** — GWorlds, offscreen rendering.
- `_Pack3` — Standard File (save dialogs).
- `_SetTrapAddress` 9× — the game patches traps (find out which!).
- 30 rare unknowns (≤7 calls), mostly palette 0xAA94–0xAAA0 — to be clarified
  during implementation.
- IMPORTANT: the first version of the table had a +1 shift in 0xA985–0xA99F
  (caught on "ModalDialog at startup"; canonically: A991=ModalDialog,
  A992=DetachResource, A994=CurResFile, A997=OpenResFile, A998=UseResFile,
  A99A=CloseResFile, A99C=CountResources, A99D=GetIndResource). Resolves the
  former unknowns: A98B=_ParamText, A98F=_SetIText. Fixed in analyze_code.py
  + traps.cpp.

## Think C startup (seg 1, flow confirmed by execution)

`0x2E0`: a GetResource('STRS', id++) / GetResAttrs (BTST #5 — purgeable)
counting loop; then ResrvMem(size) → LoadResource → DetachResource → HLock →
store the string-pool pointer. The Resource Manager must tolerate GetResource
on a nonexistent id (return 0, resNotFound) — that is the normal exit from the
counting loop.

## Sound / music

- In the application: 34 `snd ` (id 5000–5165), 34 `INST`, `MDRV` id 11
  (13 KB, a custom MIDI driver?), `SMOD` ×4 (sound modifiers?).
- In Data: DigiSnd.dat / MIDISnd.dat / NISDIGI.dat / NISMIDI.dat — data in the
  data forks, format TBD (likely a sample bank + MIDI sequences).

## Misc

- `Mohawk.ini`, `TMPL` (ResEdit templates), `DLOG/DITL/ALRT/MENU/MBAR/WIND` —
  standard UI. `dctb/wctb` — window color tables.
- The 4-byte editor segment stubs — to be treated as no-ops.
