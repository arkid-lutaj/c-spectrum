cmake_minimum_required(VERSION 3.14)
project(cspectrum C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The window is optional. Without it the core, the CLI and the tests still
# build, which is what CI uses so it doesn't need a display or X11 headers.
option(CSPECTRUM_GUI   "Build the graphical interface" ON)
option(CSPECTRUM_TESTS "Build the unit tests"          ON)

if(MSVC)
    add_compile_options(/W4)

    # MSVC ships <stdatomic.h> but keeps C atomics behind a switch even in C11
    # mode; without it the header is just an #error. Needs VS 2022 17.5 or
    # newer (cl 19.35). Older MSVC cannot build the ring buffer at all.
    if(CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 19.35)
        add_compile_options(/experimental:c11atomics)
    else()
        message(FATAL_ERROR
            "MSVC ${CMAKE_C_COMPILER_VERSION} has no C11 atomics. "
            "Use Visual Studio 2022 17.5+, or build with MinGW-w64 or clang-cl.")
    endif()

    # Stop it complaining about fopen and friends.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wshadow -Wstrict-prototypes)
endif()

# ---- dependencies -----------------------------------------------------------

add_library(kissfft STATIC
    deps/kissfft/kiss_fft.c
    deps/kissfft/kiss_fftr.c
)
target_include_directories(kissfft PUBLIC deps/kissfft)
if(NOT MSVC)
    # Vendored code, not ours to keep warning free.
    target_compile_options(kissfft PRIVATE -w)
endif()

# ---- core library -----------------------------------------------------------
# Everything except the window and main(). Both the executable and the tests
# link against this, so the tests exercise the shipping code and not a copy.

add_library(cspectrum_core STATIC
    src/cs_config.c
    src/cs_ringbuf.c
    src/cs_thread.c
    src/cs_biquad.c
    src/cs_features.c
    src/cs_analysis.c
    src/cs_envelope.c
    src/cs_monitor.c
    src/cs_synth.c
    src/cs_source.c
    src/cs_telemetry.c
    src/cs_engine.c
    src/cs_report.c
)

target_include_directories(cspectrum_core PUBLIC
    src
    deps/miniaudio
    deps/kissfft
)

target_link_libraries(cspectrum_core PUBLIC kissfft)

find_package(Threads REQUIRED)
target_link_libraries(cspectrum_core PUBLIC Threads::Threads)

if(NOT MSVC)
    target_link_libraries(cspectrum_core PUBLIC m)
endif()

if(WIN32)
    # miniaudio's WASAPI backend.
    target_link_libraries(cspectrum_core PUBLIC winmm ole32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(cspectrum_core PUBLIC dl)
elseif(APPLE)
    target_link_libraries(cspectrum_core PUBLIC
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
endif()

# ---- executable -------------------------------------------------------------

add_executable(cspectrum src/main.c)
target_link_libraries(cspectrum PRIVATE cspectrum_core)

if(CSPECTRUM_GUI)
    include(FetchContent)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG        5.5
        GIT_SHALLOW    TRUE
    )
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
    set(CUSTOMIZE_BUILD ON CACHE BOOL "" FORCE)
    # raylib brings its own miniaudio for audio playback, and we only need it
    # to draw, so turn that off. Two copies of miniaudio in one binary is a
    # pile of duplicate symbols.
    set(USE_AUDIO OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(raylib)

    target_sources(cspectrum PRIVATE src/cs_gui.c)
    target_compile_definitions(cspectrum PRIVATE CS_WITH_GUI)
    target_link_libraries(cspectrum PRIVATE raylib)
endif()

# ---- tests ------------------------------------------------------------------

if(CSPECTRUM_TESTS)
    enable_testing()

    add_executable(cspectrum_tests
        tests/test_main.c
        tests/test_ringbuf.c
        tests/test_biquad.c
        tests/test_analysis.c
        tests/test_features.c
        tests/test_monitor.c
        tests/test_envelope.c
        tests/test_config.c
    )
    target_link_libraries(cspectrum_tests PRIVATE cspectrum_core)
    target_include_directories(cspectrum_tests PRIVATE src tests)

    add_test(NAME unit COMMAND cspectrum_tests)
endif()

message(STATUS "cspectrum: gui=${CSPECTRUM_GUI} tests=${CSPECTRUM_TESTS} build=${CMAKE_BUILD_TYPE}")
