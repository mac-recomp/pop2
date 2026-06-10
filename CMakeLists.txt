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

find_package(SDL2 REQUIRED)
target_link_libraries(pop2 PRIVATE SDL2::SDL2)
