# Native (PC/mobile) build of Pikmin 2 on top of Aurora.
# The original matching build is unaffected: it still uses configure.py + ninja + MWCC.
cmake_minimum_required(VERSION 3.25)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif ()

project(mppm LANGUAGES C CXX VERSION 0.1.0)
if (APPLE)
    enable_language(OBJC OBJCXX)
    if (NOT CMAKE_OSX_SYSROOT)
        execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
                OUTPUT_VARIABLE CMAKE_OSX_SYSROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif ()
endif ()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)
include(cmake/mppm_gnu_options.cmake) # GNU-style options, translated for clang-cl
if (WIN32)
    # No min/max macros from <windows.h>; no "use the _s functions" warnings from the CRT.
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif ()
if (MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Build with clang-cl on Windows (-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl), not cl.exe")
endif ()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MPPM_WARNINGS "Show compiler warnings for game code" OFF)

# Aurora uses std::jthread/std::stop_token, which Apple's libc++ (Xcode 16.x) still gates behind
# -fexperimental-library.
if (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fexperimental-library>)
    add_link_options(-fexperimental-library)
endif ()

# ---- Aurora (GX/VI/PAD/DVD/CARD/OS over SDL3 + WebGPU) ----
set(AURORA_ENABLE_DVD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI ON CACHE BOOL "" FORCE) # launcher / in-game menus (port/launcher)
# Local fixes to Aurora (port/patches/aurora/*.patch), applied to the submodule checkout once.
find_package(Git QUIET)
file(GLOB AURORA_PATCHES ${CMAKE_SOURCE_DIR}/port/patches/aurora/*.patch)
list(SORT AURORA_PATCHES)
foreach (patch ${AURORA_PATCHES})
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${patch}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/extern/aurora RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
    if (NOT already EQUAL 0)
        execute_process(COMMAND ${GIT_EXECUTABLE} apply ${patch}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/extern/aurora RESULT_VARIABLE failed)
        if (failed)
            message(FATAL_ERROR "Could not apply ${patch} to extern/aurora")
        endif ()
        message(STATUS "Applied ${patch}")
    endif ()
endforeach ()
if (APPLE AND NOT MPPM_USE_HOMEBREW_PACKAGES)
    # Never link Homebrew's libraries (zstd, libpng, ...): a shipped app cannot depend on them, and
    # on Intel Macs their /usr/local include path is dropped as an implicit directory. Aurora and
    # borealis then build their own static copies. Only llvm-objcopy is taken from Homebrew.
    option(MPPM_USE_HOMEBREW_PACKAGES "Let dependencies use Homebrew packages (local builds only)" OFF)
    list(APPEND CMAKE_IGNORE_PREFIX_PATH /opt/homebrew /usr/local)
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)
endif ()
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)
if (APPLE)
    # aurora/lib/system_info.cpp relies on TargetConditionals.h being included transitively (newer SDKs only).
    target_compile_options(aurora_core PRIVATE -include TargetConditionals.h)
endif ()
target_compile_definitions(aurora_mtx PRIVATE MTX_USE_PS=1)

# ---- Borealis (encounter/borealis, MIT): RmlUi component framework, config, file dialogs, disc
# verification and GitHub update checks for the launcher. ----
set(BOREALIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BOREALIS_ENABLE_UI OFF CACHE BOOL "" FORCE) # UI framework ported from Dusklight (port/launcher/ui): matches our Aurora
set(BOREALIS_ENABLE_CLI OFF CACHE BOOL "" FORCE)
set(BOREALIS_ENABLE_WEBSOCKET ON CACHE BOOL "" FORCE) # M5.3b lobby client (port/net/lobby_client.cpp)
set(BOREALIS_ENABLE_NET OFF CACHE BOOL "" FORCE)
set(BOREALIS_ENABLE_CRASH OFF CACHE BOOL "" FORCE)
set(BOREALIS_ENABLE_SENTRY OFF CACHE BOOL "" FORCE)
set(BOREALIS_ENABLE_DISCORD OFF CACHE BOOL "" FORCE)
set(BOREALIS_CURL_PROVIDER "system" CACHE STRING "" FORCE)
add_subdirectory(extern/borealis EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)

# ---- Game sources (generated from configure.py) ----
include(cmake/sources.cmake)
set(P2_GAME_SOURCES)
foreach (lib IN LISTS P2_LIBS)
    list(APPEND P2_GAME_SOURCES ${${lib}})
endforeach ()

# The only .c files in the game libraries (JAudio dsp, sysGCU THP) are built with MWCC -lang=c++.
set(_p2_c_sources ${P2_GAME_SOURCES})
list(FILTER _p2_c_sources INCLUDE REGEX "\\.c$")
set_source_files_properties(${_p2_c_sources} PROPERTIES LANGUAGE CXX)

file(GLOB_RECURSE PORT_SOURCES CONFIGURE_DEPENDS port/*.c port/*.cpp)
# The launcher is ordinary host C++ (exceptions, RTTI, no decomp headers): its own library.
list(FILTER PORT_SOURCES EXCLUDE REGEX "/port/launcher/")
# The vendored math library is its own target (p2libm, below).
list(FILTER PORT_SOURCES EXCLUDE REGEX "/port/libm/")
file(GLOB_RECURSE LAUNCHER_SOURCES CONFIGURE_DEPENDS port/launcher/*.cpp)
# New game-side code (the start-menu scene, docs/launcher/SCENE.md) is built like the game itself:
# same options and forced types.h, and its operator new/delete go to JKRHeap like the game's.
# It is not in cmake/sources.cmake, so the MWCC matching build never sees it.
file(GLOB_RECURSE PORT_GAME_SOURCES CONFIGURE_DEPENDS port/game/*.cpp)
list(FILTER PORT_SOURCES EXCLUDE REGEX "/port/game/")
list(APPEND P2_GAME_SOURCES ${PORT_GAME_SOURCES})

# MTX_USE_PS: declare the PSMTX* names (Aurora maps them to its C implementation).
set(P2_DEFINES TARGET_PC=1 VERSION_GPVE01=1 MTX_USE_PS=1)

# ---- Vendored libm (port/libm, musl 1.2.5 subset; docs/online/M5_1B_BUILD.md) ----
# The game's math goes to p2m_* functions that give the same bits on every host and CPU, instead
# of the system libm (glibc picks FMA or AVX2 variants by CPU; macOS has its own). Its public
# functions are listed in port/libm/names.txt; the inexact ones also lose their builtin status in
# the game (P2_OPTIONS below).
# MPPM_VENDORED_LIBM=OFF is for A/B runs only: the game then uses the system libm again, whose
# results depend on the CPU and the libm version (not deterministic between machines).
option(MPPM_VENDORED_LIBM "Route the game's math to the vendored p2libm" ON)
file(STRINGS ${CMAKE_SOURCE_DIR}/port/libm/names.txt P2LIBM_NAMES REGEX "^[a-z]")
set(P2LIBM_INEXACT sinf cosf tanf asinf acosf atanf atan2f expf logf log10f powf
        sin cos tan asin acos atan atan2 exp log log10 pow)
file(GLOB P2LIBM_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/port/libm/src/*.c)
add_library(p2libm STATIC ${P2LIBM_SOURCES} port/libm/p2m_sqrt.c)
set_target_properties(p2libm PROPERTIES LINKER_LANGUAGE C)
target_include_directories(p2libm PRIVATE ${CMAKE_SOURCE_DIR}/port/libm/internal ${CMAKE_SOURCE_DIR}/port/libm/src
        PUBLIC ${CMAKE_SOURCE_DIR}/port/libm)
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_include_directories(p2libm PRIVATE ${CMAKE_SOURCE_DIR}/port/libm/compat) # <features.h>
endif ()
# -fno-math-errno: sqrt/sqrtf are then the bare instruction (musl never sets errno anyway).
mppm_target_gnu_options(p2libm PRIVATE -include ${CMAKE_SOURCE_DIR}/port/libm/p2m_names.h
        -ffp-contract=off -fno-builtin -fno-fast-math -fno-math-errno -w)

# `ninja -C build-pc p2libm_selftest`, then `./build-pc/p2libm_selftest port/libm/selftest_expected.txt`.
add_executable(p2libm_selftest EXCLUDE_FROM_ALL port/libm/selftest.c)
mppm_target_gnu_options(p2libm_selftest PRIVATE -ffp-contract=off -fno-builtin -fno-fast-math)
target_link_libraries(p2libm_selftest PRIVATE p2libm $<$<NOT:$<PLATFORM_ID:Windows>>:m>)

# Zero on alloc (JKRHeap::alloc, port/sdk/heap_zero.cpp) and -ftrivial-auto-var-init=zero.
option(MPPM_ZERO_INIT "Game heap allocations and uninitialised locals read as zero" ON)
include(CheckCXXCompilerFlag)
if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    check_cxx_compiler_flag(/clang:-ftrivial-auto-var-init=zero P2_HAVE_AUTO_VAR_INIT_ZERO)
else ()
    check_cxx_compiler_flag(-ftrivial-auto-var-init=zero P2_HAVE_AUTO_VAR_INIT_ZERO)
endif ()
if (MPPM_ZERO_INIT)
    set_source_files_properties(port/sdk/heap_zero.cpp PROPERTIES COMPILE_DEFINITIONS MPPM_ZERO_INIT=1)
else ()
    set_source_files_properties(port/sdk/heap_zero.cpp PROPERTIES COMPILE_DEFINITIONS MPPM_ZERO_INIT=0)
endif ()

set(P2_OPTIONS)
# GNU-style list; clang-cl (frontend MSVC) gets it translated by mppm_target_gnu_options.
if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    list(APPEND P2_OPTIONS
            -fsigned-char            # MWCC char is signed
            -fno-strict-aliasing     # decomp code type-puns freely
            -fwrapv
            -ffp-contract=off        # deterministic float math (netplay)
            -fno-exceptions
            -fno-rtti
            # Many original functions fall off the end without returning a value (MWCC returned
            # whatever was in r3). Clang would treat that as unreachable and run into the next
            # function; return an unspecified value instead.
            $<$<COMPILE_LANGUAGE:CXX>:-fno-strict-return>
            -Wno-multichar
            -Wno-parentheses        # e.g. "a <= x < b" in the original code; behaviour kept
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-c++11-narrowing>
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-register>
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-reserved-user-defined-literal>
            -Wno-error=incompatible-pointer-types
            -Wno-error=int-conversion
            -Wno-error=implicit-function-declaration
    )
    if (NOT MPPM_WARNINGS)
        list(APPEND P2_OPTIONS -w)
    endif ()
    # Vendored libm (docs/online/M5_1B_BUILD.md): the game's math calls are routed to p2libm below.
    # The compiler must not evaluate an inexact one itself (constant folding with the build
    # machine's libm, llvm.sin intrinsics, merging sin and cos into sincos), so it gets no builtin
    # knowledge of them; -fmath-errno keeps libstdc++'s std::sin(float) (__builtin_sinf) a real call.
    if (MPPM_VENDORED_LIBM)
        list(APPEND P2_OPTIONS -fmath-errno)
        foreach (_fn IN LISTS P2LIBM_INEXACT)
            list(APPEND P2_OPTIONS -fno-builtin-${_fn})
        endforeach ()
    endif ()
    # Uninitialised locals read as zero (determinism checklist item 13). MPPM_ZERO_INIT=OFF gives
    # the old behaviour, with heap memory left as it is too (port/sdk/heap_zero.cpp), for A/B runs.
    if (MPPM_ZERO_INIT AND P2_HAVE_AUTO_VAR_INIT_ZERO)
        list(APPEND P2_OPTIONS -ftrivial-auto-var-init=zero)
    endif ()
endif ()

# Aurora's SDK headers come before the decomp's include/ so <dolphin/...> lookups (and, on
# case-insensitive filesystems, "Dolphin/...") resolve to Aurora; the decomp's own copies of
# overlapping SDK headers also forward there under TARGET_PC.
set(P2_INCLUDES
        ${CMAKE_SOURCE_DIR}/port/include
        ${CMAKE_SOURCE_DIR}/extern/aurora/include
        ${CMAKE_SOURCE_DIR}/include
)
# The decomp's C/C++ library wrappers are only for the game's own quoted includes ("string.h");
# keeping them off the <...> search path stops Aurora and system headers from reaching them.
list(APPEND P2_OPTIONS -iquote ${CMAKE_SOURCE_DIR}/include/stl)
# Every game translation unit sees types.h (and through it the SDK compat layer) first, even
# headers that only include SDK headers, which resolve straight to Aurora.
list(APPEND P2_OPTIONS -include ${CMAKE_SOURCE_DIR}/include/types.h)

add_library(p2game STATIC ${P2_GAME_SOURCES})
target_compile_definitions(p2game PRIVATE ${P2_DEFINES})
mppm_target_gnu_options(p2game PRIVATE ${P2_OPTIONS})
target_include_directories(p2game PRIVATE ${P2_INCLUDES})
target_link_libraries(p2game PRIVATE aurora::core)

set(AURORA_LIBS aurora::core aurora::gx aurora::gd aurora::si aurora::vi aurora::pad
        aurora::mtx aurora::os aurora::dvd aurora::card aurora::thp)

# Game code allocates through JKRHeap via the global operator new/delete (JKRHeap.cpp). Host
# libraries (Aurora, Dawn, SDL, the C++ runtime) must not: they allocate from their own threads and
# keep memory alive across game heap resets. Rename the plain operator new/delete symbols inside
# the game objects only, so game code (and JKRHeap.cpp's definitions) use p2_operator_* while
# everything else keeps the C++ runtime's malloc-based ones.
if (APPLE)
    set(P "_")
else ()
    set(P "")
endif ()
# The C++ operator names in the list are Itanium-mangled; the MSVC ABI (Windows) has its own.
if (CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC" OR MSVC)
    set(P2_NEW "??2@YAPEAX_K@Z")
    set(P2_NEW_ARRAY "??_U@YAPEAX_K@Z")
    set(P2_DELETE "??3@YAXPEAX@Z")
    set(P2_DELETE_ARRAY "??_V@YAXPEAX@Z")
    set(P2_DELETE_SIZED "??3@YAXPEAX_K@Z")
    set(P2_DELETE_ARRAY_SIZED "??_V@YAXPEAX_K@Z")
else ()
    set(P2_NEW "${P}_Znwm")
    set(P2_NEW_ARRAY "${P}_Znam")
    set(P2_DELETE "${P}_ZdlPv")
    set(P2_DELETE_ARRAY "${P}_ZdaPv")
    set(P2_DELETE_SIZED "${P}_ZdlPvm")
    set(P2_DELETE_ARRAY_SIZED "${P}_ZdaPvm")
endif ()
configure_file(cmake/p2_new_syms.txt.in ${CMAKE_BINARY_DIR}/p2_new_syms.all.txt @ONLY)
file(STRINGS ${CMAKE_BINARY_DIR}/p2_new_syms.all.txt _p2_syms)
if (NOT MPPM_VENDORED_LIBM)
    list(FILTER _p2_syms EXCLUDE REGEX "p2m_")
endif ()
list(JOIN _p2_syms "\n" _p2_syms)
file(WRITE ${CMAKE_BINARY_DIR}/p2_new_syms.tmp "${_p2_syms}\n")
configure_file(${CMAKE_BINARY_DIR}/p2_new_syms.tmp ${CMAKE_BINARY_DIR}/p2_new_syms.txt COPYONLY) # rewritten only on a change
# macOS has no objcopy: Homebrew's llvm (brew install llvm) provides llvm-objcopy, found here.
find_program(P2_OBJCOPY NAMES llvm-objcopy objcopy HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin REQUIRED)
# The rename runs only when the archive is linked again: make one game object depend on the list,
# so editing cmake/p2_new_syms.txt.in re-creates the archive and renames it with the new list.
list(GET PORT_GAME_SOURCES 0 _p2_syms_dep_source)
set_property(SOURCE ${_p2_syms_dep_source} APPEND PROPERTY OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/p2_new_syms.txt)
add_custom_command(TARGET p2game POST_BUILD
        COMMAND ${P2_OBJCOPY} --redefine-syms=${CMAKE_BINARY_DIR}/p2_new_syms.txt $<TARGET_FILE:p2game>
        COMMENT "Routing game operator new/delete to JKRHeap and math to p2libm"
        VERBATIM)

# Aurora's matrix library is reached by simulation code (MTXRotRad, MTXRotAxisRad, quaternions,
# the camera's cull planes): its math goes to p2libm too. A forced header renames the calls after
# <math.h> is read, so extern/ needs no edit.
if (MPPM_VENDORED_LIBM)
    mppm_target_gnu_options(aurora_mtx PRIVATE -include ${CMAKE_SOURCE_DIR}/port/libm/p2m_route_mtx.h -ffp-contract=off)
    target_include_directories(aurora_mtx PRIVATE ${CMAKE_SOURCE_DIR}/port/libm)
    target_link_libraries(aurora_mtx PRIVATE p2libm)
endif ()

# Build check: no system libm function may be left in the game library or in aurora_mtx, and the
# game library may not define one (an out-of-line copy of trig.h's sinf would become a second
# p2m_sinf). Runs after the objcopy step; cmake/p2_libm_check.cmake.
if (NOT CMAKE_NM)
    find_program(CMAKE_NM NAMES llvm-nm nm REQUIRED) # clang-cl builds do not set it
endif ()
set(P2LIBM_CHECK_STAMP ${CMAKE_BINARY_DIR}/p2libm_check.stamp)
add_custom_command(OUTPUT ${P2LIBM_CHECK_STAMP}
        COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DPREFIX=${P}
                -DNAMES=${CMAKE_SOURCE_DIR}/cmake/p2_libm_names.txt
                "-DLIBS=$<TARGET_FILE:p2game>|$<TARGET_FILE:aurora_mtx>"
                "-DNODEF_LIBS=$<TARGET_FILE:p2game>"
                -DSTAMP=${P2LIBM_CHECK_STAMP}
                -P ${CMAKE_SOURCE_DIR}/cmake/p2_libm_check.cmake
        DEPENDS p2game aurora_mtx $<TARGET_FILE:p2game> $<TARGET_FILE:aurora_mtx>
                ${CMAKE_SOURCE_DIR}/cmake/p2_libm_check.cmake ${CMAKE_SOURCE_DIR}/cmake/p2_libm_names.txt
        COMMENT "Checking that game math uses p2libm"
        VERBATIM)
if (MPPM_VENDORED_LIBM)
    add_custom_target(p2libm_check DEPENDS ${P2LIBM_CHECK_STAMP})
else ()
    add_custom_target(p2libm_check COMMAND ${CMAKE_COMMAND} -E echo "MPPM_VENDORED_LIBM=OFF: game math uses the system libm (A/B build, not deterministic between machines)" VERBATIM)
endif ()

add_executable(mppm ${PORT_SOURCES})
target_compile_definitions(mppm PRIVATE ${P2_DEFINES})
target_include_directories(mppm PRIVATE ${P2_INCLUDES})
target_link_libraries(mppm PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,p2game>" aurora::main ${AURORA_LIBS} p2libm Threads::Threads)
add_dependencies(mppm p2libm_check)

# Version string for the launcher (port/launcher/branding.hpp): a v* tag gives "1.2.0", anything else
# "<project version>-dev+<git describe>". Reconfigures when HEAD moves.
set(MPPM_GIT_DESCRIBE "")
find_package(Git QUIET)
if (GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --match "v*"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE MPPM_GIT_DESCRIBE
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    # HEAD changes on checkout, the branch ref on commit.
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --symbolic-full-name HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE _mppm_head_ref
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    foreach (_ref IN ITEMS HEAD ${_mppm_head_ref})
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --git-path ${_ref}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE _mppm_ref_path
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        get_filename_component(_mppm_ref_path "${_mppm_ref_path}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
        if (EXISTS "${_mppm_ref_path}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mppm_ref_path}")
        endif ()
    endforeach ()
endif ()
if (MPPM_GIT_DESCRIBE MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+.*)$")
    set(MPPM_VERSION_STRING "${CMAKE_MATCH_1}")
elseif (MPPM_GIT_DESCRIBE)
    set(MPPM_VERSION_STRING "${PROJECT_VERSION}-dev+${MPPM_GIT_DESCRIBE}")
else ()
    set(MPPM_VERSION_STRING "${PROJECT_VERSION}-dev")
endif ()
configure_file(port/launcher/version.h.in ${CMAKE_BINARY_DIR}/generated/mppm_version.h @ONLY)
if (WIN32)
    # Icon and version resource; a GUI program (SDL_main supplies WinMain), so no console window opens.
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _mppm_v "${MPPM_VERSION_STRING}")
    set(MPPM_RC_VERSION "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")
    file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/packaging/windows/moonsprout.ico" MPPM_RC_ICON)
    configure_file(packaging/windows/mppm.rc.in ${CMAKE_BINARY_DIR}/generated/mppm.rc @ONLY)
    target_sources(mppm PRIVATE ${CMAKE_BINARY_DIR}/generated/mppm.rc)
    set_target_properties(mppm PROPERTIES WIN32_EXECUTABLE ON)
    # Dawn's DLLs (and the DXC shader compiler it loads at run time) next to mppm.exe.
    include(extern/aurora/cmake/AuroraCopyRuntimeDLLs.cmake)
    aurora_copy_runtime_dlls(mppm)
endif ()

add_library(mppm_launcher STATIC ${LAUNCHER_SOURCES})
target_compile_features(mppm_launcher PUBLIC cxx_std_20)
# port/include: the C bridge headers the launcher implements (p2_host.h, p2_config.h, p2_wide.h).
target_include_directories(mppm_launcher
        PUBLIC ${CMAKE_SOURCE_DIR}/port/launcher/include
        PRIVATE ${CMAKE_SOURCE_DIR}/port/launcher ${CMAKE_SOURCE_DIR}/extern ${CMAKE_SOURCE_DIR}/port/include
        ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(mppm_launcher PRIVATE borealis::config borealis::io borealis::file_select
        borealis::disc borealis::update borealis::log aurora::core aurora::card aurora::gx aurora::pad aurora::vi fmt::fmt)
target_link_libraries(mppm PRIVATE mppm_launcher)
# Data-mod overlay (M5.2, port/mods/): mod.json is parsed with nlohmann_json (fetched by borealis),
# and the mods' "mppm" minimum version is compared with MPPM_VERSION_STRING.
if (NOT TARGET nlohmann_json::nlohmann_json)
    message(FATAL_ERROR "nlohmann_json::nlohmann_json is missing (borealis normally provides it)")
endif ()
target_link_libraries(mppm PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(mppm PRIVATE ${CMAKE_BINARY_DIR}/generated)
# Launcher resources (fonts, stylesheets, art) are loaded relative to the executable. One copy rule
# per file, so editing a stylesheet alone recopies it on the next build (no relink needed);
# CONFIGURE_DEPENDS picks up added or removed files.
file(GLOB_RECURSE LAUNCHER_RESOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/port/launcher/res/*)
get_target_property(MPPM_RUNTIME_DIR mppm RUNTIME_OUTPUT_DIRECTORY)
if (NOT MPPM_RUNTIME_DIR)
    set(MPPM_RUNTIME_DIR ${CMAKE_BINARY_DIR})
endif ()
set(LAUNCHER_RESOURCE_OUTPUTS)
foreach (res ${LAUNCHER_RESOURCES})
    file(RELATIVE_PATH rel ${CMAKE_SOURCE_DIR}/port/launcher/res ${res})
    set(out ${MPPM_RUNTIME_DIR}/res/${rel})
    add_custom_command(OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${res} ${out}
            DEPENDS ${res}
            COMMENT "Copying res/${rel}"
            VERBATIM)
    list(APPEND LAUNCHER_RESOURCE_OUTPUTS ${out})
endforeach ()
add_custom_target(mppm_resources DEPENDS ${LAUNCHER_RESOURCE_OUTPUTS})
add_dependencies(mppm mppm_resources)

# Data-mod self-test (M5.2): `ninja -C build-pc p2mods_selftest`, then `./build-pc/p2mods_selftest`.
# Only the pure parts (port/mods/pure/: no Aurora, SDL or p2_net) and tests/mods/. Not part of the
# default build.
file(GLOB P2MODS_SELFTEST_SOURCES CONFIGURE_DEPENDS port/mods/pure/*.cpp tests/mods/*.cpp)
add_executable(p2mods_selftest EXCLUDE_FROM_ALL ${P2MODS_SELFTEST_SOURCES})
target_compile_features(p2mods_selftest PRIVATE cxx_std_20)
target_include_directories(p2mods_selftest PRIVATE ${CMAKE_SOURCE_DIR}/port/include ${CMAKE_SOURCE_DIR}/port/mods)
target_link_libraries(p2mods_selftest PRIVATE nlohmann_json::nlohmann_json)

# Frame pacer self-test (fps_design 10, T1): `ninja -C build-pc frame_pacer_test`, then run it.
# Not part of the default build.
add_executable(frame_pacer_test EXCLUDE_FROM_ALL tests/frame_pacer_test.cpp)
target_compile_features(frame_pacer_test PRIVATE cxx_std_20)
target_include_directories(frame_pacer_test PRIVATE ${CMAKE_SOURCE_DIR}/port)
target_link_libraries(frame_pacer_test PRIVATE ${AURORA_SDL3_TARGET})

# ---- M5.3b online (docs/online/M5_3B_NETCODE.md): one CMake fragment per package, each owned by it ----
include(cmake/mppm_net_transport.cmake OPTIONAL) # transport package: ENet, libjuice, socket shim, transport tests
include(cmake/mppm_net_lobby.cmake OPTIONAL)     # server package: borealis::ws for the lobby client, its test
include(cmake/mppm_net_session.cmake OPTIONAL)   # lockstep package: session self-test, build-flags header
# Default lobby server for online.serverUrl; empty until the official server is deployed.
set(MPPM_DEFAULT_SERVER_URL "https://moonsprout-lobby.fly.dev" CACHE STRING "Default lobby server URL for online play (empty = none)")
target_compile_definitions(mppm_launcher PRIVATE "MPPM_DEFAULT_SERVER_URL=\"${MPPM_DEFAULT_SERVER_URL}\"")
