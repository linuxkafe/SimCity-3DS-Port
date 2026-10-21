---
ticket: T041
title: Headless Azahar fork — build from source, and the settings-layer crash it exposes
sprint: sprint-02
priority: high
status: in-progress
depends_on: T031, T038
created: 2026-10-21
---

# T041 — Headless Azahar fork

## Problem

The Azahar AppImage on the Steam Deck cannot be used as a stable test
environment: it dies with

```
QPixmap::scaled: Pixmap is a null pixmap
```

during startup, before the guest ever runs. The `LD_PRELOAD` shim that
intercepts `abort()` only converts the crash into an early exit — the emulator
still stops at guest frame 1 and the debug bridge on port 5002 never opens.

There is no `offscreen` Qt platform plugin in the AppImage
(`Available platform plugins are: xcb, wayland, wayland-egl`), and neither
machine has `sudo`, so `xvfb` cannot be installed. The AppImage path is a
dead end.

**Therefore: build Azahar from source with the Qt frontend removed entirely.**
No Qt, no SDL, no GPU, no window. A headless binary has no pixmap to be null.

## Build

- Fork: `https://github.com/azahar-emu/azahar` at
  `/home/seyon/dev/Games/PC/headless-citra`, branch `headless`.
- New file `src/citra_meta/headless_main.cpp` — minimal `Frontend::EmuWindow`
  and `Frontend::ImageInterface` stubs, driving `System::Load()` +
  `System::RunLoop()` directly.
- `src/CMakeLists.txt` — `citra_meta` / `citra_cli` no longer gated behind
  `ENABLE_QT`.
- `src/citra_meta/CMakeLists.txt` — builds `headless_main.cpp` instead of the
  Qt `main.cpp`; links `citra_core`; `-rdynamic` so crash backtraces resolve.

CMake flags:

```
ENABLE_QT=OFF ENABLE_SDL2=OFF ENABLE_SCRIPTING=ON ENABLE_GDBSTUB=ON
ENABLE_SOFTWARE_RENDERER=ON ENABLE_OPENGL=OFF ENABLE_VULKAN=OFF
ENABLE_CUBEB=OFF ENABLE_OPENAL=OFF ENABLE_FFMPEG=OFF
ENABLE_DISCORD_RPC=OFF ENABLE_ROOM=OFF ENABLE_LIBUSB=OFF
```

Build host has no `sudo`. Missing distro packages (`libboost-dev`,
`libcurl4-openssl-dev`, `nlohmann-json3-dev`, `libssl-dev`, `libzstd-dev`,
`libasound2-dev`, `libusb-1.0-0-dev`) were fetched with `apt-get download` and
extracted with `dpkg-deb -x` into `/home/seyon/.local/build-prefix/`, then
passed to CMake via `CMAKE_CXX_FLAGS=-I...` / `CMAKE_EXE_LINKER_FLAGS=-L...`.

## The crash the headless build exposes

`std::out_of_range: unordered_map::at` during `System::Load()`. Backtrace
(resolved with `addr2line` against the Debug build):

```
main
 └─ Core::System::Load
     └─ Core::System::Init
         └─ Service::Init
             └─ Service::AttemptLLE
                 └─ std::unordered_map<std::string,bool>::at   ← throws
```

Root cause, `src/core/hle/service/service.cpp:205`:

```cpp
if (!Settings::values.lle_modules.at(service_module.name) && ...)
```

`Settings::values.lle_modules` is a `std::unordered_map<std::string,bool>`.
**The Qt configuration layer is what populates it**
(`src/citra_qt/configuration/config.cpp:536-541`, one entry per LLE service
module). With `ENABLE_QT=OFF` nothing populates it, the map is empty, and
`.at()` on a missing key throws.

This is a latent contract violation in the core, not a headless-only quirk:
the core assumes a frontend has already filled the map, and does not check.
The headless build is the first thing to exercise that path without Qt.

**Fix in the headless frontend:** populate the map with HLE-only
(`use_lle = false`) for every entry of `Service::service_module_map`, mirroring
what the Qt config layer does. Upstream-worthy fix: the core should use
`find()` and treat "absent" as "HLE", instead of `at()`.

## Other Qt-layer duties the headless frontend has to reproduce

