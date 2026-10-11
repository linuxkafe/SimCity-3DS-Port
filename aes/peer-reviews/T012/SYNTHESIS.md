# Synthesis: T012 (SimCity 3DS rendering fix + its delivery artifacts)

Rubric: `aes/peer-reviews/T012/rubric.md`, hash `3ae95ef4fae55c376f7b20fa2aa8f0391501d1713b58d49689c20c982d35239c`
Mode: **Multi-Perspective Review** (fallback — see "Declared limitations")
Moderator role: **VERIFIER**. Personas: REVIEWER.

## Persona findings (as submitted)

| Persona | BLOCKER | MAJOR | MINOR |
|---------|---------|-------|-------|
| CÍNICO | 3 | 2 | 0 |
| PURISTA | 2 | 3 | 0 |
| PRAGMÁTICO | 2 | 3 | 0 |
| UTILIZADOR | 2 | 3 | 0 |

## Merged findings

| ID | Finding | Type | Personas | Criterion | Closure |
|----|---------|------|----------|-----------|---------|
| F01 | Every delivered `*.3dsx` is a raw ELF, not a 3DSX container; no packager step exists | BLOCKER | C+P+Pr+U (4/4) | C-01 | C-01 loop prints no FAIL; magic = `33445358` |
| F02 | `status: done` with 0/6 acceptance criteria checked and `aes/verification/` empty | BLOCKER | C+P+Pr+U (4/4) | E-01 | `aes/verification/T012/` non-empty **and** boxes 16-21 all `[x]` |
| F03 | `gfxGetFramebuffer` cached in init while `gfxSwapBuffers` runs every frame | BLOCKER/MAJOR — **divergence preserved** | C+P+Pr+U (4/4) | C-06 | call appears inside `ds3_render_frame` |
| F04 | No 3DS install instructions anywhere; README documents only DS/melonDS | MAJOR (was BLOCKER for one persona — divergence preserved) | Pr+U (2/4) | U-02 | delivery doc names one artifact, its format, and its loader |
| F05 | `CLAUDE.md` documents `make check`; no `check:` rule exists (silent exit 0) | MAJOR | P+U (2/4) | U-01 | `^check:` exists **and** `make check` exits non-zero on failure |
| F06 | 11 experiment artifacts + 3 abandoned linker scripts in root; `.gitignore` covers none of `*.cia`/`3dsx_*.ld` | MAJOR | all 4 | D-01 | artifact count 0 **and** `.gitignore` matches `*.cia|*.ld` |
| F07 | `Tag_CPU_arch: v7` on the delivered artifact | **DOWNGRADED MAJOR → MINOR** | C (sole reporter) | E-04 | `readelf -A` reports `v6K` **and** `3dsx_*.ld` count 0 |
| F08 | `row_pixels` clamped to 256 but not against `g_top_width - offset_x`; visual skew | MAJOR | Pr (sole reporter) | C-05 | clamp present + on-device print of `g_top_width/height` |

## Divergences preserved

1. **F03 severity.** CÍNICO = BLOCKER, Purista/Pragmático/Utilizador = MAJOR. Not averaged.
   CÍNICO's mechanism: because swap rotates buffers, from frame 2 the cached pointer names
   the *presented* buffer, so the user keeps seeing frame 0. The others read it as a
   correctness/tearing defect of lesser weight. The closure condition is identical either way,
   so the divergence does not block resolution — but it is recorded, not resolved by fiat.
2. **F04 severity.** PRAGMÁTICO = BLOCKER, UTILIZADOR = MAJOR. Recorded as MAJOR (documentation
   gap, not a build defect) with the divergence noted.
3. **F07 mechanism — REJECTED BY THE VERIFIER.** CÍNICO reported that the binary "is built for
   ARMv7 and cannot execute on ARMv6K hardware." Executed evidence contradicts the mechanism:

   ```
   readelf -A build/simcity.3dsx            -> Tag_CPU_arch: v7,  Tag_CPU_name: "7-A"
   readelf -A 3dsx_crt0.o                   -> Tag_CPU_arch: v6K, Tag_CPU_name: "MPCore"
   objdump -d build/simcity.3dsx | grep -cE
     '\b(ldrex|strex|movw|movt|itte|ittt|cbz|cbnz|ldrd|strd)\b'   -> 0
   ```

   **Zero** ARMv7-only instructions exist in the emitted code. The `v7` tag is an
   `.ARM.attributes` metadata artifact of merged inputs, not evidence of unexecutable code.
   The crash is already fully explained by F01 — the loader never reaches instruction
   execution at all, because the file is not a container. The "cannot execute ARMv7" story
   is the claim this review most needed to kill, and it does not survive one disassembly.
   Retained only as MINOR hygiene.

4. **Sampled claim no persona flagged.** The human script deliberately probes the ARMv7 story
   even though only one persona raised it, so that a future reader repeating it finds the
   counter-evidence in the same place.

## Verdict

| Condition | Value |
|-----------|-------|
| BLOCKER | 2 (F01, F02) + 1 unresolved-divergence BLOCKER (F03) |
| MAJOR | 5 (F04, F05, F06, F08, F07-downgraded) |
| MINOR | 1 |
| Human validation script executed by an independent human | **NO** |

**VERDICT: REJECT**

Rationale, in the order the rule table forces it:

- `≥1 BLOCKER` caps the verdict at MAJOR-REVISIONS at best. F01 and F02 are BLOCKER and are
  undisputed (4/4 personas). The port's central claim — a runnable 3DS deliverable — is
  false as built.
- `Human script not executed → REJECT` applies independently and unconditionally. A
  moderator dry-run is not the human execution the protocol requires, and the executor must
  not be the T012 maintainer. Until a non-author runs
  `aes/peer-reviews/T012/human-validation.sh` and archives the output, this round **cannot**
  verdict ACCEPT regardless of how the BLOCKERs are resolved.
- No downgrade was applied. The absence of a `check:` rule (F05) means the green signal the
  project trusted does not exist; that is a fact about the gate system, not an opinion.

## Consequence for the standing narrative

`aes/kanban.md` records **T012 [COMPLETE]** and its Learning History asserts a rendering fix.
Both claims are **NÃO-SUPORTADA**. T012 must be reverted to an open state. The kanban text
"removed per-row gfxSwapBuffers that caused triplicated image" also misdescribes the code:
there is exactly one `gfxSwapBuffers` call, at `3ds_render.c:93`, after the row loop — the
ticket's stated defect does not exist in the tree in the form it claims to have fixed.

## Declared limitations

1. **Independence is simulated, not epistemic.** All four personas ran as fresh subagents on
   one model family. Per `MULTI_PERSPECTIVE_REVIEW.md` §7.1, they share training culture and
   converge; this is not the ≥2-independent-reviewers condition of `PEER_REVIEW.md` §4.
2. **Fleet isolation was unavailable.** `deckaf` reports both nodes NOT READY
   (`model API unreachable (HTTP 000000)`), so cross-node isolated personas could not be
   used. Isolation was therefore intra-process only.
3. **Findings are NÃO-VERIFICÁVEL until the human script runs.** The VERIFIER notes in
   `MODERATOR-NOTES.md` are the moderator's own execution, which grounds SUPORTADA for the
   reviewer findings but does not satisfy §7's requirement for an independent human.

## Next actions

1. Revert T012 to an open state and correct the kanban claim (F02).
2. Run `human-validation.sh` with an executor who is not the T012 maintainer; archive output here.
3. Only then address F01 — without a container, F03/F08 are untestable on device.