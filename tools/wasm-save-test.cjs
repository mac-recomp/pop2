// Headless round-trip test for the web save-manager, run under Node on the wasm
// build. Boots the guest, starts a level, saves to a named slot via the real
// Save path, and verifies the file landed under the chosen name with the 'POP2'
// magic, pop2_list_saves enumerates it, and pop2_load_slot drives an Open. The
// [sf] trap override is confirmed separately by grepping the guest's stderr.
//
//   source ~/emsdk/emsdk_env.sh && node tools/wasm-save-test.cjs 2>/tmp/guest.err
//
// Event loop: the guest's per-frame Asyncify yield drains in Node's check phase,
// which starves setTimeout (timers phase) — so the driver runs in the SAME phase
// via setImmediate (guest and driver interleave, ~one guest frame per turn) and
// measures elapsed time with Date.now(), not timers. Don't intercept
// process.stdout/stderr.write here: doing so wedges the guest's Node stdio at
// boot. pop2.js is an IIFE, so its Module is reached via globalThis.__POP2_MODULE
// (web/pre.js exposes it under Node).
const BUILD = '/home/h/src/pop2/PoP2MacRecomp/build-wasm';
const SLOT = 'Test Slot 42';
const fs = require('fs');

process.chdir(BUILD);
process.env.POP2_NO_VIDEO = '1';
process.env.POP2_NO_AUDIO = '1';

// Route the guest's yield through setImmediate so this driver's setImmediate
// turns interleave with it (the guest only installs its own if __pop2yield is unset).
globalThis.__pop2yield = () => new Promise((res) => setImmediate(res));

const result = {};
let Module = null, done = false;

function finish() {
  if (done) return; done = true;
  const ok = Object.keys(result).length > 0 && Object.values(result).every(Boolean);
  try { fs.writeFileSync('/tmp/save_test_result.json', JSON.stringify({ result, ok }, null, 2)); } catch (e) {}
  console.log('\n=== save-manager round-trip ===');
  for (const k of Object.keys(result)) console.log((result[k] ? 'PASS' : 'FAIL') + '  ' + k);
  console.log(ok ? '\nFS-CHECKS PASS' : '\nFAILURES PRESENT');
  process.exit(ok ? 0 : 1);
}

require(BUILD + '/pop2.js');   // pre.js calls callMain + exposes __POP2_MODULE under Node

const T0 = Date.now();
const elapsed = () => Date.now() - T0;
let phase = 0, mark = 0, saveAttempts = 0;
const readdir = () => { try { return Module.FS.readdir('/data/pop2'); } catch (e) { return []; } };

function step() {
  try {
    switch (phase) {
      case 0:  // wait for runtime init (pre.js sets __POP2_READY in
               // onRuntimeInitialized — calling an export before that aborts and
               // poisons the instance, so this must be a non-calling gate), bind Module
        if (globalThis.__POP2_READY && globalThis.__POP2_MODULE) {
          Module = globalThis.__POP2_MODULE; result.booted = true; mark = elapsed(); phase = 1;
        } else if (elapsed() > 40000) { result.booted = false; return finish(); }
        break;
      case 1:  // let the title/intro come up (Save is disabled until a game runs),
               // then start a new game with Cmd+N
        if (elapsed() - mark > 5000) { Module._pop2_menu_cmd('N'.charCodeAt(0)); mark = elapsed(); phase = 2; }
        break;
      case 2: {  // retry Save (Cmd+S) until the file lands: early on the game
                 // refuses ("unable to save at this point") while level 1 loads /
                 // the kid is mid-entry. The refusal Alert auto-dismisses, so retry.
        if (readdir().indexOf(SLOT) < 0 && elapsed() - mark > 3000 && saveAttempts < 12) {
          Module.ccall('pop2_save_slot', 'void', ['string'], [SLOT]); saveAttempts++; mark = elapsed();
        }
        if (readdir().indexOf(SLOT) >= 0 || saveAttempts >= 12) {
          result.fileWritten = readdir().indexOf(SLOT) >= 0;           // name == chosen slot
          let magic = false;
          if (result.fileWritten) {
            const b = Module.FS.readFile('/data/pop2/' + SLOT);
            magic = b.length >= 4 && String.fromCharCode(b[0], b[1], b[2], b[3]) === 'POP2';
          }
          result.magicPOP2 = magic;                                    // real save file
          result.listedBySlot = Module.ccall('pop2_list_saves', 'string', [], []).split('\n').indexOf(SLOT) >= 0;
          Module.ccall('pop2_load_slot', 'void', ['string'], [SLOT]);  // drive Open of the slot
          result.loadCallReturned = true;                              // ccall did not throw
          mark = elapsed(); phase = 3;
        }
        break;
      }
      case 3:  // let the Open run a few seconds, then finish
        if (elapsed() - mark > 3000) return finish();
        break;
    }
  } catch (e) { console.log('step error (phase ' + phase + '): ' + e.message); return finish(); }
  if (elapsed() > 90000) return finish();
  setImmediate(step);
}
setImmediate(step);
