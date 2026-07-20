cmake_minimum_required(VERSION 3.20)
project(CTR-Native C)
include(CheckCCompilerFlag)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CTR_NATIVE_VERSION "0.1.0-beta.7.1")

# Host builds target x86-32 with SSE. On the 3DS the devkitPro platform module
# supplies the ARM11 arch flags (-march=armv6k -mtune=mpcore -mfloat-abi=hard),
# and the find-root overrides below must not fire: devkitARM relies on the find
# root to locate libctru/citro3d under $DEVKITPRO.
if(NOT NINTENDO_3DS)
    if(NOT CMAKE_C_COMPILER_TARGET MATCHES "i.86" AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "i.86")
        set(CMAKE_C_FLAGS "-m32 ${CMAKE_C_FLAGS}")
        set(CMAKE_EXE_LINKER_FLAGS "-m32 ${CMAKE_EXE_LINKER_FLAGS}")
    endif()

    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
endif()

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE CTR_NATIVE_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT CTR_NATIVE_GIT_HASH)
    set(CTR_NATIVE_GIT_HASH "unknown")
endif()

execute_process(
    COMMAND git diff --quiet --exit-code
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE CTR_NATIVE_GIT_DIRTY
    ERROR_QUIET
)
if(CTR_NATIVE_GIT_DIRTY EQUAL 0)
    set(CTR_NATIVE_BUILD_ID "${CTR_NATIVE_GIT_HASH}")
else()
    string(TIMESTAMP CTR_NATIVE_BUILD_TIMESTAMP "%Y%m%d%H%M%S" UTC)
    set(CTR_NATIVE_BUILD_ID "${CTR_NATIVE_GIT_HASH}-dirty-${CTR_NATIVE_BUILD_TIMESTAMP}")
endif()

if(NINTENDO_3DS)
    # The ARM EABI defaults to -fshort-enums, sizing each enum to the smallest
    # type that holds it. The decomp's layouts assume 4-byte enums and assert on
    # it (CTR_STATIC_ASSERT(sizeof(enum X) == 0x4)), and those enums sit inside
    # structs parsed from on-disc game data, so a narrower enum silently shifts
    # field offsets. Force the wide form.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-short-enums -Wno-error -g")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msse -Wno-error -g")
endif()

function(ctr_native_add_label_warning_if_supported target warning flag_check)
    set(saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror=unknown-warning-option")
    endif()

    check_c_compiler_flag("${warning}" "${flag_check}")
    set(CMAKE_REQUIRED_FLAGS "${saved_required_flags}")

    if(${flag_check})
        target_compile_options(${target} PRIVATE "${warning}")
    endif()
endfunction()

# SDL3 -- host only. The 3DS backend targets libctru/citro3d directly.
if(NOT NINTENDO_3DS)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_OFFSCREEN OFF CACHE BOOL "" FORCE)
set(SDL_SNDIO OFF CACHE BOOL "" FORCE)
set(SDL_SNDIO_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(externals/SDL)
endif()

# CTR Native
#
# main.c is a unity build: it #includes every other .c directly. The 3DS backend
# is the one exception -- it must be its own translation unit so libctru's
# headers never meet the decomp's (see platform/n3ds/n3ds_backend.h).
set(CTR_NATIVE_SOURCES main.c)
if(NINTENDO_3DS)
    list(APPEND CTR_NATIVE_SOURCES platform/n3ds/n3ds_backend.c)
endif()

add_executable(ctr_native ${CTR_NATIVE_SOURCES})

if(NINTENDO_3DS)
    # The backend TU talks to libctru, which was built with the ARM EABI's
    # default short enums. Nothing enum-typed crosses n3ds_backend.h -- it
    # exposes only plain scalars -- so this file can match libctru's ABI while
    # the game TU keeps the 4-byte enums its struct layouts require.
    set_source_files_properties(platform/n3ds/n3ds_backend.c
        PROPERTIES COMPILE_OPTIONS "-fshort-enums")
endif()
target_include_directories(ctr_native PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(ctr_native PRIVATE
    CTR_NATIVE
    BUILD=926
    CTR_INTERNAL
    CTR_NATIVE_VERSION=\"${CTR_NATIVE_VERSION}\"
    CTR_NATIVE_BUILD_ID=\"${CTR_NATIVE_BUILD_ID}\"
)
# Optional native enhancements. Retail behavior is the default; each one is a
# single switch away, e.g. cmake -DCTR_NATIVE_VIS_OFFPATH_FALLBACK=ON ...
# The source-level defaults live next to the code they gate, so leaving these
# OFF reproduces retail exactly.
#   0 = retail (baked PVS decides everything)
#   1 = all-visible only when the camera has no PVS source at all
#   2 = always draw all track geometry, ignoring the baked PVS
set(CTR_NATIVE_VIS_OFFPATH_FALLBACK "0" CACHE STRING
    "Off-path visibility mode: 0 retail, 1 fallback when no PVS source, 2 always all-visible")
set_property(CACHE CTR_NATIVE_VIS_OFFPATH_FALLBACK PROPERTY STRINGS 0 1 2)

# Accept the boolean spelling too, so -D...=ON still means mode 1 rather than
# silently expanding to a non-numeric token that #if would treat as 0.
if(CTR_NATIVE_VIS_OFFPATH_FALLBACK STREQUAL "ON")
    set(CTR_NATIVE_VIS_OFFPATH_FALLBACK "1")
elseif(CTR_NATIVE_VIS_OFFPATH_FALLBACK STREQUAL "OFF")
    set(CTR_NATIVE_VIS_OFFPATH_FALLBACK "0")
endif()

if(NOT CTR_NATIVE_VIS_OFFPATH_FALLBACK MATCHES "^[0-2]$")
    message(FATAL_ERROR "CTR_NATIVE_VIS_OFFPATH_FALLBACK must be 0, 1 or 2 (got '${CTR_NATIVE_VIS_OFFPATH_FALLBACK}')")
endif()

if(NOT CTR_NATIVE_VIS_OFFPATH_FALLBACK STREQUAL "0")
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_VIS_OFFPATH_FALLBACK=${CTR_NATIVE_VIS_OFFPATH_FALLBACK})
endif()

# Log how close the per-frame primitive/OT buffers come to their per-level
# budget. Overflow is otherwise silent and mimics the vanishing-geometry bug.
option(CTR_NATIVE_PRIMMEM_WATERMARK
    "Log peak PrimMem/OTMem usage per frame and warn on guard-band overflow"
    OFF)
if(CTR_NATIVE_PRIMMEM_WATERMARK)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_PRIMMEM_WATERMARK=1)
endif()

