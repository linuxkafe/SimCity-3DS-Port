type: BLOCKER (pragmatico) / MAJOR (utilizador)
title: No 3DS install instructions exist anywhere; README documents only the DS/melonDS path
personas: pragmatico+utilizador (2/4)
evidence: grep -rni 'luma|azahar|cia|install' README.md CLAUDE.md aes/tickets/T012*.md -> no 3DS installation content. README.md:26-30 says 'cp build/simcity.nds' to melonDS (a DS artifact) inside a 3DS ticket's delivery. simcity.cia / simcity_full.cia / simcity.smdh exist in root and are named in no document. Reader must guess which of .3dsx/.cia/.nds is the deliverable and which loader accepts it.
closure-condition: A delivery doc names exactly one installable artifact, states its format (3DSX container vs raw ELF vs CIA), and states which of Luma/Azahar/FBI accepts it, plus the SD path.
rubric-criterion: U-02
