cmake_minimum_required(VERSION 3.20)
project(CTR-Native C)
include(CheckCCompilerFlag)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CTR_NATIVE_VERSION "0.1.0-beta.7.1")

if(NOT CMAKE_C_COMPILER_TARGET MATCHES "i.86" AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "i.86")
    set(CMAKE_C_FLAGS "-m32 ${CMAKE_C_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "-m32 ${CMAKE_EXE_LINKER_FLAGS}")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)

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

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msse -Wno-error -g")

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

# SDL3
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

# CTR Native
add_executable(ctr_native main.c)
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
target_link_libraries(ctr_native SDL3::SDL3)
target_link_options(ctr_native PRIVATE -static-libgcc)
if(WIN32)
    target_link_options(ctr_native PRIVATE -static)
endif()