# 3DS only, and temporary: skip generating SPU voice samples while ndsp is not
# wired up and nothing is audible anyway. Worth 48-81% of frame time. Voice
# lifecycle still runs, so the game sequences itself normally -- see the comment
# on CTR_3DS_SKIP_SPU_MIX in platform/native_audio.c. Turn OFF to measure mixer
# optimisations on real 3DS timings. Delete this once ndsp exists.
option(CTR_3DS_SKIP_SPU_MIX
    "3DS: skip SPU voice sample generation while there is no audio output"
    ON)
if(NINTENDO_3DS AND NOT CTR_3DS_SKIP_SPU_MIX)
    target_compile_definitions(ctr_native PRIVATE CTR_3DS_SKIP_SPU_MIX=0)
endif()

# Log every growth/reset of the decoded-voice PCM arena in the audio mixer.
option(CTR_NATIVE_AUDIO_ARENA_LOG
    "Log decoded-voice PCM arena growth and resets"
    OFF)
if(CTR_NATIVE_AUDIO_ARENA_LOG)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_AUDIO_ARENA_LOG=1)
endif()

# Reset that arena on the LOAD_RESTART fast path (Time Trial "Retry"), which
# reloads no audio banks and so never hits the only unconditional reset.
option(CTR_NATIVE_AUDIO_ARENA_RESET_ON_RESTART
    "Drop decoded voice PCM on race restart (fixes unbounded arena growth across retries)"
    OFF)
if(CTR_NATIVE_AUDIO_ARENA_RESET_ON_RESTART)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_AUDIO_ARENA_RESET_ON_RESTART=1)
endif()

# Populate + clear a per-frame side table of GTE sub-pixel fractions, keyed by
# the exact truncated screen coordinate the GTE writes to the packet. No
# visible effect alone -- see docs/GTE_CAPTURE_DESIGN.md. Shared
# infrastructure for CTR_NATIVE_VERTEX_DESNAP and later stereo/perspective-UV.
option(CTR_NATIVE_GTE_CAPTURE
    "Populate the GTE sub-pixel capture side table (infrastructure only, no visible effect)"
    OFF)

# Consume the capture table in the vertex builders and vertex shader to draw
# geometry at its true sub-pixel position instead of PS1-style whole-pixel
# rounding. Game logic reads unchanged truncated integers either way.
option(CTR_NATIVE_VERTEX_DESNAP
    "Draw geometry at true GTE sub-pixel position instead of snapping to whole pixels"
    OFF)

if(CTR_NATIVE_VERTEX_DESNAP AND NOT CTR_NATIVE_GTE_CAPTURE)
    message(STATUS "CTR_NATIVE_VERTEX_DESNAP implies CTR_NATIVE_GTE_CAPTURE; enabling it")
    set(CTR_NATIVE_GTE_CAPTURE ON CACHE BOOL "Populate the GTE sub-pixel capture side table" FORCE)
