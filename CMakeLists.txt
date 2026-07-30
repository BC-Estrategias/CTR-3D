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
    list(APPEND CTR_NATIVE_SOURCES platform/n3ds/n3ds_audio_thread.c)
    list(APPEND CTR_NATIVE_SOURCES platform/n3ds/n3ds_render_thread.c)
endif()

add_executable(ctr_native ${CTR_NATIVE_SOURCES})

if(NINTENDO_3DS)
    # The backend TU talks to libctru, which was built with the ARM EABI's
    # default short enums. Nothing enum-typed crosses n3ds_backend.h -- it
    # exposes only plain scalars -- so this file can match libctru's ABI while
    # the game TU keeps the 4-byte enums its struct layouts require.
    set_source_files_properties(platform/n3ds/n3ds_backend.c platform/n3ds/n3ds_audio_thread.c platform/n3ds/n3ds_render_thread.c
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

# Per-frame OT-walk content counters ([OTPROF] log line every 64 rendered
# frames): prim counts by blend class, distinct state keys vs actual draws,
# packet-identical-to-last-frame share, plus a links-only dry walk timed as
# draw_otag_drywalk_ms (chase vs translate split). This is the decision
# instrument for the render campaign -- it prices the retained-geometry,
# z-buffer-reorder, and render-thread candidates before any of them is built.
# Counts are honest in profile builds; take draw_otag_parse_ms from a clean
# build (the hashing runs inside the walk and inflates it).
option(CTR_NATIVE_OTWALK_PROFILE
    "Per-frame OT-walk counters + dry-walk chase timing"
    OFF)
if(CTR_NATIVE_OTWALK_PROFILE)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_OTWALK_PROFILE=1)
endif()

# 3DS z-break substrate: enable the PICA depth test against the per-prim
# emit-ordinal carried in vertex z. Order-preserving by construction (equal
# ordinal + equal-passes test = painter), so ON must be pixel-identical to
# OFF -- that A/B is the acceptance. The ordinal stamping and vertex z
# plumbing are unconditional; this gate only turns the depth TEST on. It
# exists to make submission reorder (cross-bucket state-sorted batching)
# legal, which is where the fps payoff lives.
option(CTR_NATIVE_OT_DEPTH
    "3DS: depth-test against per-prim emit ordinals (z-break substrate)"
    ON)
if(CTR_NATIVE_OT_DEPTH)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_OT_DEPTH=1)
endif()

# First consumer of the depth substrate: defer the frame's draws, bin opaque
# (BM_NONE) content by GPU state across OT buckets, replay blends in order.
# Collapses the ~350 state transitions of a race frame's walk into ~a-draw-
# per-state. Requires CTR_NATIVE_OT_DEPTH (the depth test is what makes
# opaque reorder legal); runtime A/B via dumpreq 'drawmerge'. This is the
# gate that deliberately gives up the substrate's ON==OFF pixel identity --
# opaque submission ORDER changes, pixels should not, and draws/frame must.
option(CTR_NATIVE_DRAW_MERGE
    "3DS: cross-bucket state-sorted draw merge (needs CTR_NATIVE_OT_DEPTH)"
    ON)
if(CTR_NATIVE_DRAW_MERGE)
    if(NOT CTR_NATIVE_OT_DEPTH)
        message(FATAL_ERROR "CTR_NATIVE_DRAW_MERGE requires CTR_NATIVE_OT_DEPTH=ON")
    endif()
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_DRAW_MERGE=1)
endif()

# Track censuses for the native track renderer (s27) -- one [TRKCENSUS] log
# block per level load: vertex-pool edge sharing (the no-crack premise),
# water/animated-vertex exclusion share, AnimTex inventory, and texture
# working set per LOD variant. Read-only over game structs; golden unchanged.
# Design + what each number decides: docs/NATIVE_TRACK_RENDERER_DESIGN.md 7.
option(CTR_NATIVE_TRACK_CENSUS
    "[TRKCENSUS] level-geometry censuses for the native track renderer"
    OFF)
if(CTR_NATIVE_TRACK_CENSUS)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_TRACK_CENSUS=1)
endif()

# Native track renderer (s27+, docs/NATIVE_TRACK_RENDERER_DESIGN.md). Current
# state: ladder step 1 -- load-time resident-mesh builder, count-only ([TRKMESH]
# line per level), draws nothing on any platform yet. The 3DS draw side lands
# behind this same gate.
option(CTR_NATIVE_TRACK_RENDERER
    "Native track renderer: resident track meshes from level data (in progress)"
    OFF)
