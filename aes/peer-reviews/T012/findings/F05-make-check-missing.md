type: MAJOR
title: CLAUDE.md documents `make check` but the Makefile has no check: rule; .PHONY declares it, making it a silent no-op success
personas: purista+utilizador (2/4)
evidence: CLAUDE.md:15 "make check - run tests". 'grep -qE "^check:" Makefile' -> no match (rules at 13,17,36,45,56,69,84,87,92,110,132,135). Makefile:9 lists check in .PHONY with no rule -> 'make -n check' prints "Nothing to be done for 'check'" and exits 0. Real test targets are ds-test-headless (56) and 3ds-test-headless (69).
closure-condition: 'grep -qE "^check:" Makefile' succeeds AND 'make check' exits non-zero when 3ds-test-headless fails (or CLAUDE.md no longer names `make check`).
rubric-criterion: U-01
