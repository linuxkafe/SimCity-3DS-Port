# 3dsdbgkit — 3DS Homebrew Debug Bridge

> **⚠️ STATE: EXPERIMENTAL — runs, but not playable, and not yet autonomous**
>
> The game boots, renders correctly on hardware, and runs at a measured cost per
> frame. It is **not playable at speed** and there is **no automated test
> coverage** — every performance number so far was taken by hand against a
> device, which is the main thing now being fixed.

> **🚨 KNOWN PROBLEM: two copyrighted ROM dumps are in this repository's history.**
> `investigation/quarantine-armv7/simcity.cia` (13.1 MB) and `simcity_full.cia`
> (13.6 MB) were committed in `6d1774a` and are reachable from `origin/master`.
> They are now untracked, but untracking does not remove them from history, and
> the remote is public. They were pulled from history on 2026-10-17 — see
> `aes/tickets/T030-rom-exposure.md` in the local AES tree for the record and
> the options weighed. **Do not clone or redistribute this repository until that
> is resolved.**

---

## What is 3dsdbgkit?

**3dsdbgkit** is a reusable debug bridge for **libctru-based 3DS homebrew** that runs on the console itself. It provides two debugging interfaces over a single TCP port (default 5002):

| Interface | Purpose |
|-----------|---------|
| **GDB RSP Stub** | Full GDB Remote Serial Protocol support — breakpoints, memory read/write, register inspection, single-step, continue. Attach with `target remote <3ds-ip>:5002` in `arm-none-eabi-gdb`. |
| **Extension Protocol** | Line-oriented ASCII commands for app-specific introspection (framebuffer dump, structured memory dump, custom queries). Coexists with RSP on the same socket. |

**Key properties:**
- **Zero SimCity dependency** — the debug kit is completely generic and reusable in any libctru homebrew
- **Non-blocking socket** serviced from your main loop via `dbg_poll()` — debugging never stalls your application
- **Software breakpoints** (ARM `BKPT #0`) — cooperative, trapped at next `dbg_poll()`
- **Framebuffer streaming** — generic pixel dump helpers for any screen or arbitrary address range
- **SOC service integration** — proper 3DS socket initialization via `soc:u`

---

## Integration (3 lines in your homebrew)

```c
#include "3dsdbg.h"

int main() {
    // ... your init ...
    dbg_init();              // once, after SOC/gfx are up

    while (aptMainLoop()) {
        // ... your frame logic ...
        dbg_poll();          // once per iteration
    }
    return 0;
}
```

Then from host:
```bash
arm-none-eabi-gdb your.elf
(gdb) target remote 192.168.1.42:5002
(gdb) break main
(gdb) continue
```

Extension commands (plain text lines, not RSP packets):
```
ping           → OK pong
info           → OK 3dsdbgkit/1 pid=ok breaks=0
brk 0x00102000 → OK break set
dumpmem 0x00100000 256 → OK dumping + binary blob
```

---

## Current state, measured

Performance work so far, all measured on hardware. `core` is
`simcity_recomp_advance()` in ARM11 ticks per frame.

| Change | Effect | Evidence |
|---|---|---|
| `bgra()` integer divides → exact 32-entry table | shipped; the reported −23.7% is **retracted**, see below | `tools/check_rgb565_exact.c`, 0 divergences over all 32768 inputs |
| Core log off by default (removes 2× per-frame SHA-256) | **−14.5%**, active by default | reproduced over 12 interleaved pairs, +21.8M ticks |
| Read frame as BGR555 instead of re-widening BGRA | **no measurable gain** | −1.1%, inside measurement noise |

Three things worth knowing before trusting any number here:

1. **`core` is not a stable clock.** It varies by up to **42%** with scene
   content. Measuring change A and then change B minutes apart measures the
   drift, not the change. Every real result here uses interleaved pairs with
   the difference taken *within* each pair, and discards contaminated pairs
   rather than averaging them in.
2. **An earlier −23.7% figure was retracted.** Drift alone moves 50M ticks
   against a reported 46M effect. The change stays (it is provably
   output-identical and removes 183 584 divides); the *number* was not sound.
3. **Nothing has been verified for output equality on hardware.** The colour
   conversion is proven exhaustively on the host. A device-side equivalence test
   needs a same-frame capture from both builds and has not been done.

Still unattributed: the large majority of per-frame ticks, spread across the
static route loop, `smp_sync_to_master` (the DSP) and the composite pass.

### Patches, not edits

The upstream core is never edited in place. `patches/` holds a chain applied on
every build, so a build is reproducible from a clean checkout:

| Patch | Purpose |
|---|---|
| `0001-cold-reset-sram-on-heap.patch` | ARM11 portability for the cold reset |
| `0002-core-bgra-without-divides.patch` | replaces per-pixel divides with an exact table |
| `0003-core-expose-bgr555.patch` | exposes the composited frame the core already builds |
| `0004-core-switchable-analyze.patch` | makes `analyze()` measurable |

The generated dispatch shards (v11/v34/v35/v36) are ROM-certified and remain
byte-identical to upstream.

---

## Project Layout

```
3dsdbgkit/
├── frontend/3ds/
│   ├── 3dsdbg.c/h      ← Core debug bridge (GDB RSP + extension)
│   ├── 3ds_main.c      ← Example host application entry point
│   ├── 3ds_render.c/h  ← Example rendering
│   └── 3ds_input.c/h   ← Example input handling
├── frontend/ds/        ← Nintendo DS frontend (separate target)
├── src/                ← Static-recompiled SimCity core (upstream, read-only)
├── debug/              ← Debug artifacts, test images, rotation analysis
├── tools/              ← Host-side helpers (netdbg.py, dsdbg.py, raw2png.py)
├── investigation/      ← ARMv7/3DSX packing experiments
├── docs/
│   ├── VISION.md
│   ├── REQUIREMENTS.md
│   └── ROADMAP.md
├── CMakeLists.txt      ← Cross-compile for ARM11 (3DS) and ARM9 (DS)
├── Makefile            ← Convenience targets (docker-build, doctor, etc.)
└── .gitignore          ← Excludes build/, objs/, *.nds, *.3dsx, aes/, .aes/
```

---

## Build

```bash
# Inside Docker (recommended — pulls devkitpro/devkitarm)
make docker-build

# Or with native devkitARM installed
make 3ds-build
```

## Test / Debug

```bash
# Headless frame validation (requires ROM at ~/SimCity(USA).sfc)
make 3ds-test-headless

# Doctor: check environment
make doctor

# Host-side GDB attach helper
python3 tools/netdbg.py <3ds-ip>
```

## Requirements

- Docker with `devkitpro/devkitarm` image (pulled automatically by `make setup`)
- A legally obtained SimCity (USA) SNES ROM at `~/SimCity(USA).sfc` (for the example app)
- melonDS or DeSmuME for DS target; Citra or real 3DS for 3DS target

## Related

- **Upstream core**: SimCity SNES Static Recomp 1.4.0 (handwritten sources + generated dispatch shards)
- **DS port**: Separate frontend in `frontend/ds/` targeting ARM9 / libnds
- **3dsdbgkit**: This debug bridge — reusable in any libctru homebrew

---

## License

MIT — see [LICENSE](LICENSE).