if(CTR_NATIVE_TRACK_RENDERER)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_TRACK_RENDERER=1)

    # On 3DS the whole depth compose (design 4.3) is the OT_DEPTH substrate --
    # track fragments and packets meet in one depth buffer only because that
    # gate legalizes comparing a real depth test against emit-ordinal z -- and
    # rung 3a issues its draws at the DRAW_MERGE barrier (N3DS_FlushRun's
    # caller). Neither dependency is optional the way CTR_NATIVE_DIRECT_EMIT's
    # is; catch a mismatched config at configure time rather than a silent
    # black track or a merge-barrier that never runs on hardware. PC is
    # count-only at this ladder step (no drawing at all), so it carries no
    # such requirement.
    if(NINTENDO_3DS)
        if(NOT CTR_NATIVE_OT_DEPTH)
            message(FATAL_ERROR "CTR_NATIVE_TRACK_RENDERER requires CTR_NATIVE_OT_DEPTH=ON on 3DS (the depth compose is the OT_DEPTH substrate)")
        endif()
        if(NOT CTR_NATIVE_DRAW_MERGE)
            message(FATAL_ERROR "CTR_NATIVE_TRACK_RENDERER requires CTR_NATIVE_DRAW_MERGE=ON on 3DS (draws are issued at the merge-bin barrier)")
        endif()
    endif()
endif()

# Render worker thread on core 2 (New3DS-only core; core 1 is the audio
# worker's). DrawOTag becomes the async post the game's PS1 GPU protocol
# already expects (bool_DrawOTag_InProgress / DrawSyncCallback / double-
# buffered OT+primMem), so the OT walk, the C3D frame bracket's GPU wait, and
# frame end/swap all leave the game thread; everything else the facade exposes
# posts synchronously in issue order. Behavior-preserving by construction
# (same jobs, same order, one frame in flight, gated by the game's own
# DrawSync fence) -- ON is the default and the A/B lives at runtime: dumpreq
# 'renderthread'. Falls back to today's synchronous path on old 3DS or core-2
# refusal. (GTE-capture builds ran sync until s26 double-buffered the capture
# table -- native_gte_capture.c -- so capture and the worker now coexist.)
# PC builds compile none of this. Design: docs/RENDER_THREAD_DESIGN.md.
option(CTR_NATIVE_RENDER_THREAD
    "3DS: render pipeline on a core-2 worker thread (runtime A/B: dumpreq renderthread)"
    ON)
if(NINTENDO_3DS AND CTR_NATIVE_RENDER_THREAD)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_RENDER_THREAD=1)
endif()

# Direct-emit fast path (retained-geometry campaign, transitional form --
# docs/RETAINED_GEOMETRY_V2_DESIGN.md, session 26). OT nodes whose payload is
# purely opaque poly prims go packet -> merge bin on the 3DS walk directly,
# skipping the GrVertex intermediate, the split machinery, and the scratch
# copies -- the translate/convert half the core-2 profile showed as the
# render worker's tax. Requires DRAW_MERGE (bins) + OT_DEPTH (order freedom);
# byte-equivalent bins by construction, runtime A/B: SELECT+D-LEFT on
# hardware, dumpreq 'directemit' in Azahar. On PC builds the gate compiles a
# count-only scanner ([DEMIT] window line = the eligibility ceiling; runtime
# default 0 on PC, flip g_dbg_directEmit in source to measure) and never
# changes PC output. Hardware-accepted 2026-07-28 (owner A/B, Labs arcade:
# visually identical, ~1.7ms median frame win -- the deleted GrVertex/split
# machinery, NOT the per-triangle emit work, which just moved buckets:
# draw_all_splits 42.1->27.6ms but draw_otag_parse 2.7->15.5ms) and promoted
# to default ON the same night. The per-triangle work itself is the full
# retained form's target (docs/RETAINED_GEOMETRY_V2_DESIGN.md).
option(CTR_NATIVE_DIRECT_EMIT
    "Direct packet->bin emit for all-opaque-poly OT nodes (3DS fast path; PC counts only)"
    ON)
if(CTR_NATIVE_DIRECT_EMIT)
    if(NINTENDO_3DS AND NOT CTR_NATIVE_DRAW_MERGE)
        message(FATAL_ERROR "CTR_NATIVE_DIRECT_EMIT requires CTR_NATIVE_DRAW_MERGE on 3DS (it emits into the merge bins)")
    endif()
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_DIRECT_EMIT=1)
endif()

# Log every growth/reset of the decoded-voice PCM arena in the audio mixer.
option(CTR_NATIVE_AUDIO_ARENA_LOG
    "Log decoded-voice PCM arena growth and resets"
    OFF)
