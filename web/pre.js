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
