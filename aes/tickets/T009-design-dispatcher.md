---
ticket: T009
title: Design DS-native dispatcher with reduced memory footprint
sprint: sprint-01
priority: high
status: pending
created: 2026-10-10
---

# T009 — Design DS-native dispatcher

## Context
T001 was BLOCKED because the upstream static-recomp dispatch (35 MB unstripped,
9 MB stripped) exceeds the DS ARM9 3.5 MB hard limit by ~2.5× even when
stripped. The v35 shards alone are 25 MB.

The Never-Do rule prohibits introducing an interpreter/JIT/dynamic recompiler.
The solution is to design a purpose-built dispatcher for the DS that:
1. Is significantly smaller than the full static-recomp dispatch.
2. Covers the same execution paths needed for SimCity gameplay.
3. Preserves the fail-closed guarantee for unknown paths.

## Acceptance Criteria
- [ ] Total compiled dispatch + core < 3 MB (leaving 500 KB for runtime state).
- [ ] Covers the same instruction contexts exercised during a full game session.
- [ ] Unknown execution paths trigger a fail-closed diagnostic.
- [ ] Frame output matches upstream SHA-256 for the same ROM + input sequence
      (within DS resolution differences).

## Scope
**In scope:**
- Profiling the upstream core to identify the most-commonly-executed contexts.
- Designing a compact dispatcher that covers those contexts.
- Keeping the fail-closed guard for uncovered paths.
- Writing the DS frontend (T003) once the dispatcher is ready.

**Out of scope:**
- Modifying upstream generated shards (T001's generated code).
- Full APU/SMP reproduction (deferred to audio subsystem).
- Widescreen support (infeasible on DS).

## Notes
The upstream project already has `tools/prune_shadowed_contexts.py` which
identifies unused context entries. Running this with aggressive pruning might
reduce the shard count sufficiently — this should be investigated first before
designing a new dispatcher from scratch.