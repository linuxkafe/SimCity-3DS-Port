---
project: SimCity 3DS Port
created: 2026-10-20
current_sprint: sprint-01
current_ticket: T025
---

# Kanban — SimCity 3DS Port

## Backlog
| ID | Title | Priority |
|----|-------|----------|
| T032 | Deterministic per-frame hashes | high |
| T033 | Automated paired A/B harness with bounds | high |
| T039 | Pin the Deck's Azahar configuration | high |
| T040 | `svc=0x32` kills the debug port | medium |
| T044 | Extract the audio drain into a host-testable function | medium |
| T046 | Settle T023's contradictory frame-rate baseline on hardware | high |
| T047 | 30 fps target: feasibility, decomposition, first move | high |
| — | Verify output equality on hardware | high |

## Sprint 01 — Measurement Infrastructure
**Goal**: Establish deterministic frame verification and measurement discipline

| ID | Title | Status |
|----|-------|--------|
| T032 | Deterministic per-frame hashes | pending |
| T033 | Automated paired A/B harness with bounds | pending |

## In Progress
| ID | Title | Status |
|----|-------|--------|
| T024 | Colour & audio diagnosis | in-progress (unblocked by T038) |
| T025 | Core perf deep-dive (static recompiler) | in-progress (unblocked by T038) |

## Done
| ID | Title | Completed |
|----|-------|-----------|
| T041 | Headless Azahar fork — build from source, no Qt | 2026-10-21 |
| T043 | The debug bridge's blocking accept froze the guest | 2026-10-21 |
| T042 | Audio drain loop underflow hung the port | 2026-10-21 |
| T038 | T025 build could not reach the main loop | 2026-10-21 |
| T037 | Restore the control; find the abort | 2026-10-19 |
| T036 | Drift-detecting measurement harness | 2026-10-19 |
| T035 | Recover a reproducible good build | 2026-10-19 |
| T031 | Headless emulator environment (Azahar, Steam Deck) | 2026-10-17 |
| T029 | Make `analyze()` measurable | 2026-10-17 |
| T023 | Performance profiling — core=92.7% of loop, 0.27 fps | 2026-10-20 |

## Blocked
| ID | Title | Blocker |
|----|-------|---------|
| — | Verify output equality on hardware | 3DS at 10.0.0.130 unreachable |
