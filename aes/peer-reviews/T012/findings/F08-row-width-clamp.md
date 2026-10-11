type: MAJOR
title: Row width clamped to hardcoded 256 but offset_x computed from g_top_width without clamping the pair; skews if g_top_width < 256+offset
personas: pragmatico (sole reporter)
evidence: frontend/3ds/3ds_render.c:70 'if (row_pixels > 256u) row_pixels = 256u;' and :74 computes offset_x from g_top_width, but nothing enforces offset_x + row_pixels <= g_top_width. Comments at :57,:73 assume 400x240. memset at :78 uses g_top_width*g_top_height so no overrun; failure mode is visual skew, not crash.
closure-condition: a clamp 'row_pixels <= g_top_width - offset_x' (or equivalent) is present, and an on-device print of g_top_width/g_top_height confirms the values the code assumes.
rubric-criterion: C-05
