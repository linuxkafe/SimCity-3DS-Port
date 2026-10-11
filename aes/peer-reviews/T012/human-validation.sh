#!/usr/bin/env bash
# Human Validation Script — SimCity 3DS / T012 peer review
# Executor: <a colleague; MUST NOT be the T012 author/maintainer>
# Date: __________
# Run from repo root. Each task is falsifiable in ~1 minute. Record PASS/FAIL per line.

set +e
fail=0
ok(){ echo "PASS: $1"; }
no(){ echo "FAIL: $1"; fail=1; }

# T1 — verify F01: is the shipped 3dsx actually a 3DSX container?
m=$(od -An -tx1 -N4 build/simcity.3dsx 2>/dev/null | tr -d ' \n')
[ "$m" = "33445358" ] && ok "F01 build/simcity.3dsx is a 3DSX container" || no "F01 build/simcity.3dsx magic=$m (expected 33445358=3DSX)"

# T2 — verify F05: does the documented gate actually exist and fail on failure?
grep -qE '^check:' Makefile && ok "F05 'make check' target exists" || no "F05 no check: target in Makefile (CLAUDE.md documents 'make check')"

# T3 — verify F02: is there any archived verification evidence for T012?
n=$(ls aes/verification/T012/ 2>/dev/null | wc -l)
[ "$n" -ge 1 ] && ok "F02 T012 has verification bundle ($n files)" || no "F02 aes/verification/T012/ empty or missing"

# T4 — verify F06: are the CIAs / linker scripts still sitting in the root?
k=$(ls test_*.c test_*.3dsx *.cia 3dsx_*.ld 2>/dev/null | wc -l)
[ "$k" -eq 0 ] && ok "F06 no orphan experiment artifacts" || no "F06 $k orphan artifacts remain in root"

# T5 — verify F03 against the upstream contract (libctru lives only in the container)
GFX_HDR=$(docker run --rm devkitpro/devkitarm cat /opt/devkitpro/libctru/include/3ds/gfx.h 2>/dev/null)
if printf '%s' "$GFX_HDR" | grep -q "pointer will change every frame"; then
  ok "F03 libctru confirms framebuffer pointer changes per frame"
else
  no "F03 could not confirm libctru framebuffer contract (docker unreachable?)"
fi

# T6 — sample a claim NO persona flagged: are there really zero ARMv7-only instructions?
if [ -f build/simcity.3dsx ]; then
  ARCH=$(docker run --rm -v "$PWD:/work" -w /work devkitpro/devkitarm \
    sh -c 'PATH=/opt/devkitpro/devkitARM/bin:$PATH; readelf -A /work/build/simcity.3dsx 2>/dev/null | grep "Tag_CPU_arch:"' 2>/dev/null)
  echo "INFO: artifact declares $ARCH  (a v7 tag here does NOT prove unexecutable code)"
  echo "INFO: run the disassembly scan in MODERATOR-NOTES.md before repeating the ARMv7 claim"
fi

echo "-----"
[ $fail -eq 0 ] && echo "SCRIPT RESULT: all sampled checks PASS" || echo "SCRIPT RESULT: at least one check FAILs"
exit $fail