if(CTR_NATIVE_AUDIO_ARENA_LOG)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_AUDIO_ARENA_LOG=1)
endif()

# Per-window voice-start decode counters + [PERF-VOICE] report line + the
# worst-frame/key-on correlation (docs/VOICE_START_DECODE_DESIGN.md step 1).
# The 64-frame [PERF] averages smear a one-frame key-on decode spike into
# nothing, which is how the sound-start hitches hid; this is the
# instrumentation that has to confirm the cause before optimizing further.
option(CTR_NATIVE_AUDIO_DECODE_PROFILE
    "Per-window voice-start decode counters"
    OFF)
if(CTR_NATIVE_AUDIO_DECODE_PROFILE)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_AUDIO_DECODE_PROFILE=1)
endif()

# Per-role disc-image read counters + two [DISCIO] report lines (XA streaming
# vs everything else; on 3DS those are also the two threads). Every disc read
# is one seek plus one read, and on 3DS that pair is an SD round trip -- but
# nothing counted them, so "the SD pump blocks the worker for seconds" had no
# way to separate raw read cost from time spent queued behind the other
# thread. The run-shape fields (runs1, maxRun) and the per-call-site tallies
# (dir/cd/xaCnt/sng) are what say WHICH caller is reading a sector at a time;
# a mean sectors-per-call cannot, and reading one off a mean sent this
# session's first two fix attempts at the wrong callers. Counts from profile
# builds, timings from clean ones, same rule as CTR_NATIVE_OTWALK_PROFILE.
option(CTR_NATIVE_DISC_IO_PROFILE
    "Per-thread disc-image read counters ([DISCIO] window lines)"
    OFF)
if(CTR_NATIVE_DISC_IO_PROFILE)
    target_compile_definitions(ctr_native PRIVATE CTR_NATIVE_DISC_IO_PROFILE=1)
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

    # Native track renderer (rung 3a): sibling shader for the resident track
    # draw path, same picasso -> bin2s -> embedded .S/.h pipeline as ctr.v.pica
    # above. Guarded by CTR_NATIVE_TRACK_RENDERER -- step 1 only ships a
    # minimal placeholder .pica (see the file's own header) that assembles and
    # writes every declared output, so a plain 3DS build with the gate OFF
    # never even looks for it and a build with the gate ON doesn't break
    # before step 2 (a later task) writes the real shader body.
    if(CTR_NATIVE_TRACK_RENDERER)
        set(CTR_N3DS_TRACK_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds/shaders/ctr_track.v.pica")
        set(CTR_N3DS_TRACK_SHADER_SHBIN "${CTR_N3DS_SHADER_GEN}/ctr_track.shbin")

        add_custom_command(
            OUTPUT "${CTR_N3DS_TRACK_SHADER_SHBIN}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CTR_N3DS_SHADER_GEN}"
            COMMAND ${CTR_PICASSO_EXE} -o "${CTR_N3DS_TRACK_SHADER_SHBIN}" "${CTR_N3DS_TRACK_SHADER_SRC}"
            DEPENDS "${CTR_N3DS_TRACK_SHADER_SRC}"
            COMMENT "picasso: ctr_track.v.pica -> ctr_track.shbin"
        )
        add_custom_command(
            OUTPUT "${CTR_N3DS_SHADER_GEN}/ctr_track_shbin.S" "${CTR_N3DS_SHADER_GEN}/ctr_track_shbin.h"
            COMMAND ${DKP_BIN2S} -a ${DKP_BIN2S_ALIGNMENT} -H "${CTR_N3DS_SHADER_GEN}/ctr_track_shbin.h"
                    "${CTR_N3DS_TRACK_SHADER_SHBIN}" > "${CTR_N3DS_SHADER_GEN}/ctr_track_shbin.S"
            DEPENDS "${CTR_N3DS_TRACK_SHADER_SHBIN}"
            COMMENT "bin2s: embedding ctr_track.shbin"
        )
        # ASM language + the ctr_shbin.S source-file-properties override above
        # already ran unconditionally for the first shader in this same
        # if(NINTENDO_3DS) scope, so both apply here too; only the new output
        # needs its own PROPERTIES call.
        set_source_files_properties("${CTR_N3DS_SHADER_GEN}/ctr_track_shbin.S"
            PROPERTIES LANGUAGE ASM)

        target_sources(ctr_native PRIVATE "${CTR_N3DS_SHADER_GEN}/ctr_track_shbin.S")
    endif()

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
