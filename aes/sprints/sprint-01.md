---
sprint: sprint-01
period: 2026-10-20 → 2026-11-03
status: active
---

# Sprint 01 — Measurement Infrastructure

## Goal

Establish deterministic frame verification and measurement discipline so that
every subsequent optimisation can be validated automatically.

## Tickets

| ID | Title | Status |
|----|-------|--------|
| T031 | Headless emulator environment (Azahar, Steam Deck) | done |
| T029 | Make `analyze()` measurable | done |
| T023 | Performance profiling — core=92.7% of loop, 0.27 fps | done |
| T038 | T025 build could not reach the main loop | done |
| T024 | Colour & audio diagnosis | in-progress (unblocked) |
| T025 | Core perf deep-dive (static recompiler) | in-progress (unblocked) |
| T032 | Deterministic per-frame hashes | pending |
| T033 | Automated paired A/B harness with bounds | pending |
| T039 | Pin the Deck's Azahar configuration | pending |
| T040 | `svc=0x32` kills the debug port | pending |

## Retrospective

*Filled at end of sprint.*

### What went well
- Deck headless workflow stabilized: `ssh deck@steamdeck` + systemd service + Azahar
- aes_prof.py fixed (decimal parser, not hex); anlz toggle confirmed measurable (+4.3%)
- Full per-phase breakdown captured: core=92.7%, dbg=2.5%, present=2.2%, render=1.6%
- Framebuffer capture working (t024_fb.py, 192 KB BGR565)

### What went wrong
- ROM path with parentheses caused Azahar SD card mapping issues; switched to simple `simcity.sfc`
- Debug bridge `dbgk_init()` blocked on Wi-Fi check in emulator; nobridge build needed for initial testing
- aes_prof.py parsed state values as hex (wrong — `%u` prints decimal), silently corrupting all measurements
- Devkitpro MCP tool couldn't find source path in container; built via podman on Deck instead
- **T038:** the T025 build's failure to boot was read as a hang for hours. It was a *failed* init whose error only went to a console the detached launch discards. The trace file ended at a step that had *succeeded*. The real cause was the Deck's Azahar configuration, and the **Oct-18 gold control — which contains none of the new code — failed identically**. That control was available from the first hour and would have ruled the port out immediately.

### What to change next sprint
- T025: profile `simcity_recomp_advance` internals — the interpreter hypothesis is now testable against live instruction counts (measured 12 483 instr/frame mean, reproducible across runs)
- T026: Add title ID to SMDH at build time so the game launches without manual patching
- T024: Trace the audio path — why does `simcity_recomp_audio_available()` return 0?
- T039: pin the Azahar configuration so the SD-card stall cannot silently return
- T040: `svc=0x32` takes the debug port down, so `state` is unusable while the app runs
- Reduce dbg phase cost (2.5%) by making `dbgk_poll` more efficient or optional
- **Measure the control before the arm, every time.** Three separate root-cause claims in this project were wrong because the control was not held fixed.
