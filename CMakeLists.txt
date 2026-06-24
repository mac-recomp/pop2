cmake_minimum_required(VERSION 3.20)
project(pop2 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Generated sources: run `python3 pop2-recomp/recomp.py extracted/app/CODE gen`
file(GLOB GEN_SOURCES ${CMAKE_SOURCE_DIR}/gen/*.cpp)
if(NOT GEN_SOURCES)
    message(FATAL_ERROR "no generated sources in gen/ — run recomp.py first")
endif()

file(GLOB RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/pop2-runtime/src/*.cpp)
add_executable(pop2
    ${GEN_SOURCES}
    ${RUNTIME_SOURCES}
)
target_include_directories(pop2 PRIVATE
    pop2-runtime/include
    ${CMAKE_SOURCE_DIR}/gen
)
target_compile_options(pop2 PRIVATE -Wall -Wno-unused-variable -Wno-unused-label)

if(EMSCRIPTEN)
    # --- WebAssembly target: emcmake cmake -G Ninja .. ---
    # SDL2 is Emscripten's built-in port (no find_package). ASYNCIFY lets the
    # deeply-nested recompiled main loop yield to the browser at its existing
    # pump points (video.cpp/traps.cpp), so none of gen/*.cpp is restructured.
    # Mirror the native asset layout. main.cpp passes argv[1]=<app>/CODE and
    # argv[2]=<volume>; resource_mgr derives its dump root as <app>/../data.
    # So the three host trees must mount as:
    #   /data/app    app resources + CODE   (argv[1] = /data/app/CODE)
    #   /data/data   resource dumps         (= /data/app/../data, rm s_data_root)
    #   /data/pop2   raw game volume        (argv[2]; .dat files via file_mgr)
    set(POP2_APP_DIR  "${CMAKE_SOURCE_DIR}/extracted/app"
        CACHE PATH "app resources + CODE segments")
    set(POP2_RSRC_DIR "${CMAKE_SOURCE_DIR}/extracted/data"
        CACHE PATH "extracted resource dumps (Prince/Kid/NIS/...)")
    set(POP2_VOL_DIR  "${CMAKE_SOURCE_DIR}/../extracted_sit/Prince of Persia 2"
        CACHE PATH "raw game data volume (.dat files)")

    # Space-free symlinks so the --preload-file 'src@dst' mappings parse
    # cleanly (the raw volume path contains spaces).
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/assets")
    file(CREATE_LINK "${POP2_APP_DIR}"  "${CMAKE_BINARY_DIR}/assets/app"  SYMBOLIC)
    file(CREATE_LINK "${POP2_RSRC_DIR}" "${CMAKE_BINARY_DIR}/assets/data" SYMBOLIC)
    file(CREATE_LINK "${POP2_VOL_DIR}"  "${CMAKE_BINARY_DIR}/assets/vol"  SYMBOLIC)

    # Stack-switching for the blocking main loop: JSPI (engine-native, near-zero
    # overhead, Chromium 137+) or Asyncify (universal, adds code instrumentation
    # plus a per-yield cost). Default to Asyncify for broad browser support.
    option(POP2_WASM_JSPI "Use JSPI instead of Asyncify (Chromium 137+)" OFF)
    if(POP2_WASM_JSPI)
        set(POP2_STACK_SWITCH "-sJSPI")
    else()
        set(POP2_STACK_SWITCH "-sASYNCIFY" "-sASYNCIFY_STACK_SIZE=1048576")
    endif()

    # Data delivery: bundle it via --preload-file (default; convenient for dev),
    # or ship the engine alone and let the page's file picker load the user's own
    # game-data archive into MEMFS, then start the guest via callMain.
    option(POP2_WASM_NO_PRELOAD "Load game data via the in-page file picker instead of bundling it" OFF)
    # Both modes use the same shell (it drives #stage[data-state]) and start the
    # guest via callMain. The default build also bundles the data via
    # --preload-file; the no-preload build leaves it out and the shell's
    # drop-zone loads the player's archive into MEMFS.
    set(POP2_DATA_OPTS
        "SHELL:--shell-file ${CMAKE_SOURCE_DIR}/web/shell.html"
        "-sINVOKE_RUN=0"
        "-sEXPORTED_RUNTIME_METHODS=callMain,FS,ccall,HEAPU8")
    if(NOT POP2_WASM_NO_PRELOAD)
        list(APPEND POP2_DATA_OPTS
            "SHELL:--preload-file ${CMAKE_BINARY_DIR}/assets/app@/data/app"
            "SHELL:--preload-file ${CMAKE_BINARY_DIR}/assets/data@/data/data"
            "SHELL:--preload-file ${CMAKE_BINARY_DIR}/assets/vol@/data/pop2")
    endif()

    # Web build optimization. At -O0 the recompiled 68k engine runs interpreted-slow
    # and pegs a full CPU core (≈100% of one thread), which spins up laptop fans; -O2
    # runs it several times faster AND is smaller (~12 MB vs the -O0 -g0 ~23 MB wasm).
    # -g0 strips the DWARF debug info (most of the remaining bloat).
    #   An earlier revision blamed a "slow HP drain" on an -O1/-O2 miscompile. That was
    #   wrong — the drain was traced (POP2_WATCH on the HP word + addr2line) to intended
    #   level-14 finale logic: f2_54d6 integrates HP toward zero and f6_4382 drives it,
    #   both gated on the env8582==6 finale flag, and it reproduces on -O0 native too.
    #   So -O2 is safe; it's the verified web choice. (Native still builds -O0.)
    target_compile_options(pop2 PRIVATE -O3 -g0)
    target_link_options(pop2 PRIVATE -O3 -g0)

    target_compile_options(pop2 PRIVATE "-sUSE_SDL=2")
    target_link_options(pop2 PRIVATE
        "-sUSE_SDL=2"
        ${POP2_STACK_SWITCH}
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=268435456"   # 256 MB: 16 MB guest + ~36 MB MEMFS + SDL
        "-sSTACK_SIZE=5242880"         # deep recompiled C++ call chains
        "-sEXIT_RUNTIME=0"             # guest loop never returns (ExitToShell)
        "-sFORCE_FILESYSTEM=1"
        "-lidbfs.js"                   # IDBFS for persistent saves (see main.cpp)
        # SHELL: keeps CMake from de-duplicating the repeated --preload-file /
        # --pre-js flag tokens (de-dup silently drops the 2nd mapping, leaving
        # it dangling as a bogus input file).
        "SHELL:--pre-js ${CMAKE_SOURCE_DIR}/web/pre.js"
        ${POP2_DATA_OPTS}
    )
    set_target_properties(pop2 PROPERTIES SUFFIX ".html")
    # Re-link when the shell/pre-js change (Ninja doesn't track --shell-file /
    # --pre-js inputs on its own, so an HTML-only edit would otherwise no-op).
    set_target_properties(pop2 PROPERTIES
        LINK_DEPENDS "${CMAKE_SOURCE_DIR}/web/shell.html;${CMAKE_SOURCE_DIR}/web/pre.js")
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(pop2 PRIVATE SDL2::SDL2)
endif()
