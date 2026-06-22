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

    target_compile_options(pop2 PRIVATE "-sUSE_SDL=2")
    target_link_options(pop2 PRIVATE
        "-sUSE_SDL=2"
        "-sASYNCIFY"
        "-sASYNCIFY_STACK_SIZE=1048576"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=268435456"   # 256 MB: 16 MB guest + ~36 MB MEMFS + SDL
        "-sSTACK_SIZE=5242880"         # deep recompiled C++ call chains
        "-sEXIT_RUNTIME=0"             # guest loop never returns (ExitToShell)
        "-sFORCE_FILESYSTEM=1"
        # SHELL: keeps CMake from de-duplicating the repeated --preload-file /
        # --pre-js flag tokens (de-dup silently drops the 2nd mapping, leaving
        # it dangling as a bogus input file).
        "SHELL:--shell-file ${CMAKE_SOURCE_DIR}/web/shell.html"
        "SHELL:--pre-js ${CMAKE_SOURCE_DIR}/web/pre.js"
        "SHELL:--preload-file ${CMAKE_BINARY_DIR}/assets/app@/data/app"
        "SHELL:--preload-file ${CMAKE_BINARY_DIR}/assets/data@/data/data"
        "SHELL:--preload-file ${CMAKE_BINARY_DIR}/assets/vol@/data/pop2"
    )
    set_target_properties(pop2 PROPERTIES SUFFIX ".html")
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(pop2 PRIVATE SDL2::SDL2)
endif()
