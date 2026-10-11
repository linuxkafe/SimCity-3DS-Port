# VISION — SimCity DS Port

## Problem
SimCity SNES Static Recomp 1.4.0 is a fully static recompilation of the original
SimCity (USA) SNES ROM. It runs natively on x86_64 Windows/Linux. The goal is to
produce a playable port for the Nintendo DS, reusing as much of the generated
static dispatch as possible, while adapting the PPU, audio, and frontend to DS
hardware constraints.

## Solution
- Reuse the generated C dispatch (v11/v34/v35/v36 shards) unchanged where possible.
- Adapt the PPU rendering path (sc_v28_video.c) to DS VRAM/BWRAM geometry.
- Replace the SDL2 frontend with a NitroSDK-based DS frontend (both screens,
  touchscreen, cartridge SRAM).
- Build inside a Docker container (devkitpro/devkitarm) so the host needs only
  Docker.
- Test on an emulator (melonDS / DeSmuME) before any physical cartridge.

## Value Proposition
A native, deterministic, fail-closed SimCity experience on the Nintendo DS,
preserving the static-recomp certification guarantees of the original project.