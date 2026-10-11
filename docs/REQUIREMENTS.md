# REQUIREMENTS — SimCity DS Port

## Functional
- [ ] Boot the exact SimCity (USA) ROM on DS via static dispatch.
- [ ] Render at least one complete frame to the DS screens.
- [ ] Accept controller input (D-pad, A/B/X/Y, Start/Select, L/R).
- [ ] Accept touchscreen input for soft-pointer navigation.
- [ ] Persist battery SRAM (32 KiB) to cartridge.
- [ ] Audio: produce PCM output audible on DS hardware (headphone jack).
- [ ] Settings overlay (F1-equivalent) drawn on the touch screen.

## Non-Functional
- [ ] Must compile inside `devkitpro/devkitarm` Docker image.
- [ ] Must run on melonDS or DeSmuME for pre-hardware testing.
- [ ] Deterministic: same ROM + same input → identical frame output.
- [ ] Fail-closed: unknown execution paths must abort with a diagnostic, not
  silently produce wrong output.
- [ ] No dynamic recompilation, no JIT, no interpreter fallback.

## Constraints
- Target: Nintendo DS (ARM9 @ 67 MHz, ARM7 @ 33 MHz, 256 KB BWRAM, 128 KB IWRAM,
  96 KB VRAM-A/B, 128 KB cartridge SRAM).
- Toolchain: devkitPro devkitARM (ARM9 `arm-none-eabi-gcc`).
- Build: Docker (devkitpro/devkitarm image), CMake or Makefile.
- ROM: externally supplied, legally obtained SimCity (USA).sfc (512 KB LoROM).