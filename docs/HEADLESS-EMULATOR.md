# The headless emulator environment

**Status**: working. Verified on the Steam Deck on 2026-10-21.

The AppImage of Azahar cannot be used as a measurement environment. This
document describes the replacement: an Azahar built from source with no Qt
frontend at all.

## Why the AppImage is not usable

It dies during startup, before the guest ever runs:

```
QPixmap::scaled: Pixmap is a null pixmap
```

That is a Qt assertion, and it aborts the process. The AppImage also has no
`offscreen` Qt platform plugin — the only ones it ships are `xcb`, `wayland`
and `wayland-egl` — so there is no way to run it without a display. Neither
machine has `sudo`, so `xvfb` is not an option either.

An `LD_PRELOAD` shim that intercepts `abort()` does convert the crash into a
clean exit, but the emulator still stops at guest frame 1 and the debug
bridge on port 5002 never opens. It buys a stack trace, not a working
emulator.

## What the headless build is

A fork of `https://github.com/azahar-emu/azahar` at
`/home/seyon/dev/Games/PC/headless-citra`, branch `headless`.

- No Qt, no SDL, no GPU, no window. A headless binary has no pixmap to be
  null.
- `src/citra_meta/headless_main.cpp` — a minimal `Frontend::EmuWindow` and
  `Frontend::ImageInterface` that drive `System::Load()` and
  `System::RunLoop()` directly.
- The Qt frontend's null-pixmap sites are also guarded, so the Qt build does
  not hit the same assertion.

```
ENABLE_QT=OFF ENABLE_SDL2=OFF ENABLE_SCRIPTING=ON ENABLE_GDBSTUB=ON
ENABLE_SOFTWARE_RENDERER=ON ENABLE_OPENGL=OFF ENABLE_VULKAN=OFF
ENABLE_CUBEB=OFF ENABLE_OPENAL=OFF ENABLE_FFMPEG=OFF
ENABLE_DISCORD_RPC=OFF ENABLE_ROOM=OFF ENABLE_LIBUSB=OFF
```

## Running it

```
./simcity-headless [options] <rom_path>
```

| Option | Meaning |
|---|---|
| `--rom <path>` | ROM to run (may also be given positionally) |
| `--data-dir <path>` | override the Azahar user data directory (default `$XDG_DATA_HOME/azahar-emu`) |
| `--max-frames <n>` | stop after n emulated slices (0 = unlimited) |
| `--max-seconds <n>` | stop after n wall-clock seconds (0 = unlimited) |
| `--log <filter>` | log filter, e.g. `"*:Info Debug.Emulated:Debug"` |
| `--log-file <path>` | write the emulator log to this file |
| `--gdbstub <port>` | listen for a GDB client on this TCP port |

The guest's SD card is `<data-dir>/sdmc/`, and the guest writes its trace log
there. The Deck's is `/home/deck/.local/share/azahar-emu/sdmc/`, which is
where `simcity.sfc` lives. **The ROM is never copied off the Deck and never
enters this repository.**

## Deployed on the Deck

```
/home/deck/simcity-test/simcity-headless        29 MB, stripped
/home/deck/simcity-test/simcity-headless.debug  380 MB, separate symbols
```

Split debug info: small enough to scp, symbols kept for post-mortem.

## What the headless build exposed

The headless build is the first thing to run this port without a Qt
frontend, and it found two port defects that had been hiding behind the
AppImage's crash:

- **T042** — the audio drain loop underflowed its unsigned counter and hung
  the port on the first frame producing more than 512 audio frames.
- **T043** — the debug bridge's listen socket was blocking, so the guest
  froze whenever no host was attached.

Both are documented in `aes/tickets/`.

## Diagnostics

The GDB stub is started by the frontend, not the core, so the headless
frontend starts it itself:

```
./simcity-headless --gdbstub 24689 rom.3dsx
```

```
$ gdb -batch -ex "target remote :24689" -ex "info threads" -ex "info registers pc"
```

`build/simcity.elf` has DWARF, so `addr2line -f -e build/simcity.elf <addr>`
turns a guest address into a source line. That is how T042 and T043 were
found: the guest's PC alternated between two addresses, which showed it was
looping rather than blocked, and the addresses named the function.

The port's own globals can be read out of guest memory the same way
(`nm build/simcity.elf | grep g_frame_count` gives the address), which is
how "frozen at frame 60" was established rather than assumed.

## Limitations

- **No audio.** Azahar has no `ndsp:u` service, so the port's NDSP audio
  path is a no-op under the emulator. See **T045**. Silence here is evidence
  of nothing.
- The guest's `printf` goes to the bottom screen, not to the emulator's
  stdout. The port's trace file is the real diagnostic channel.
