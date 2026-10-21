---
ticket: T039
title: Pin the Deck's Azahar configuration so the SD-card stall cannot silently return
sprint: sprint-01
priority: high
status: pending
created: 2026-10-21
tier: light
depends_on: [T038]
---

# T039 — Pin the Azahar configuration

## Context

T038 established by measurement that the Deck's Azahar configuration decides
whether the 3DS can read its ROM at all:

| `sdmc_directory` | `use_custom_storage` | Result |
|---|---|---|
| `/run/media/mmcblk0p1/Emulation/storage/azahar/sdmc/` | `true` | **stalls** — the app never reaches the main loop |
| `/home/deck/.local/share/azahar-emu/sdmc/` | `false` | **works** |

The failing configuration is what EmuDeck's reference config specifies:
`~/.config/EmuDeck/backend/configs/azahar/qt-config.ini`. It is not the
live config, but any tool that copies the reference over the live config
reintroduces the stall — and the symptom (a 70-byte trace file) does not
name the cause.

Azahar rewrites `use_custom_storage` to `false` on every start, so editing
the live config by hand does not stick.

## Acceptance Criteria

- [ ] The working configuration is recorded in `docs/` with the exact
      `sdmc_directory`, `nand_directory`, `use_custom_storage` and
      `use_virtual_sd` values
- [ ] A check exists that detects the failing configuration before a
      measurement run, and fails loudly
- [ ] The check is exercised by a test that demonstrates it catching the
      EmuDeck reference configuration
- [ ] `docs/` states which configuration reproduces the stall, so the
      next person does not have to rediscover it
- [ ] The Deck's SD directory that the emulator actually mounts is
      documented, including the fact that a symlink of the same name in
      another directory is not visible to the emulator

## Notes

This is deliberately a small ticket. The value is entirely in making the
failure *recognisable*, not in fixing anything new — T038 already fixed it.

The check should be cheap enough to run before every measurement, e.g. read
`~/.config/azahar-emu/qt-config.ini` and compare the storage keys against
the known-good values.
