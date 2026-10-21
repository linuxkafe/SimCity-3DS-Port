# Deck Measurement Runbook

**Status:** current as of 2026-10-21. Supersedes the scattered procedures in
`docs/DECK-RUN-2026-10-19.md` and `docs/DECK-STABILITY-2026-10-19.md`.

## The one thing to check first

Before any measurement run, the Azahar configuration decides whether the 3DS
can read its ROM at all. The wrong configuration produces a **70-byte trace
file** that reads like a hang but is a failed init.

```bash
ssh deck@steamdeck 'grep -E "^(sdmc_directory|nand_directory|use_custom_storage|use_virtual_sd)=" \
    ~/.config/azahar-emu/qt-config.ini'
```

### Working configuration

```
sdmc_directory=/home/deck/.local/share/azahar-emu/sdmc/
nand_directory=/home/deck/.local/share/azahar-emu/nand/
use_custom_storage=false
use_virtual_sd=true
```

This is what Azahar writes when it regenerates its own config. To get it
back:

```bash
ssh deck@steamdeck 'rm -f ~/.config/azahar-emu/qt-config.ini'   # regenerated next launch
```

### Failing configuration — do NOT use

```
sdmc_directory=/run/media/mmcblk0p1/Emulation/storage/azahar/sdmc/
use_custom_storage=true
```

This is what `~/.config/EmuDeck/backend/configs/azahar/qt-config.ini`
specifies. Copying the EmuDeck reference over the live config reintroduces
the stall. Azahar rewrites `use_custom_storage` to `false` at startup, so
editing that key by hand does not stick — the *paths* the two configs leave
behind are what differ.

## The SD card the emulator actually mounts

There are two `sdmc:` directories on the Deck. Only one is mounted:

| path | contents | visible to the guest? |
|---|---|---|
| `~/.local/share/azahar-emu/sdmc/` | the mounted one | **yes** |
| `/run/media/deck/mmcblk0p1/Emulation/storage/azahar/sdmc/` | the media one | only if configured |

The app's own trace file is written to whichever is mounted, so the presence
of `simcity-trace.log` in a directory is the proof of which one it is.

**A symlink of the same name in the other directory is never seen by the
emulator.** A build compiled against `sdmc:/simcity.sfc` failed to open it
even though a symlink of that name existed in the unmounted directory.

The ROM must be a **real file** in the mounted root. `SimCity(USA).sfc` has
parentheses and spaces, which the 3DS FAT driver has to resolve through
long-name handling; `simcity.sfc` and `ROM.SFC` are short 8.3 names and are
preferable. The port tries all three in order and traces each attempt.

## Launching

```bash
ssh deck@steamdeck 'bash /home/deck/simcity-test/launch.sh \
    /home/deck/simcity-test/<build>.3dsx'
```

`launch.sh` sets `XDG_RUNTIME_DIR`, `WAYLAND_DISPLAY=wayland-0` and
`QT_QPA_PLATFORM=wayland`, and `setsid --fork`s the AppImage so the session
survives the SSH disconnect.

**The `simcity-azahar` systemd unit does not work.** It cannot reach the
Wayland socket from its environment, and `QT_QPA_PLATFORM=xcb` then aborts
with `SIGBUS` in `libQt6Gui` (coredump 105315). Use the launcher script.

**`launch.sh` deletes the trace file from the wrong directory.** It removes
`~/.local/share/azahar-emu/sdmc/simcity-trace.log`; if the mounted root is
the media one, the stale file survives and a diagnosis can be made against
a previous run's output. Delete the trace file from the *mounted* root
before a run, and check its mtime.

## Reading the result

```bash
ssh deck@steamdeck 'cat <mounted-root>/simcity-trace.log'
```

A healthy run contains, in order:

```
[T0] fread: 524288/524288 bytes
[T0] ROM loaded OK
[T1] create: rc=1 inst=0x...
[T4] loop: entered, auto-advance on (START toggles pause)
[T6] advance #N: done route=1 rendered=1 frames N..N+1
```

If it stops at `[T0] fopen:`, the ROM was not found. If it stops at
`[T0] fread:`, the read hung. If it stops at `boot:` with no `[T0]` line
at all, the build predates the T038 tracing.

## The debug port is currently unusable

Port 5002 is refused while the app runs, because the recurring
`svc=0x32` fault takes the network stack with it:

```
Kernel.SVC <Error> ... svc=0x32 raw=0xD8E007F7 level=27 summary=7 module=1 description=1015
```

**The trace log is the only measurement channel.** The `state`, `prof` and
`fbrd` verbs cannot be used while the app is running. See T040.

## Measurement discipline

- **Measure the control in the same session as the arm.** The Deck's
  endpoint has moved three times already (T035, T037, T038), and each time
  a plausible root cause was asserted from a flapping control.
- **Record the md5 of the binary on both the dev host and the Deck** before
  every run.
- **A threshold that changes between runs is not a threshold.** In T038 a
  clean-looking 32 KB → 40 KB boundary was measured; the same file then
  read fine in other runs. It was the environment, not a FAT cluster limit.
