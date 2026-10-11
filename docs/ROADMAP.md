# ROADMAP — SimCity DS Port

## Sprint 01 — Port Core & DS Frontend
| Ticket | Title | Priority | Status |
|--------|-------|----------|--------|
| T001 | Port static-recomp core to ARM11/DS-Nitro | high | in-progress |
| T002 | Adapt PPU rendering to DS VRAM geometry | high | pending |
| T003 | Replace SDL2 frontend with NitroSDK DS frontend | high | pending |
| T004 | Cartridge SRAM persistence | medium | pending |
| T005 | Audio output path (ARM7 co-processor or ARM9 software) | high | pending |

## Sprint 02 — Emulator Testing & Polish
| Ticket | Title | Priority | Status |
|--------|-------|----------|--------|
| T006 | melonDS/DeSmuME test harness | high | pending |
| T007 | Settings overlay on touch screen | medium | pending |
| T008 | Input mapping & soft-pointer | medium | pending |

## Backlog
- [DISCOVERED mid-task] Widescreen (398x239) is likely infeasible on DS top screen
  (256x192 native). Consider letterboxed 256x192 or cropped 256x160 + HUD.
- [DISCOVERED mid-task] 32,040 Hz stereo PCM at 16-bit may exceed DS ARM9 audio
  budget; investigate ARM7 co-processor offload or downsampling.