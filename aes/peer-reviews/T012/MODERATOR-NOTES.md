# Moderator Verification Notes (VERIFIER role)

Role: VERIFIER (execution access) — PEER_REVIEW.md §4.
The four personas were REVIEWERs (reasoning only). These notes are the verifier
pass that converts NÃO-VERIFICÁVEL reviewer findings into typed verdicts.

Rubric hash verified before synthesis: 3ae95ef4fae55c376f7b20fa2aa8f0391501d1713b58d49689c20c982d35239c

## Findings I executed and confirmed SUPORTADA

- F01 (C-01): confirmed. 5/5 .3dsx are 7f454c46 (ELF); no 3dsxtool/makerom in CMakeLists or Makefile.
- F02 (E-01): confirmed. aes/verification/ empty; T012-render-fix.md:6 status:done; boxes 16-21 all unchecked.
- F03 (C-06): confirmed via upstream doc. libctru /opt/devkitpro/libctru/include/3ds/gfx.h:140
  "Please remember that the returned pointer will change every frame if double buffering is enabled."
  Code fetches once in ds3_render_init (3ds_render.c:37-38) and calls gfxSwapBuffers every frame (:93). CONFIRMED.
- F05 (U-01): confirmed. No check: rule in Makefile; 'make -n check' is a silent no-op exit 0.
- F06 (D-01): confirmed. 11 orphan files + 3 linker scripts; .gitignore excludes none of *.cia/3dsx_*.ld.

## Divergence: F07 severity DOWNGRADE (I do not pass it through as written)

F07 claims the delivered binary "is built for ARMv7 ... cannot execute on ARMv6K hardware."
My verification:
  - readelf -A build/simcity.3dsx -> Tag_CPU_arch: v7, Tag_CPU_name "7-A"   (tag present)
  - readelf -A 3dsx_crt0.o        -> Tag_CPU_arch: v6K, Tag_CPU_name MPCore   (input is v6K)
  - objdump -d build/simcity.3dsx | grep -cE '(ldrex|strex|movw|movt|itte|ittt|cbz|cbnz|ldrd|strd)' -> 0
    i.e. ZERO ARMv7-only instructions in the emitted code stream.
Conclusion: the v7 ARM.attributes tag is a metadata artifact (likely from libctru/libstdc++
object attrs merged at link), NOT evidence that the 3DS CPU cannot execute the code. The
crash is already fully explained by F01 (raw ELF, never containerised) — the loader never
gets to execute ARM code at all. F07 as stated is NÃO-VERIFICÁVEL/contradicted; I retain it
as MINOR hygiene (clean the abandoned 3dsx_*.ld and re-check arch after F01 is fixed) and
explicitly reject the "cannot execute ARMv7 code" mechanism.

## Divergence: F03 severity (Cínico BLOCKER vs others MAJOR) — PRESERVED, not averaged

All four personas found the cached framebuffer pointer; they disagreed on severity.
I keep both readings visible rather than merging to consensus: Cínico argues the swap
makes the cached pointer the *presented* buffer from frame 2 (user sees a stale frame);
the others read it as a correctness/tearing defect of MAJOR weight. The finding stands
regardless of which severity is correct; the CLOSURE CONDITION is identical for both.

## Divergence: F04 severity (BLOCKER vs MAJOR) — PRESERVED

Pragmático rates missing install instructions BLOCKER; Utilizador rates MAJOR. The defect
is the same (README documents only the DS/melonDS path inside a 3DS delivery). Kept BLOCKER
is NOT applied; recorded as MAJOR because it is a documentation gap, not a build defect —
but the divergence is preserved, not resolved by fiat.