endif()

if(CTR_NATIVE_GTE_CAPTURE)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_GTE_CAPTURE=1)
endif()
if(CTR_NATIVE_VERTEX_DESNAP)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_VERTEX_DESNAP=1)
endif()

target_compile_options(ctr_native PRIVATE -O2)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ctr_native PRIVATE -Wenum-conversion -Wenum-compare -Wswitch-enum -Wformat -Wparentheses)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(ctr_native PRIVATE -Wall -Wextra -Wstrict-aliasing=2)
    ctr_native_add_label_warning_if_supported(ctr_native -Wfree-labels CTR_HAS_WFREE_LABELS)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
    ctr_native_add_label_warning_if_supported(ctr_native -Wc23-extensions CTR_HAS_WC23_EXTENSIONS)
endif()
if(NINTENDO_3DS)
    # PICA200 vertex shader: platform/n3ds/shaders/ctr.v.pica -> ctr.shbin
    # (picasso) -> embedded ctr_shbin.S/.h (bin2s), so native_renderer_3ds.c
    # can #include the compiled shader like the citro3d examples do. Hand-
    # rolled as two build-time custom commands rather than
    # dkp_add_embedded_binary_library: that helper's dkp_resolve_file requires
    # its input to already exist on disk at *configure* time, which a file
    # this build generates can't satisfy on a clean tree, and would force a
    # reconfigure (not just a rebuild) on every shader edit.
    set(CTR_N3DS_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds/shaders/ctr.v.pica")
    set(CTR_N3DS_SHADER_GEN "${CMAKE_CURRENT_BINARY_DIR}/n3ds-shaders")
    set(CTR_N3DS_SHADER_SHBIN "${CTR_N3DS_SHADER_GEN}/ctr.shbin")

    add_custom_command(
        OUTPUT "${CTR_N3DS_SHADER_SHBIN}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CTR_N3DS_SHADER_GEN}"
        COMMAND ${CTR_PICASSO_EXE} -o "${CTR_N3DS_SHADER_SHBIN}" "${CTR_N3DS_SHADER_SRC}"
        DEPENDS "${CTR_N3DS_SHADER_SRC}"
        COMMENT "picasso: ctr.v.pica -> ctr.shbin"
    )
    add_custom_command(
        OUTPUT "${CTR_N3DS_SHADER_GEN}/ctr_shbin.S" "${CTR_N3DS_SHADER_GEN}/ctr_shbin.h"
        COMMAND ${DKP_BIN2S} -a ${DKP_BIN2S_ALIGNMENT} -H "${CTR_N3DS_SHADER_GEN}/ctr_shbin.h"
                "${CTR_N3DS_SHADER_SHBIN}" > "${CTR_N3DS_SHADER_GEN}/ctr_shbin.S"
        DEPENDS "${CTR_N3DS_SHADER_SHBIN}"
        COMMENT "bin2s: embedding ctr.shbin"
    )
    # Tried tagging this LANGUAGE C on the theory that devkitARM's gcc
    # dispatches .S by file extension regardless -- it doesn't: CMake's C
    # compile rule forces the C frontend (-x c) rather than letting gcc's own
    # extension sniffing run, so cc1 tried to parse ".section ..." as C and
    # failed. Enabling the real ASM language is what dkp_add_embedded_binary_library
    # actually relies on; do the same here instead of the "C" fallback.
    enable_language(ASM)
    set_source_files_properties("${CTR_N3DS_SHADER_GEN}/ctr_shbin.S"
        PROPERTIES LANGUAGE ASM)

    target_sources(ctr_native PRIVATE "${CTR_N3DS_SHADER_GEN}/ctr_shbin.S")
    target_include_directories(ctr_native PRIVATE "${CTR_N3DS_SHADER_GEN}")

    # citro3d before ctru: the linker resolves left to right and citro3d pulls
    # symbols out of ctru.
    target_link_libraries(ctr_native citro3d ctru)
    ctr_generate_smdh(ctr_native.smdh
        NAME "CTR-Native"
        DESCRIPTION "Crash Team Racing (native port) ${CTR_NATIVE_VERSION}"
        AUTHOR "CTR-tools"
    )
    ctr_create_3dsx(ctr_native SMDH ctr_native.smdh)
else()
    target_link_libraries(ctr_native SDL3::SDL3)
    target_link_options(ctr_native PRIVATE -static-libgcc)
    if(WIN32)
        target_link_options(ctr_native PRIVATE -static)
    endif()
endif()
