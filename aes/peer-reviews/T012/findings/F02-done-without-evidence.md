type: BLOCKER
title: Ticket is status:done with all 6 acceptance criteria unchecked and aes/verification/ empty
personas: cinico+purista+pragmatico+utilizador (unanimous, 4/4)
evidence: aes/tickets/T012-render-fix.md:6 'status: done' but lines 16-21 are all '- [ ]'. 'ls aes/verification/ | wc -l' -> 0. T012-build-log.md:62-64 concedes "Headless test: Not yet run for 3DS platform". T012-learn.md:40 "we cannot verify frame output deterministically". grep -cE 'grep|`|readelf|od ' aes/tickets/T012-render-fix.md -> 0 (no cited command anywhere).
closure-condition: 'ls aes/verification/T012/ | wc -l' >= 1 containing a headless/device run log, AND all six boxes at T012-render-fix.md:16-21 read '- [x]' or status reverts from done.
rubric-criterion: E-01
