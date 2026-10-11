# 3dsdbgkit — 3DS Homebrew Debug Bridge

> **⚠️ STATE: UNPLAYABLE / EXPERIMENTAL**
>
> This project is a work-in-progress debug toolkit for Nintendo 3DS homebrew development.
> It is **not playable**, not stable, and not ready for end users.

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