# T001 Review — SimCity SNES Static Recomp → Nintendo DS Port

## Gate Verdict: REJECTED (BLOCKER)

The port as designed **cannot succeed** on the Nintendo DS without a fundamental
architectural change to the upstream core. The generated dispatch code is 9×
larger than the DS ARM9 memory budget.

---

## Evidence

### Memory Budget (from `ds9.ld` linker script)
```
lma9:    ORIGIN=0x2004000, LENGTH=0x27c000   (~2.8 MB ROM load)
main:    ORIGIN=0x2001000, LENGTH=0xf7f000   (~16 MB addressable RAM)
Assertion: . <= 0x2380000                   (3.5 MB hard limit for DS mode)
```

### Compiled Core Size (from Docker build)
| Component | Size (KB) | Share of 3.5 MB budget |
|-----------|-----------|----------------------|
| v35 shards (98 files) | 25,220 | **720%** |
| v34 shards (64 files) | 5,992 | 171% |
| v11 shards (18 files) | 1,868 | 53% |
| Generated core | 1,080 | 31% |
| Handwritten src | 572 | 16% |
| Audio (C++ + DSP) | 200 | 6% |
| **Total stripped** | **~9,000** | **257%** |
| Total unstripped | 35,407 | 1,011% |

**Shortfall: ~5,500 KB over the 3,500 KB DS mode limit (1.6×), or ~31,900 KB if
unstripped (9×).**

### Per-shard breakdown (most expensive)
- `sc_v35_group_000E5.o`: 591 KB
- `sc_v35_group_0006B.o`: 540 KB
- `sc_v35_group_000EC.o`: 525 KB
- Average per v35 shard: **261 KB**

Each v35 shard is a ~1 KB-address-range context dispatch table containing
hundreds of case labels. They are the dominant size contributor.

---

## Additional Findings (non-blocking but must be fixed)

### 1. ds_render.c — compilation errors
The macros `TOP_SCREEN_WIDTH`, `TOP_SCREEN_HEIGHT` etc. are not defined.
They must be `DS_TOP_SCREEN_WIDTH`, `DS_TOP_SCREEN_HEIGHT` (as defined in
CMakeLists.txt via `-D` flags). **Fix: add `DS_` prefix.**

### 2. C++ namespace linkage
`sc_static_apu.cpp` references `SC_STATIC_SNES::smp` and
`SC_STATIC_SNES::dsp` which are defined in `smp.cpp` / `sdsp.cpp`. These
compile individually but the linker reports undefined references because the
C++ objects are compiled without the namespace context (likely a missing
`#include` or `using namespace`). This is an upstream issue — the C++ sources
use internal names that may be affected by the `-fno-rtti` / `-fno-exceptions`
flags.

### 3. Audio sample rate
The core produces 32,040 Hz stereo PCM. The DS audio hardware supports up
to 44,100 Hz but mixing to mono 22,050 Hz is a reasonable approximation.
This is not a blocker.

---

## Root Cause Analysis

The static recompilation generates one dispatch function per instruction-address
context on the SNES W65C816. Each "shard" covers a 1 KB range of guest PC
addresses and contains a switch/case table mapping context states to the
next PC. SimCity's game logic is dense enough that the number of distinct
contexts is very large — especially in the v35 "town route" phase where the
game builds and manages a city.

The v35 shards alone (25 MB) cover 98 × 1 KB = 98 KB of guest PC space,
with an average expansion factor of ~260× per shard. This expansion is what
makes the approach viable on x86_64 (where 25 MB is trivial) but impossible
on the DS (3.5 MB hard limit).

---

## Options to Proceed

### Option A: Skip v35/v36 shards (partial playability)
Compile only v11 (bootstrap) + v34 (early game) shards. This covers the
boot sequence, title screen, and early city setup (~4 MB total, still too
large). Not viable without further reduction.

### Option B: Prune the dispatch (requires upstream modification)
The upstream project has a `tools/prune_shadowed_contexts.py` tool. Running
this aggressively (prune all but the most common contexts) could reduce
the dispatch size. However, this changes the generated code and breaks
ROM certification guarantees — directly violates the Never-Do rule in
CLAUDE.md.

### Option C: Use a different emulation approach
Replace the static recompilation with a traditional SNES emulator (no$gba
style) that runs on the DS. This is the standard approach for SNES-on-DS
projects and would fit within the memory budget. **However**, this is
explicitly prohibited by the Never-Do rule ("Do NOT introduce a runtime
interpreter, JIT, or dynamic recompiler").

### Option D: Target DSi enhanced mode
The DSi extends the ARM9 memory to ~16 MB (LMA9I region). Even so, 9 MB
still barely fits and leaves no room for VRAM, frame buffers, or runtime
data. Marginally more feasible but still problematic.

### Option E: Reduce source level (architectural redesign)
Instead of porting the full static recomp, port only the minimal subset:
- Use the upstream's `headless-test` approach to validate a subset of
  the dispatch.
- Generate a smaller, purpose-built dispatcher for the DS that covers
  only the SimCity-specific paths needed.
- This is a net-new implementation, not a port.

---

## Recommendation

**Option E** is the only path consistent with the Never-Do rules. The full
static-recomp core cannot fit on DS hardware. A new, smaller dispatcher
targeted at the DS memory budget should be designed from first principles,
starting from the upstream source manifests (SOURCE-MANIFEST.json) to
understand which instruction contexts are actually executed during normal
gameplay, and generating only those.

This would be a separate project (T009) and is out of scope for T001.

---

## Updated Kanban

| Ticket | Title | Status |
|--------|-------|--------|
| T001 | Port static-recomp core to ARM11/DS-Nitro | **BLOCKED** |
| T009 | Design DS-native dispatcher (smaller footprint) | pending |
| T002–T008 | (frontend, audio, SRAM, etc.) | **PENDING** — blocked by T001 |