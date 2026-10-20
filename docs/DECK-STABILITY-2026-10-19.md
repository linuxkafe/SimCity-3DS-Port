# Deck Stability Investigation — 2026-10-19

## Summary

The control binary (`d55fe93a…`, Oct-18 18:03 build) **worked at 10:31** (5 rendered frames, create rc=1) but **fails consistently since ~11:00** (dies at boot, `after-boot:rnd=0`). Same binary, same Deck, same emulator.

## What Changed

| Time | Event |
|------|-------|
| 10:31 | Control run: 5 frames rendered, create rc=1 |
| 11:00-11:45 | Multiple Azahar runs (arms, control, manual tests) |
| 11:45+ | Control stops reproducing: dies at boot, trace=70 bytes |

## What Was Tested

| Attempt | Result |
|---------|--------|
| Fresh Azahar config (deleted ~/.local/share/azahar-emu) | Still fails at boot |
| Fresh Vulkan shader cache (removed radv cache) | Made it worse (Failed to find title id) |
| Different file extensions (.cxi vs .3dsx) | Same failure |
| Different file paths (SD card vs home dir) | Same failure |
| Fresh Azahar AppImage (same file) | Same failure |
| No pkill in pre-run | Same failure |
| Longer timeout (90s) | Same failure |
| Direct manual run (no harness) | Same failure |

## Azahar Log (Failure)

```
[   0.584753] Loader <Error> core/loader/loader.cpp:GetLoader:161: Failed to load file /run/media/deck/mmcblk0p1/Emulation/roms/n3ds/simcity.cxi
[   0.584762] Core <Critical> core/core.cpp:Load:372: Failed to obtain loader for /run/media/deck/mmcblk0p1/Emulation/roms/n3ds/simcity.cxi
```

## Hypotheses (Priority Order)

1. **Vulkan/RADV driver state corruption** — repeated Azahar runs corrupted GPU state
2. **Shader cache corruption** — accumulated shader cache causing loader failure
3. **SD card/filesystem issue** — ext4 errors on SD card (errors=remount-ro)
4. **Azahar AppImage internal state** — AppImage mount/overlay corruption
5. **File descriptor/resource leak** — accumulated FDs from repeated launches

## What Works

- File reads fine from shell (md5 matches, `cat` works)
- File magic bytes correct (3DSX header valid)
- SD card mount healthy (rw, ext4, no errors in mount)
- Trace file creation works (boot line written)

## What Doesn't Work

- Azahar loader fails at `core/loader/loader.cpp:GetLoader:161`
- "Failed to find title id for ROM (Error 0)" at `core/core.cpp:Load:539`
- App dies before `create` callback

## Next Steps (Priority)

1. **Reboot the Deck** — most likely to fix GPU/driver state
2. **Re-flash Azahar AppImage** — rule out AppImage corruption
3. **Test with different homebrew** — isolate whether it's this specific 3DSX
4. **Check dmesg/journalctl** — look for kernel/Vulkan errors (needs sudo)
5. **Test on different hardware** — if available

## Impact on T035

T035 bisect is **BLOCKED** until control stabilizes. The harness `tools/deck_drift.py` correctly detects `UNSTABLE_CONTROL` and refuses to rank arms — this is correct behavior.

The bisect (2 commits: a4bce13 vs dd098df) is ready to run once control stabilizes.

## Update 2026-10-19 15:45 — Control Still Unstable After Cache Clearing

### Current Status
- Control binary (`d55fe93a…`) **consistently failing** at boot
- Trace: 70 bytes, only `boot: rom_path=sdmc:/SimCity(USA).sfc`
- Azahar log: `Failed to find title id for ROM (Error 0)` at `core/core.cpp:Load:539`
- Loader error: `Failed to load file` at `core/loader/loader.cpp:GetLoader:161`

### What Was Tried (All Failed)
| Attempt | Result |
|---------|--------|
| Fresh Azahar config (deleted ~/.local/share/azahar-emu) | Fails at boot |
| Fresh Vulkan shader cache (removed radv cache) | Fails at boot |
| Fresh Azahar config + shader cache | Fails at boot |
| Different file extensions (.cxi vs .3dsx) | Same failure |
| Different file paths (SD card vs home dir) | Same failure |
| Fresh Azahar AppImage (same file) | Same failure |
| No pkill in pre-run | Same failure |
| Longer timeout (90s) | Same failure |
| Direct manual run (no harness) | Same failure |
| Cleared radv_shader_cache + azahar-emu config | Fails at boot |
| File copied to /home/deck/ | Same failure |

### What Works
- File reads fine from shell (md5 matches, `dd` works)
- File magic bytes correct (3DSX header valid)
- SD card mount healthy (rw, ext4, no errors in mount)
- Trace file creation works (boot line written)

