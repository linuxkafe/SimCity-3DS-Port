# SD-PROCESS-004 — Per-frame instruction tracing (T025 step 1)

## Claim

Per-frame SNES instruction counts can be traced via `simcity_recomp_instruction_count()`
diff, and the result can be surfaced through both the trace log (every 10 frames)
and the `state` verb (`instr_frame` field). This enables T025 step 1: confirming
whether the interpreter dispatch is stable or growing over time.

## Evidence

Code added to `frontend/3ds/3ds_main.c`:

```c
/* Per-frame instruction counting (T025 step 1) */
uint64_t instr_now = simcity_recomp_instruction_count(g_recomp);
if (g_frame_count > 0) {
    g_instr_per_frame = instr_now - g_last_instr_count;
    if (g_frame_count % 10 == 0)
        ds3_trace("[T5] instr/frame #%u: %llu",
                  (unsigned)g_frame_count, (unsigned long long)g_instr_per_frame);
}
g_last_instr_count = instr_now;
g_frame_count++;
```

Added to `state` verb format string: `instr_frame=%llu`

The `instr_frame` field in `state` reports the instruction delta from the previous
frame. If stable (~12,000 SNES instructions/frame), the interpreter dispatch is
consistent. If growing, the recompiler is re-translating blocks.

## Hostile audit

- `g_frame_count` (volatile int) was reused for per-frame tracking. It's declared
  at line 180 as `volatile int g_frame_count = 0;` for frame limiting. The reuse
  is safe because `g_frame_limit` is never checked against `g_frame_count` in
  the main loop — it's dead code. This should be cleaned up in a future patch.
- The trace log on the emulated SD card only syncs to the host when the app
  exits cleanly. Crashes before flush = lost trace. `ds3_trace` calls `fflush`
  but the file system layer in Azahar may buffer at its own level.

## Source chunks

- `frontend/3ds/3ds_main.c:178-187` — static vars and declaration
- `frontend/3ds/3ds_main.c:349-362` — per-frame instruction delta computation
- `frontend/3ds/3ds_main.c:496-513` — state verb format string update
- `static-recomp/src/simcity_static_recomp.c:123` — `static_cpu_step` (interpreter)
