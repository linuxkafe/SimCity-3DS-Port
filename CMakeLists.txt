# SimCity DS/3DS Port — root CMakeLists.txt
#
# Builds the static-recomp core + DS/3DS frontend into a .nds / .3dsx cartridge.
# Toolchain: devkitpro/devkitarm (Docker) or local arm-none-eabi-gcc.

cmake_minimum_required(VERSION 3.20)
project(SimCityDS LANGUAGES C CXX ASM)

# ── Platform selection ───────────────────────────────────────────────
# Default: DS (for backwards compatibility)
set(PLATFORM "DS" CACHE STRING "Target platform: DS or 3DS")
if(NOT (PLATFORM STREQUAL "DS" OR PLATFORM STREQUAL "3DS"))
    message(FATAL_ERROR "PLATFORM must be DS or 3DS")
endif()
message(STATUS "Target platform: ${PLATFORM}")

# ── Toolchain detection ──────────────────────────────────────────────
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(PLATFORM STREQUAL "DS")
        # Common devkitARM locations for DS
        set(_dkp_candidates
            "/opt/devkitpro/devkitARM"
            "$ENV{DEVKITARM}"
            "/opt/devkitpro"
            "/opt/devkitpro/cmake"
        )
        foreach(_cand ${_dkp_candidates})
            if(EXISTS "${_cand}/cmake/NDS.cmake")
                set(CMAKE_TOOLCHAIN_FILE "${_cand}/cmake/NDS.cmake")
                break()
            endif()
        endforeach()
        if(NOT CMAKE_TOOLCHAIN_FILE AND EXISTS "/opt/devkitpro/cmake/NDS.cmake")
            set(CMAKE_TOOLCHAIN_FILE "/opt/devkitpro/cmake/NDS.cmake")
        endif()
    else()
        # 3DS toolchain - use devkitARM.cmake which includes the platform
        set(_dkp_candidates
            "/opt/devkitpro/devkitARM"
            "$ENV{DEVKITARM}"
            "/opt/devkitpro"
            "/opt/devkitpro/cmake"
        )
        foreach(_cand ${_dkp_candidates})
            if(EXISTS "${_cand}/cmake/devkitARM.cmake")
                set(CMAKE_TOOLCHAIN_FILE "${_cand}/cmake/devkitARM.cmake")
                break()
            endif()
        endforeach()
        if(NOT CMAKE_TOOLCHAIN_FILE AND EXISTS "/opt/devkitpro/cmake/devkitARM.cmake")
            set(CMAKE_TOOLCHAIN_FILE "/opt/devkitpro/cmake/devkitARM.cmake")
        endif()
    endif()
endif()

if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
        "DevkitARM toolchain not found for ${PLATFORM}.\n"
        "  Docker:  docker run --rm -v '$(PWD):/work' -w /work "
        "devkitpro/devkitarm bash -c 'make PLATFORM=${PLATFORM} docker-build'")
endif()
message(STATUS "Using toolchain: ${CMAKE_TOOLCHAIN_FILE}")

if(NOT CMAKE_C_COMPILER)
    message(FATAL_ERROR "C compiler not set. Did the toolchain file configure it?")
