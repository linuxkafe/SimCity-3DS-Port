# T001 Plan — Port static-recomp core to ARM11/DS-Nitro

## Phase 1: Hostile Analysis

### INSIGHTS CONSULTED
- static-recomp CMakeLists.txt (upstream project)
- static-recomp/include/simcity_static_recomp.h (public API)
- static-recomp/internal/include/sc_machine.h, sc_v11_runtime.h, sc_v28_video.h
- static-recomp/src/sc_machine.c, sc_v11_bus.c, sc_v28_video.c, simcity_static_recomp.c
- frontend/linux/main.c, menu.h, settings_ini.h (frontend reference)

### ASSUMPTIONS I'M MAKING

- [KNOWN] The DS is dual-screen, ARM9 @ 67 MHz, 256 KB BWRAM + 128 KB IWRAM.
  — Justification: official NTR hardware spec (documented).
- [KNOWN] devkitpro/devkitarm image is available via Docker.
  — Justification: confirmed by `docker images` output.
- [KNOWN] Generated dispatch shards are ROM-certified and must remain unchanged.
  — Justification: upstream CLAUDE.md Never-Do list.
- [INFERRED] The 32,040 Hz stereo PCM output is too heavy for the DS ARM9 alone
  (1.3 million 16-bit samples/sec = ~2.6 MB/s CPU time at 67 MHz).
  → evidence chain: audio_sink() fires on every frame → PCM drain must be in
  a separate co-processor or aggressively downsampling.
- [ASSUMED] DS top screen (256×192) can display the 256×239 SNES frame with
  vertical cropping; bottom screen (256×192) used for HUD/touch input.
  — Impact if false: game appears cropped; no solution without mode change.
- [ASSUMED] `double` arithmetic for NTSC timing must be replaced by fixed-point
  integer rational arithmetic to avoid 64-bit div which is slow on ARM9.
  — Impact if false: frame rate drops below 30 fps on DS hardware.
- [UNKNOWN] Whether melonDS or DeSmuME supports headless execution for frame
  SHA-256 validation.
  — Outside reliable knowledge at this phase.

### WHAT WASN'T SPECIFIED
- Audio strategy (ARM7 co-processor vs. ARM9 software vs. downsampled mono).
- Widescreen support (likely infeasible on DS).
- Touchscreen-specific input mapping.
- Cartridge save persistence mechanism (GBA-style battery backup via cartridge).

### ALTERNATIVES I DIDN'T CHOOSE
- Option A: Rewrite the core from scratch — rejected because it would lose
  ROM-certified dispatch and certification guarantees.
- Option B: Run upstream Linux binary via Wine — rejected because there's no
  Wine port for DS; architecture mismatch.
- Option C: Use an existing SNES emulator (no$gba style) — rejected by Never-Do
  rule; we must use static recomp, not an interpreter/JIT fallback.

### RISKS & SIDE EFFECTS
- The `SCV11Runtime` structure is ~50 KB of state; on DS it must fit in IWRAM
  or BWRAM with the correct linker sections.
- Generated dispatch uses `uint64_t` for master clock counters; ARM9 has no
  hardware 64-bit divide — must verify correctness (no divide-by-variable in hot path).
- NTSC timing uses `double` rational arithmetic; DS ARM9 lacks FPU.
- SDL2 frontend references `SDL_GetWindowSize`, `SDL_RenderCopy`, etc. — all
  must be replaced by DS-native graphics APIs (libnds `videoMain()` etc.).
- The upstream C++ sources (`snes9x-bapu-aot/*.cpp`) need C++17 compatible
  cross-compile; arm-none-eabi-g++ must handle the subset used.

### COST OF BEING WRONG
HIGH — wrong memory layout or 64-bit arithmetic will cause silent corruption or
crash on DS hardware, with no way to debug without hardware.

### REASONING SKELETON
- Claim: DS ARM9 cannot run the core at playable speed without changes.
  → Premise: ARM9 is 67 MHz, no FPU; upstream runs at ~60 fps on x86_64.
  → Premise: `simcity_recomp_advance()` must produce 32,040 Hz stereo PCM.
  → Inference: PCM generation at 32,040 Hz × 2 channels × 2 bytes ≈ 128 KB/s
    of audio CPU cycles at 67 MHz is borderline.
  → Conclusion: The core may need audio offload or adaptive sampling.

### SCOPE BOUNDARIES DECLARATION
This analysis covers: memory layout, 64-bit arithmetic, FPU absence, audio,
PPU rendering adaptation, and frontend replacement.
Excludes: cartridge save persistence (T004), widescreen (infeasible), full audio
pipeline (T005), settings overlay (T007).

---

## Phase 2: Solution Proposal

### Approach
1. **Create a DS-native CMakeLists.txt** that copies the upstream core sources
   and generates the `.nds` ROM.
2. **Adapt `src/` files**: add DS-specific hooks for PPU, input, and audio.
3. **Create `frontend/ds/`**: minimal DS frontend using NitroSDK (libnds) —
   advances one frame per user action, renders to top screen, accepts input.
4. **Memory partitioning**: place `SCV11Runtime` in IWRAM; large generated dispatch
   data in BWRAM; frame buffers in VRAM.
5. **Integer-only timing**: replace `double` rational math with `uint64_t` /
   `uint32_t` integer operations.

### What I will NOT change
- Generated dispatch shards (`static-recomp/generated/`).
- Handwritten core logic in `sc_machine.c`, `sc_v11_bus.c`, etc. (copy as-is,
  fix compile errors only).
- The upstream CMakeLists.txt.

### Verification criteria
- `make docker-build` exits 0.
- The produced `.nds` boots in melonDS.
- After 100 frames, `simcity_recomp_instruction_count()` > 0.
- Frame output SHA-256 matches upstream for the same ROM + input sequence.

---

## Phase 3: Implementation Plan

### Files to create
```
SimCityDS/
├── CMakeLists.txt              ← DS build configuration
├── Makefile                    ← Convenience targets
├── src/
│   ├── ds_port.h               ← DS-specific declarations
│   └── ds_audio_hack.c         ← Downsampled mono audio stub
├── frontend/ds/
│   ├── ds_main.c               ← libnds entry point
│   ├── ds_input.c              ← DS button + touch handling
│   ├── ds_input.h
│   ├── ds_render.c             ← top/bottom screen rendering
│   ├── ds_render.h
│   └── ds_audio.c              ← DS audio output (ARM7 or ARM9 software)
├── ds_linker.ld                ← Memory layout (IWRAM/BWRAM/VRAM)
├── .gitignore
└── docs/                       ← already created
```

### Source copy strategy
- Copy the handwritten upstream core sources into `src/upstream/` as-is.
- Create thin wrappers in `src/ds_*.c` that adapt the public API to DS calling conventions.
- Build the generated dispatch as a precompiled dependency (unchanged).

### First deliverable
A `.nds` that boots to black screen, accepts input, and prints to console.
Progressive: then 1 frame rendered, then full loop.