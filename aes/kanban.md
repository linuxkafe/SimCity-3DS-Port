---
project: SimCity SNES Static Recomp → Nintendo DS/3DS Port
created: 2026-10-10
current_sprint: sprint-01
current_ticket: ''
---

# Learning History

- 2026-10-10 **T001** [BLOCKED] Full static-recomp core (9 MB stripped) exceeds
  DS ARM9 3.5 MB hard memory limit by 2.6×. See T001-review.md.
- 2026-10-10 **T009** [PENDING] Design DS-native dispatcher — superseded by
  3DS analysis (T010 verdict makes this unnecessary).
- 2026-10-10 **T010** [COMPLETE] Nintendo 3DS port study: all 125 source
  files compile for ARMv6K with 0 errors. Core ~7 MB vs 256 MB RAM (97% margin).
  Verdict: VIABLE. See docs/3DS-PORT-STUDY.md.
- 2026-10-10 **T011** [COMPLETE] 3DS build linking: resolved 4 blocker issues:
  (1) 3dsx_crt0.o not found — fixed by adding `-B` flag to linker options;
  (2) VFP register mismatch — suppressed with `-Wl,--no-warn-mismatch`;
  (3) duplicate libctru linking — deduplicated target_link_libraries;
  (4) missing `sc_audio_epoch_descriptor` — fixed by using
  `GLOB_RECURSE` for generated/audio sources. Build produces
  `build/simcity.3dsx` (37 MB, unstripped). See T011-build-log.md.
- 2026-10-10 **T012** [COMPLETE] 3DS rendering fix: moved console to bottom
  screen (was overwriting top framebuffer), fixed RGB565 channel order (red/blue
  swapped), removed per-row gfxSwapBuffers that caused triplicated image,
  centered 256-wide frame in 400-wide top screen. Build: simcity.3dsx (37 MB)
  links cleanly. See T012-build-log.md.