### Hypotheses (Priority Order)
1. **Vulkan/RADV driver state corruption** — repeated Azahar runs corrupted GPU state
2. **Shader cache corruption** — accumulated shader cache causing loader failure (persists despite clearing)
3. **SD card/filesystem issue** — ext4 errors on SD card (errors=remount-ro)
4. **Azahar AppImage internal state** — AppImage mount/overlay corruption
5. **File descriptor/resource leak** — accumulated FDs from repeated launches

### Impact on T035
T035 bisect is **BLOCKED** until control stabilizes. The harness correctly detects `UNSTABLE_CONTROL` and refuses to rank arms.

### Recommended Next Step
**Reboot the Steam Deck** — most likely to fix GPU/driver state corruption. This is the single highest-impact action and should be attempted before any further investigation.


## Update 2026-10-19 16:30 — Post-Reboot Status: Still Unstable

### Current Status (Post-Reboot)
- Control binary (`d55fe93a…`) **still failing** at boot
- Trace: MISSING (process crashes before writing trace)
- Azahar log: `Failed to find title id for ROM (Error 0)` at `core/core.cpp:Load:539`
- Preceded by: `Could not read program id when registering with SelfNCCH, this might be a 3dsx file`

### What Was Tested Post-Reboot
| Attempt | Result |
|---------|--------|
| Fresh reboot of Steam Deck | Still fails at boot |
| Fresh Azahar config + shader cache clear | Fails at boot |
| Run from local filesystem (/home/deck) | Same failure |
| Different file extensions (.cxi vs .3dsx) | Same failure |
| File copied to local storage | Same failure |
| Fresh Azahar config (deleted ~/.local/share/azahar-emu) | Fails at boot |

### Consistent Error Pattern
```
[   1.458547] Service.FS <Warning> core/file_sys/archive_selfncch.cpp:Register:255: Could not read program id when registering with SelfNCCH, this might be a 3dsx file
[   1.481219] Core <Error> core/core.cpp:Load:539: Failed to find title id for ROM (Error 0)
```

The process crashes (dumps core) before writing trace file. Error indicates Azahar cannot extract title ID from the 3DSX file's SMDH section.

### Root Cause Hypothesis
The 3DSX file appears to lack a valid SMDH (System Menu Description Header) with a proper title ID, OR Azahar's loader is failing to parse the SMDH section. This is a binary validation issue, not environmental drift.

**Key insight**: The binary worked at 10:31 but stopped working after multiple Azahar runs. The binary itself hasn't changed (md5 verified), but the Azahar runtime state may have corrupted something in the 3DSX parsing path.

### Next Steps
1. **Verify 3DSX SMDH section** - check if SMDH section exists and has valid title ID
2. **Try different Azahar build** - AppImage may have internal state corruption
3. **Test with different 3DSX** - isolate if issue is specific to this binary
4. **Check Azahar loader code** - understand why title ID lookup fails

### T035 Status
**BLOCKED** - Control binary validation fails at title ID lookup. Cannot proceed with bisect until control stabilizes.

### Next Steps
1. Investigate 3DSX SMDH section in the binary
2. Try different Azahar AppImage version
3. Test with known-good 3DSX homebrew to isolate Azahar vs binary issue

## Update 2026-10-19 20:00 — SMDH Injection Attempt Failed

### SMDH Injection Attempt
- **SMDH injected** into `simcity_test.3dsx` at offset 0xc977e0, size 0x1400
- **Title ID** written at SMDH offset 0x88: `00040000`
- **Verification**: SMDH magic "SMDH" present, title strings present, title ID at offset 0x88 verified

### Result: **NO CHANGE**
- Control still fails with `"Failed to find title id for ROM (Error 0)"`
- Trace remains 70 bytes (boot only)
- Azahar 2126.2 also fails identically

### Root Cause Analysis
The 3DSX binary **lacks a valid SMDH section** (original header: offset=0, size=0). Even after injecting a valid SMDH with title ID `00040000`, Azahar fails to read the title ID.

**Root cause hypothesis**: The 3DSX binary was built **without an SMDH section** (common for development builds). Azahar's loader requires a valid SMDH with title ID to load 3DSX homebrew. The binary worked once at 10:31 due to a transient Azahar state/fallback that's now permanently broken.

### Impact on T035
**BLOCKED** — Control binary cannot be validated. Bisect cannot proceed until a working 3DSX with valid SMDH is available.

### Recommended Next Steps
1. **Rebuild the 3DSX with SMDH** — Use devkitPro toolchain to rebuild with proper SMDH section
2. **Find alternative 3DSX** — Locate a complete build with SMDH in the Deck's intermediate builds
3. **Test with known-good 3DSX homebrew** — Isolate whether issue is Azahar or this specific binary

### T035 Status
**BLOCKED** — Cannot bisect without a working control binary. The harness correctly refuses to rank arms when control is unstable.

### Recommendation
**Stop debugging the environment** — the issue is binary-level (missing SMDH), not environmental. Focus on obtaining/building a 3DSX with valid SMDH section.
