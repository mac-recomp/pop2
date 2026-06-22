// PoP2 web entry. The game data is preloaded into MEMFS at /data (see the
// --preload-file mappings in CMakeLists.txt). Pass the CODE directory and the
// data volume as argv, mirroring the native `pop2 <CODE dir> <data volume>`
// invocation that main.cpp expects (argv[1] = CODE dir, argv[2] = data root).
Module['arguments'] = ['/data/app/CODE', '/data/pop2'];

// When run under Node (headless verification), bridge POP2_* environment
// variables into the Emscripten ENV so the game's debug/headless hooks
// (POP2_NO_VIDEO, POP2_DUMP_FB, POP2_AUTOKEY, ...) behave exactly like the
// native build. In the browser `process` is undefined, so this is a no-op.
Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  if (typeof process !== 'undefined' && process.env) {
    for (var k in process.env) {
      if (k.indexOf('POP2_') === 0) ENV[k] = process.env[k];
    }
  }
});

// The build is INVOKE_RUN=0 (the browser shell starts the guest via callMain
// after the start gesture / data load). Headless Node runs have no shell, so
// start the guest directly there. In the browser `process` is undefined.
if (typeof process !== 'undefined' && process.versions && process.versions.node) {
  // Expose the module to the headless test harness (tools/wasm-save-test.cjs):
  // pop2.js is an IIFE, so its Module is otherwise unreachable from the require()
  // caller, and the exports must not be called until the runtime is initialized
  // (calling earlier aborts and poisons the instance). Node-only — the browser
  // never enters this branch.
  globalThis.__POP2_MODULE = Module;
  Module['onRuntimeInitialized'] = function () {
    globalThis.__POP2_READY = true;   // exports are safe to call from here on
    Module['callMain'](['/data/app/CODE', '/data/pop2']);
  };
}