endif()
message(STATUS "C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "CXX compiler: ${CMAKE_CXX_COMPILER}")

# ── Build config ─────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE not set; defaulting to Release")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Upstream source path ─────────────────────────────────────────────
set(UPSTREAM_SRC_DIR "" CACHE PATH "Path to upstream static-recomp source")
if(NOT UPSTREAM_SRC_DIR)
    # Default: sibling directory to this project (relative path)
    get_filename_component(_this_dir "${CMAKE_SOURCE_DIR}" REALPATH)
    get_filename_component(_parent_dir "${_this_dir}/.." REALPATH)
    set(UPSTREAM_SRC_DIR "${_parent_dir}/SimCity-SNES-Static-Recomp/static-recomp"
        CACHE PATH "Upstream static-recomp source directory" FORCE)
endif()

if(NOT EXISTS "${UPSTREAM_SRC_DIR}/src/simcity_static_recomp.c")
    message(FATAL_ERROR
        "Upstream source not found at: ${UPSTREAM_SRC_DIR}\n"
        "  Set UPSTREAM_SRC_DIR explicitly:\n"
        "    cmake -DUPSTREAM_SRC_DIR=/path/to/SimCity-SNES-Static-Recomp/static-recomp .")
endif()
message(STATUS "Upstream source: ${UPSTREAM_SRC_DIR}")

# ── Portability patches ───────────────────────────────────────────────
# Upstream is treated as read-only and must stay byte-identical: the generated
# dispatch shards (v11/v34/v35/v36) are ROM-certified. Portability fixes that
# are required only by the target platform live in patches/*.patch and are
# applied to a build-local copy of the core, never to the upstream tree.
#
# Each patch is a normal `patch -p1` diff against the upstream layout, applied
# in filename order. A patch that does not apply is a hard error: silently
# building an unpatched core on the 3DS would reintroduce the bug it fixes.
set(PORT_CORE_DIR "${CMAKE_BINARY_DIR}/port-core")
file(REMOVE_RECURSE "${PORT_CORE_DIR}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${UPSTREAM_SRC_DIR}" "${PORT_CORE_DIR}"
    RESULT_VARIABLE _copy_result
)
if(NOT _copy_result EQUAL 0)
    message(FATAL_ERROR "Failed to materialise core into ${PORT_CORE_DIR}")
endif()

file(GLOB PORT_PATCHES "${CMAKE_SOURCE_DIR}/patches/*.patch")
list(SORT PORT_PATCHES)
foreach(_patch IN LISTS PORT_PATCHES)
    get_filename_component(_patch_name "${_patch}" NAME)
    execute_process(
        COMMAND patch -p1 -d "${PORT_CORE_DIR}" -i "${_patch}" --forward
        RESULT_VARIABLE _patch_result
        OUTPUT_VARIABLE _patch_out
        ERROR_VARIABLE _patch_err
    )
    if(NOT _patch_result EQUAL 0)
        message(FATAL_ERROR
            "Portability patch failed to apply: ${_patch_name}\n"
            "${_patch_out}${_patch_err}\n"
            "Refusing to build an unpatched core. Keep upstream unchanged; "
            "fix the patch instead.")
    endif()
    message(STATUS "Applied portability patch: ${_patch_name}")
endforeach()

# Build against the patched core, not the raw upstream tree.
set(UPSTREAM_SRC_DIR "${PORT_CORE_DIR}")
message(STATUS "Building core from: ${UPSTREAM_SRC_DIR}")

# ── Source lists ──────────────────────────────────────────────────────
file(GLOB_RECURSE CORE_C_SOURCES
    "${UPSTREAM_SRC_DIR}/src/*.c"
)

file(GLOB_RECURSE GENERATED_SHARD_SOURCES_V11
    "${UPSTREAM_SRC_DIR}/generated/v11-shards/*.c"
)
file(GLOB_RECURSE GENERATED_SHARD_SOURCES_V34
    "${UPSTREAM_SRC_DIR}/generated/v34-shards/*.c"
)
file(GLOB_RECURSE GENERATED_SHARD_SOURCES_V35
    "${UPSTREAM_SRC_DIR}/generated/v35-shards/*.c"
)

file(GLOB_RECURSE GENERATED_CORE_SOURCES
    "${UPSTREAM_SRC_DIR}/generated/*.c"
    "${UPSTREAM_SRC_DIR}/generated/audio/*.c"
)

file(GLOB_RECURSE AUDIO_CPP_SOURCES
    "${UPSTREAM_SRC_DIR}/static-audio/simcity-project-dsp/*.cpp"
    "${UPSTREAM_SRC_DIR}/static-audio/snes9x-bapu-aot/sc_static_apu.cpp"
    "${UPSTREAM_SRC_DIR}/static-audio/snes9x-bapu-aot/smp/smp.cpp"
    "${UPSTREAM_SRC_DIR}/static-audio/snes9x-bapu-aot/dsp/sdsp.cpp"
    "${UPSTREAM_SRC_DIR}/static-audio/simcity-project-dsp/*.c"
)

# ── Core library: simcity-static-recomp ───────────────────────────────
add_library(simcity-static-recomp STATIC
    ${CORE_C_SOURCES}
    ${GENERATED_SHARD_SOURCES_V11}
    ${GENERATED_SHARD_SOURCES_V34}
    ${GENERATED_SHARD_SOURCES_V35}
    ${GENERATED_CORE_SOURCES}
    ${AUDIO_CPP_SOURCES}
)

target_include_directories(simcity-static-recomp PUBLIC
    "${UPSTREAM_SRC_DIR}/include"
)
target_include_directories(simcity-static-recomp PRIVATE
    "${UPSTREAM_SRC_DIR}/internal/include"
    "${UPSTREAM_SRC_DIR}/generated"
    "${UPSTREAM_SRC_DIR}/static-audio/snes9x-bapu-aot"
    "${UPSTREAM_SRC_DIR}/static-audio/snes9x-bapu-aot/smp"
    "${UPSTREAM_SRC_DIR}/static-audio/snes9x-bapu-aot/dsp"
    "${UPSTREAM_SRC_DIR}/static-audio/simcity-project-dsp/include"
)

# ARM9/ARM11-specific flags
if(PLATFORM STREQUAL "DS")
    set(ARM_C_FLAGS
        -mcpu=arm946e-s -mthumb
        -O2
        -Wall -Wextra -Wpedantic
        -fno-strict-aliasing
    )
    set(ARM_CXX_FLAGS
        -mcpu=arm946e-s -mthumb -O2
        -fno-exceptions -fno-rtti
    )
else()
    # 3DS ARM11 (ARMv6K)
    set(ARM_C_FLAGS
        -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
        -O2
        -Wall -Wextra -Wpedantic
        -fno-strict-aliasing
        -D__3DS__ -D_3DS
    )
    set(ARM_CXX_FLAGS
        -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2
        -fno-exceptions -fno-rtti
        -D__3DS__ -D_3DS
    )
endif()

target_compile_options(simcity-static-recomp PRIVATE
    ${ARM_C_FLAGS}
    ${ARM_CXX_FLAGS}
)

target_compile_definitions(simcity-static-recomp PRIVATE
    SC_SMP_AOT
    SC_STATIC_SDSP_PRIMITIVES
)

# Suppress warnings in generated code (large scale, many false positives)
set_source_files_properties(
    ${GENERATED_SHARD_SOURCES_V11}
    ${GENERATED_SHARD_SOURCES_V34}
    ${GENERATED_SHARD_SOURCES_V35}
    ${GENERATED_CORE_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w;-O2"
)

# ── Platform-specific frontend ───────────────────────────────────────
if(PLATFORM STREQUAL "DS")
    # DS Frontend
    add_executable(simcity-ds
        frontend/ds/ds_main.c
        frontend/ds/ds_input.c
        frontend/ds/ds_render.c
        frontend/ds/ds_audio.c
    )

    target_include_directories(simcity-ds PRIVATE
        "${CMAKE_SOURCE_DIR}/frontend/ds"
        "${UPSTREAM_SRC_DIR}/include"
        "${UPSTREAM_SRC_DIR}/internal/include"
        "${UPSTREAM_SRC_DIR}/generated"
    )

    target_link_libraries(simcity-ds PRIVATE
        simcity-static-recomp
        nds9          # libnds for ARM9
    )

    target_compile_options(simcity-ds PRIVATE
        ${ARM_C_FLAGS}
    )

    target_compile_definitions(simcity-ds PRIVATE
        SIMCITY_PLATFORM_DS
        DS_TOP_SCREEN_WIDTH=256
        DS_TOP_SCREEN_HEIGHT=192
        DS_BOTTOM_SCREEN_WIDTH=256
        DS_BOTTOM_SCREEN_HEIGHT=192
    )

    set_target_properties(simcity-ds PROPERTIES
        OUTPUT_NAME "simcity"
        SUFFIX ".nds"
    )

    # Headless test binary (no libnds dependency)
    add_executable(simcity-headless-test
        frontend/ds/headless_test.c
    )
    target_include_directories(simcity-headless-test PRIVATE
        "${UPSTREAM_SRC_DIR}/include"
        "${UPSTREAM_SRC_DIR}/internal/include"
        "${UPSTREAM_SRC_DIR}/generated"
    )
    target_link_libraries(simcity-headless-test PRIVATE
        simcity-static-recomp
    )
    target_compile_options(simcity-headless-test PRIVATE
        ${ARM_C_FLAGS}
    )
    target_compile_definitions(simcity-headless-test PRIVATE
        SIMCITY_PLATFORM_DS
    )

else()
    # 3DS Frontend
    add_executable(simcity-3ds
        frontend/3ds/3ds_main.c
        frontend/3ds/3ds_input.c
        frontend/3ds/3ds_render.c
        frontend/3ds/3ds_audio.c
        frontend/3ds/3dsdbg.c
        frontend/3ds/ds3ftp.c
    )

    target_include_directories(simcity-3ds PRIVATE
        "${CMAKE_SOURCE_DIR}/frontend/3ds"
        "${UPSTREAM_SRC_DIR}/include"
        "${UPSTREAM_SRC_DIR}/internal/include"
        "${UPSTREAM_SRC_DIR}/generated"
        # libctru includes
        "$ENV{DEVKITPRO}/libctru/include"
    )
target_link_libraries(simcity-3ds PRIVATE
        simcity-static-recomp
        ${DEVKITPRO}/libctru/lib/libcitro2d.a
        ${DEVKITPRO}/libctru/lib/libcitro3d.a
        ${DEVKITPRO}/libctru/lib/libctru.a
        ${DEVKITPRO}/devkitARM/lib/gcc/arm-none-eabi/16.1.0/armv6k/fpu/libgcc.a
        ${DEVKITPRO}/devkitARM/arm-none-eabi/lib/armv6k/fpu/libc.a
        ${DEVKITPRO}/devkitARM/arm-none-eabi/lib/armv6k/fpu/libm.a
    )

    # NOTE: -specs=3dsx.specs is injected by the devkitPro toolchain itself
    # (/opt/devkitpro/cmake/Platform/Nintendo3DS.cmake:16). Adding it again here
    # made 3dsx.specs `%include <sync-dmb.specs>` twice, and gcc aborts with
    # "attempt to rename spec 'link' to already defined spec 'sync_sync_link'".
    # Do not re-add it. The remaining flags duplicate 3dsx.specs' own *link line
    # and are kept only because they are harmless and were proven to link before.
    target_link_options(simcity-3ds PRIVATE
        -B/opt/devkitpro/devkitARM/arm-none-eabi/lib/armv6k/fpu/
        -mfloat-abi=hard
        -Wl,-d -Wl,--emit-relocs -Wl,--use-blx -Wl,--gc-sections -Wl,--no-warn-mismatch
    )

    target_compile_options(simcity-3ds PRIVATE
        ${ARM_C_FLAGS}
    )

    target_compile_definitions(simcity-3ds PRIVATE
        SIMCITY_PLATFORM_3DS
        TOP_SCREEN_WIDTH=256
        TOP_SCREEN_HEIGHT=240
        BOTTOM_SCREEN_WIDTH=256
        BOTTOM_SCREEN_HEIGHT=240
    )

    # Link to a real ELF, then package a genuine 3DSX container in a POST_BUILD step.
    # Linking straight to a ".3dsx" name produces a raw ELF that no 3DS loader
    # (Luma/Azahar/FBI) accepts.
    set_target_properties(simcity-3ds PROPERTIES
        OUTPUT_NAME "simcity"
        SUFFIX ".elf"
    )

    find_program(THREEDSXTOOL 3dsxtool
        HINTS "$ENV{DEVKITPRO}/tools/bin" "${DEVKITPRO}/tools/bin"
    )

    set(THREEDSX_SMDH "${CMAKE_SOURCE_DIR}/simcity.smdh")
    set(THREEDSX_OUT "${CMAKE_BINARY_DIR}/simcity.3dsx")

    if(THREEDSXTOOL AND EXISTS "${THREEDSX_SMDH}")
        add_custom_command(TARGET simcity-3ds POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f "${THREEDSX_OUT}"
            COMMAND ${THREEDSXTOOL} $<TARGET_FILE:simcity-3ds> "${THREEDSX_OUT}"
                    --smdh=${THREEDSX_SMDH}
            COMMENT "Packaging 3DSX container: ${THREEDSX_OUT}"
            VERBATIM
        )
    else()
        message(FATAL_ERROR
            "3dsxtool not found or simcity.smdh missing — refusing to emit a raw "
            "ELF renamed .3dsx (unloadable).")
    endif()

    # Headless test for 3DS disabled — C++ audio linking issues.
    # The main .3dsx target links fine; headless-test has C++ symbol issues.
endif()

# ── Install targets ───────────────────────────────────────────────────
if(PLATFORM STREQUAL "DS")
    install(TARGETS simcity-ds RUNTIME DESTINATION .)
else()
    install(TARGETS simcity-3ds RUNTIME DESTINATION .)
endif()
