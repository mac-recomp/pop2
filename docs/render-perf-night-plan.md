# Night Session Plan — Smooth + Low-CPU Web Pacing

> Spec for an AUTONOMOUS overnight session. Launch with: "Read
> docs/render-perf-night-plan.md and execute it autonomously." No user is available —
> be safe, bounded, never break master, leave a morning report.

## The real problem (corrected 2026-06-25 — supersedes the earlier render-surgery framing)
The web build pins a CPU core because the recompiled 68k loop **busy-waits for its VBL**
(it re-pumps `video_pump()` in a tight loop until ~1/60 s of real time passes). The 100%
is that **busy-wait spin, NOT the render work** — so reducing render work (dirty-rect
surgery) would NOT lower CPU: the spin just absorbs the freed time. The one real lever is
to **idle that wait**. Two idle attempts regressed smoothness on real hardware:
- `emscripten_sleep` (setTimeout) made Firefox's compositor churn (main process 14%→125%).
- A `requestAnimationFrame` yield beat against the 16 ms present gate: the present interval
  drifts against the 16.67 ms vsync → periodic judder (user: "рывки присутствуют").

Current master state: **reverted to the smooth busy-loop** (`pop2_yield_to_browser` at the
present point — resume ASAP, browser composites at vsync). Smooth, but ~100% of one core.
Also: -O3, wasm ~12 MB, the palette convert is only ~0.4 ms/frame (GPU-palette ruled out).

## Mission
Make the loop **both smooth and low-CPU**: idle between frames with presents aligned to
vsync, no judder. Deliver a build the user can A/B by eye in the morning. Do NOT ship an
unverified pacing change to master.

## The fix to implement: present exactly once per vsync, no competing gate
- Standard browser-animation pattern: drive the present off `requestAnimationFrame`, **one
  present per rAF**, and **remove the separate 16 ms SDL_GetTicks gate** (that gate vs the
  16.67 ms vsync was the beat). `pop2_yield_to_frame()` (rAF yield) is already defined.
- Challenge: `video_pump()` is called many times per frame — event polls *in logic* AND the
  VBL busy-wait. Presenting/yielding on EVERY call blocks logic (slow-mo). So the
  present+rAF-yield must fire on the VBL-wait, not mid-logic. Variants to try:
  1. **Spin-detection**: count `video_pump` calls since the last present; once past a
     threshold (clearly the busy-wait, not logic) present + rAF-yield, reset. Profile
     logic's pumps/frame first and set the threshold above it, below the spin's.
  2. **rAF-time gate**: a flag set by the rAF callback; present only when a real vsync has
     elapsed since the last present (paced by rAF time, not SDL_GetTicks) — no 16-vs-16.67 beat.
  3. Combine: rAF-yield is the idle; the "have we drawn this vsync yet" flag prevents
     double-present and logic-blocking.
- The guest 68k loop is NOT restructured — it just blocks in the pump until its next frame.

## Verify smoothness HEADLESSLY via jitter (the key enabler)
Smoothness can't be eyeballed headless, but **frame-interval jitter can be measured**, and
that's the signature of the judder.
- Under `headless:'new'` (real compositor, ~62 fps), instrument the present path: record
  `emscripten_get_now()` at each present, and report **mean interval, std-dev, max gap, and
  the count of intervals > 1.5× mean** (≈ dropped/doubled frames). Smooth ⇒ ~16.7 ms mean,
  small std-dev, ~zero long gaps. The current rAF build (before revert) should show the beat
  (periodic long gaps) — reproduce that first as "known-bad", then iterate the fix until the
  jitter histogram is flat AND CPU is materially below the busy-loop AND fps ~60.
- Keep the existing CPU/fps measure (`~/pop2-webtest/cpu_run.mjs`, headless:'new') and the
  correctness suite (`addtime_test.mjs`, `time_test.mjs`, `plat_test.mjs`).

## Guardrails (autonomous)
1. Branch only (e.g. `pacing`). NEVER commit experiments to master; master stays the smooth
   deployable build. No deploy, no force-push, no deleting saves.
2. A pacing variant is "good" only if ALL hold: **flat jitter** (no periodic long gaps),
   **CPU materially below** the busy-loop, **fps ~60**, **correctness suite green**.
3. Build must always compile + run; revert any step that breaks it. Commit per verified step.
4. Bounded: a few variants. If none is both smooth (flat jitter) and low-CPU, **leave the
   smooth busy-loop on master** and document each variant's jitter+CPU numbers for the user
   to A/B by eye. Unverifiable smoothness ⇒ do not ship to master.
5. Morning report (DEV-LOG + final message): variants tried, their jitter + CPU + fps numbers,
   what's on the branch, and a clear recommendation for the user to test.

## Fallback / secondary
- **30 fps toggle** in the assists: halves per-frame work; still needs smooth pacing (same
  jitter check). A reasonable user-facing "cooler, choppier" option if 60 fps can't be made cool.
- **Render dirty-rect surgery: de-prioritised.** It does NOT lower CPU while the loop
  busy-waits (the spin absorbs render savings). Only revisit if, against expectation, the
  jitter/CPU profiling shows the per-frame render dominates even once the wait is idled.

## Tooling / env / conventions
- Build native: `ninja -C build pop2`. Build wasm: `ninja -C build-wasm pop2` (-O3, ~2 min).
- Serve wasm: `cd build-wasm && python3 -m http.server 8088` (the user opens :8088).
- Measure: `~/pop2-webtest/cpu_run.mjs` (headless:'new' → CPU/fps/[conv]; extend it to log the
  present-interval jitter). Correctness: `addtime_test.mjs`, `time_test.mjs`, `plat_test.mjs`
  (run `bun` from ~/.bun/bin). The `[conv]` profiler + fps trace are gated behind `#fps` in the URL.
- The present point is in `video.cpp` `video_pump()` (the `now - s_last_present >= 16` block);
  `pop2_yield_to_browser` (resume ASAP) and `pop2_yield_to_frame` (rAF yield) are near the top.
- Hygiene: `pkill -x pop2` after gdb + at session end; `pgrep`/`pkill` **-x only** (never `-f`
  — it self-matches). Don't poll background tasks (you're re-invoked when they finish).
- Git: author=committer `superheher <heh@vivaldi.net>`, **no co-author trailers**, English
  commits by meaning, journal in docs/DEV-LOG.md. Work on a branch; never touch master.

## Definition of done
- WIN: a branch commit with flat jitter + low CPU + ~60 fps + green correctness + DEV-LOG +
  a morning report recommending the user A/B it (smoothness is theirs to confirm).
- NO-WIN: smooth busy-loop stays on master; variants documented with numbers; recommend the
  30 fps toggle or "leave smooth-but-warm". STOP — don't churn.
