---
ticket: T013
title: "Every delivered *.3dsx is a raw ARM ELF, not a 3DSX container; no packager step exists"
phase: ticket
status: pending
sprint: backlog
priority: medium
created: '2026-10-11'
requires: []
produces: []
blocked_by: ''
---

# T013 — Every delivered *.3dsx is a raw ARM ELF, not a 3DSX container; no packager step exists

## Origin

Review finding from aes-peer-review.
- **Type:** BLOCKER
- **Evidence:** 'for f in $(find . -name "*.3dsx" -not -path "./devkitpro/*"); do od -An -tx1 -N4 "$f"|tr -d " \n"; done' -> 7f454c46 (\x7fELF) for all 5 files incl. build/simcity.3dsx; 'file build/simcity.3dsx' -> ELF 32-bit LSB executable ARM EABI5. CMakeLists.txt:302-305 only sets SUFFIX ".3dsx"; grep -rn '3dsxtool|makerom' CMakeLists.txt Makefile -> no match. Luma/Azahar loaders accept only the 3DSX container.
- **Rubric criterion:** C-01

## Closure Condition

'od -An -tx1 -N4 build/simcity.3dsx | tr -d " \n"' equals 33445358 AND the C-01 loop in aes/peer-reviews/T012/rubric.md prints no FAIL line for any *.3dsx outside devkitpro/.

This ticket is **done** when the closure condition above is verifiably met.
