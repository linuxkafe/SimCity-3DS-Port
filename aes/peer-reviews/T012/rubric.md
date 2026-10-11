---
rubric-id: SIMCITY-3DS-T012-v1
candidate: T012-render-fix (+ the 3DS delivery artifacts it produced)
created: 2026-10-10
mode: multi-perspective (single model family; reviewers isolated via deckaf)
deadline: 2026-10-11
dimensions:
  - correctness
  - security
  - coherence
  - debt
  - reproducibility
  - usability
---

# Review Rubric — SIMCITY-3DS-T012-v1

Scope of candidate: the claim that T012 ("3DS rendering fix") is `done`, together with
the 3DS build/link artifacts and the delivery instructions produced while investigating
the 3DS crash. The candidate is the set of claims, not the code alone.

Roles (PEER_REVIEW.md §4): reviewers convened here are REVIEWER (reasoning only, no
execution access); the moderator is VERIFIER and supplies command output as evidence.
Findings from REVIEWER personas are NÃO-VERIFICÁVEL until the human validation script runs.

## D1 correctness

| ID | Criterion | Verifiable Check |
|----|-----------|-----------------|
| C-01 | Every file named `*.3dsx` is a 3DSX container (magic `3DSX`), not a raw ELF | `for f in $(find . -name '*.3dsx' -not -path './devkitpro/*'); do [ "$(od -An -tx1 -N4 "$f"\|tr -d ' \n')" = "33445358" ] \|\| echo "FAIL $f"; done` prints no `FAIL` line |
| C-02 | T012's code claims match the code (one swap per frame, not per row) | `grep -c gfxSwapBuffers frontend/3ds/3ds_render.c` returns 1 |
| C-05 | Framebuffer stride is not assumed equal to screen width | `grep -n 'g_top_width \* 2\|g_top_width\*2' frontend/3ds/3ds_render.c` returns no match |
| C-06 | Framebuffer pointer is fetched per frame, not cached once (libctru may return a different pointer per frame under double buffering) | `grep -n 'gfxGetFramebuffer' frontend/3ds/3ds_render.c` shows the call inside `ds3_render_frame`, not only in `ds3_render_init` |

## D2 security

| ID | Criterion | Verifiable Check |
|----|-----------|-----------------|
| S-01 | No commercial ROM is committed in the tree | `find . -name '*.sfc' -not -path './devkitpro/*'` returns nothing |
| S-02 | `.gitignore` excludes build outputs and binaries | `grep -E '\.3dsx|\.cia|\.smdh|build' .gitignore` returns ≥1 match |

## D3 coherence

| ID | Criterion | Verifiable Check |
|----|-----------|-----------------|
| E-01 | The ticket marked `done` has archived verification evidence | `ls aes/verification/ \| wc -l` ≥ 1 |
| E-02 | Ticket claims cite a reproducible command or file:line | `grep -cE 'grep\|`\|readelf\|od ' aes/tickets/T012-render-fix.md` ≥ 1 |
| E-03 | Every ticket file declares a machine-readable `status:` | `for t in aes/tickets/T*.md; do grep -q '^status:' "$t" \|\| echo "FAIL $t"; done` prints no `FAIL` |
| E-04 | The claimed root cause is supported by the artifact it cites | `readelf -A build/simcity.3dsx \| grep Tag_CPU_arch` matches the value asserted in the ticket |

## D4 debt

| ID | Criterion | Verifiable Check |
|----|-----------|-----------------|
| D-01 | No orphaned experiment artifacts left in the repo root | `ls test_*.c test_*.3dsx *.cia 2>/dev/null \| wc -l` returns 0 |
| D-02 | No abandoned linker scripts left in the tree | `ls 3dsx_patched.ld 3dsx_armv6k.ld 3dsx_original.ld 2>/dev/null \| wc -l` returns 0 |
| D-03 | The Makefile provides the targets the project's own docs tell users to run | `for t in check docker-build doctor; do grep -qE "^$t:" Makefile \|\| echo "FAIL missing target: $t"; done` prints no `FAIL` |

## D5 reproducibility

| ID | Criterion | Verifiable Check |
|----|-----------|-----------------|
| R-01 | The build is re-runnable from the documented command in the container | `docker run … cmake … && make` yields `build/simcity.3dsx` without manual intervention |
| R-02 | The delivered artifact's identity is pinned by hash | `grep -qi sha256 aes/tickets/T012-build-log.md` matches |
| R-03 | Findings and evidence can cite immutable references | `git rev-parse --is-inside-work-tree` exits 0 |

## D6 usability

| ID | Criterion | Verifiable Check |
|----|-----------|-----------------|
| U-01 | `CLAUDE.md` documents only commands that exist | every target named in `CLAUDE.md` resolves via `grep -qE "^$t:" Makefile` |
| U-02 | The delivery instructions say how to install on 3DS and which format is expected | the instruction names 3DSX container vs raw ELF vs CIA and states which one Luma/Azahar accepts |

---

pre-registered-hash: (computed after creation)