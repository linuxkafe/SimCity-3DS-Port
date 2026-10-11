# T011 — 3DS Build Linking Log

## Summary
The 3DS `.3dsx` executable now links successfully. Build produces
`build/simcity.3dsx` (37 MB, unstripped, with debug info).

## Blockers Resolved

### 1. `cannot find 3dsx_crt0.o`
**Root cause**: The `3dsx.specs` file's `*startfile:` directive references
`3dsx_crt0.o` without a path. The linker searches default library paths
(`/opt/devkitpro/devkitARM/lib/gcc/arm-none-eabi/16.1.0/`) but not
`/opt/devkitpro/devkitARM/arm-none-eabi/lib/armv6k/fpu/`.

**Fix**: Added `-B/opt/devkitpro/devkitARM/arm-none-eabi/lib/armv6k/fpu/`
to `target_link_options`. This tells GCC to use that directory as a
startfile prefix, so the spec file's `3dsx_crt0%O` pattern resolves
correctly.

### 2. VFP register arguments mismatch
**Root cause**: The `3dsx_crt0.o` (MPCore, no VFP ABI tag) is compiled
without VFP register arguments, while our object files use
`-mfloat-abi=hard` (VFP registers). The linker refuses to mix them.

**Fix**: Added `-Wl,--no-warn-mismatch` to suppress the ABI mismatch
warning. This is safe because the 3DS ARM11 MPCore does support VFP,
and the crt0 is a minimal entry stub that doesn't pass float arguments.

### 3. Duplicate libctru linking
**Root cause**: `target_link_libraries` was called twice with the same
libraries, and `libctru.a` appeared twice in a single invocation.

**Fix**: Deduplicated to a single `target_link_libraries` call with each
library listed once, in correct dependency order:
`simcity-static-recomp` → `libcitro2d.a` → `libcitro3d.a` → `libctru.a`
→ `libgcc.a` → `libc.a` → `libm.a`.

### 4. Missing `sc_audio_epoch_descriptor`
**Root cause**: The `GENERATED_CORE_SOURCES` glob used `GLOB` (non-recursive)
matching `generated/audio/*.c`, but `sc_audio_epoch_registry.c` lives in
`generated/audio/epoch-topology/`.

**Fix**: Changed `GLOB` to `GLOB_RECURSE` for `GENERATED_CORE_SOURCES`.
This picks up all `.c` files under `generated/audio/` including
subdirectories.

## Build Command
```bash
docker run --rm \
  -v "/home/seyon/dev/Games/PC/scssr:/upstream:ro" \
  -v "/home/seyon/dev/Games/Nintendo/DS/SimCityDS:/work" \
  -w /work \
  devkitpro/devkitarm \
  bash -c "export DEVKITARM=/opt/devkitpro/devkitARM && \
    export PATH=\$DEVKITARM/bin:\$PATH && \
    mkdir -p build && cd build && \
    cmake .. -DCMAKE_TOOLCHAIN_FILE=\$DEVKITARM/cmake/Platform/Nintendo3DS.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DUPSTREAM_SRC_DIR=/upstream/SimCity-SNES-Static-Recomp/static-recomp \
      -DPLATFORM=3DS && \
    make -j\$(nproc)"
```

## Output
- `build/simcity.3dsx` — 37 MB, ELF 32-bit ARM, statically linked
- `build/libsimcity-static-recomp.a` — static library

## Next Steps
1. Strip debug info to reduce size (~7 MB stripped vs 37 MB unstripped)
2. Add SMDH metadata and icon (3dsxtool or manual)
3. Test on Citra emulator
4. Verify ROM loading from SD card path