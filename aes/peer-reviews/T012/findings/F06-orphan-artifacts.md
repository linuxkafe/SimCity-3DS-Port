type: MAJOR
title: 11 experiment artifacts + 3 abandoned linker scripts in repo root; .gitignore excludes none of the CIAs or .ld files
personas: all 4 (severity MAJOR; cinico counts 14 incl. linker scripts)
evidence: 'ls test_*.c test_*.3dsx *.cia 2>/dev/null | wc -l' -> 11; 'ls 3dsx_patched.ld 3dsx_armv6k.ld 3dsx_original.ld 2>/dev/null | wc -l' -> 3. simcity.cia (13 MB), simcity_full.cia (13.6 MB), test_minimal/fixed/v6/v6_debug .c+.3dsx, 3dsx_*.ld. grep -c cia .gitignore -> 0; .gitignore lists simcity.3dsx but not *.cia/test_*/3dsx_*.ld. No CMake target or ticket references them.
closure-condition: 'ls test_*.c test_*.3dsx *.cia 3dsx_*.ld 2>/dev/null | wc -l' returns 0 AND 'grep -qE "\*\.cia|\*\.ld" .gitignore' matches.
rubric-criterion: D-01
