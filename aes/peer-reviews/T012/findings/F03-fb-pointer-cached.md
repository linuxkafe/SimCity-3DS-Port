type: BLOCKER
title: gfxGetFramebuffer cached once in init while gfxSwapBuffers runs every frame; write target is the presented buffer from frame 2
note: severity divergence preserved in SYNTHESIS.md — Cinico BLOCKER, Purista/Pragmatico/Utilizador MAJOR; taken as BLOCKER (conservative), closure identical
evidence: grep -n gfxGetFramebuffer frontend/3ds/3ds_render.c -> lines 37-38 only, both inside ds3_render_init(), stored to statics g_top_fb/g_bottom_fb (28-29) and never refreshed. gfxSwapBuffers() at 3ds_render.c:93 runs every ds3_render_frame(). libctru gfx.h:140 "Please remember that the returned pointer will change every frame if double buffering is enabled." T012-render-fix.md:19 promises "Single framebuffer update per VBlank".
closure-condition: grep -n gfxGetFramebuffer frontend/3ds/3ds_render.c shows the call inside the body of ds3_render_frame (after the write loop at line 85), not only in ds3_render_init.
rubric-criterion: C-06