`System::RunLoop()` runs **one slice** and returns — it is not a loop. The Qt
frontend drives it from `EmuThread::run()` (`src/citra_qt/bootmanager.cpp:63-114`).
The headless frontend has to do the same, and also the setup that precedes it:

| Step | Qt source | Why it matters |
|---|---|---|
| `UpdateCurrentFramebufferLayout` on the window | `bootmanager.cpp` | renderer consults the layout for geometry/scaling |
| `GetAppLoader().ReadProgramId()` + `GPU().ApplyPerProgramSettings()` | `bootmanager.cpp:83-85` | per-title graphics settings |
| `Rasterizer()->LoadDefaultDiskResources()` | `bootmanager.cpp:87-90` | default textures on disk |
| `RegisterCoreLoopThreadId()` | `bootmanager.cpp:103` | identifies the core thread to the RPC layer |
| loop on `RunLoop()` until `ShutdownRequested` | `bootmanager.cpp:109-124` | the actual emulation loop |

## Headless CLI

```
azahar-headless [options] <rom_path>
  --rom <path>        ROM to run (may also be given positionally)
  --data-dir <path>   override the Azahar user data directory
                      (default: $XDG_DATA_HOME/azahar-emu)
  --max-frames <n>    stop after n emulated frames (0 = unlimited)
```

`--data-dir` matters: the guest's SD card maps to `<data-dir>/sdmc/`, and the
guest writes its trace log there. `FileUtil::SetUserPath()` stores the path
verbatim and the per-subdirectory paths are built by string concatenation, so
the headless frontend has to append the separator itself — otherwise the SDMC
dir lands at `<data-dir>sdmc/` instead of `<data-dir>/sdmc/`.

## Acceptance

- [x] Headless binary builds clean from a clean checkout
- [x] `System::Load()` returns Success for a `.3dsx` (no exception)
- [x] Emulator runs the guest for > 50 frames
- [x] Guest trace log shows `[T5] pump: ring_avail=N` with non-zero N
- [x] Debug bridge (port 5002) reachable from the host
- [x] Binary deployed to the Deck as `simcity-headless` and reproducible there

## RESOLVED

Two further port defects surfaced while validating the acceptance criteria,
and both had to be fixed before the guest would run at all. They are separate
tickets because they are port bugs, not emulator bugs, and they are worth
tracking on their own:

- **T042** — the audio drain loop underflows its unsigned counter and hangs
  the port on the first frame that produces more than 512 audio frames.
- **T043** — the debug bridge's listen socket is blocking, so the game
  freezes whenever no host is attached.

Both were found with the headless build and the emulator's GDB stub, neither
of which existed before.

## Deployed artefact

```
/home/deck/simcity-test/simcity-headless        29 MB, stripped
/home/deck/simcity-test/simcity-headless.debug  380 MB, separate symbols
```

Split debug info: the binary is small enough to scp and the symbols are kept
for post-mortem symbolisation.

```
./simcity-headless [--rom <path>] [--data-dir <path>] [--max-frames <n>]
                   [--max-seconds <n>] [--log <filter>] [--log-file <path>]
                   [--gdbstub <port>] <rom_path>
```

Verified on the Deck: port 5002 open, trace log growing, 22 `[T5] pump`
lines with non-zero `ring_avail` in the first 25 s.

## Build notes for the next person

The build host has no `sudo`. Missing distro packages (`libboost-dev`,
`libcurl4-openssl-dev`, `nlohmann-json3-dev`, `libssl-dev`, `libzstd-dev`,
`libasound2-dev`, `libusb-1.0-0-dev`) were fetched with `apt-get download` and
extracted with `dpkg-deb -x` into `/home/seyon/.local/build-prefix/`, then
passed to CMake via `CMAKE_CXX_FLAGS=-I...` / `CMAKE_EXE_LINKER_FLAGS=-L...`.

The `externals/boost` submodule needs `git submodule update --init
externals/boost` run after a fresh `rm -rf` of the directory — the checkout
registers but does not populate.

## Notes

- The trace log is written to the guest SD card, so the Deck (or an SD image
  mount) is required to read it back.
- Azahar's own RPC server is **UDP 45987**, not TCP 5002. Port 5002 is our
  port's own debug bridge (`3dsdbg.c`, `DBGK_PORT 5002`). They are unrelated.
