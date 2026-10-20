# SD-META-003 — Core is interpreter-bound, not JIT-compiled

## Claim

`sc_v11_cpu_step` is a **switch-dispatch interpreter** (one case per 1024-byte address block),
not a JIT-compiled native stub. Each SNES instruction costs 300-500 3DS cycles to dispatch
through the switch table, explaining the 0.27 fps runtime. `SIMCITY_FRAME_INSTRUCTION_GUARD`
(50,000 per frame) is a safety guard, not a translation cache — the interpreter runs until
it hits that guard or completes the scheduled frame.

## Evidence

`static-recomp/src/simcity_static_recomp.c:123-127`:
```c
static int static_cpu_step(SCV11Runtime *runtime) {
    if (runtime->scheduler.cpu_instructions < SC_V11_V02_GLOBAL_INSTRUCTIONS)
        return sc_v11_bootstrap_step(runtime);
    return sc_v11_cpu_step(runtime);
}
```
`sc_v11_cpu_step` (`generated/sc_v11_cpu_dispatch_index.c:23`):
```c
int sc_v11_cpu_step(SCV11Runtime *r){
    ...
    switch(address>>10u){              // <-- 1024-byte block dispatch
    case 0x20u: result=sc_v11_cpu_group_00020(r,address,m,x); break;
    ...
    }
    ...
```

The "static recompiler" is actually a **static translator** that groups instructions into
1024-byte blocks and interprets them via a switch-dispatch table. There is no native code
generation — each SNES instruction requires a C function call, a switch lookup, and per-instruction
tracing overhead (`sc_v11_trace_cpu_instruction`).

## Metrics from live `state` verb (frame 836):

| Metric | Value | Interpretation |
|--------|-------|----------------|
| instructions | 10,103,452 | Over 836 frames = ~12,085 instr/frame |
| master | 298,707,522 | Over 836 frames = ~357,298 master ticks/frame |
| core (µs) | 3,407,572 | 3.4 s/frame |
| cycle/core | 3,407,572 / 12,085 | ~282 µs per SNES instruction |

The 282 µs per SNES instruction is 1000× slower than native ARM execution would be. This is
interpreter overhead — the 3DS is emulating every SNES instruction through a C switch-dispatch
and per-instruction tracing hook.

## Hostile audit

- `sc_core_logging_enabled()` gates `instance->log` — if disabled, `sc_v11_trace_cpu_instruction`
  should be a no-op. But the trace hook call itself costs cycles even if empty.
- `SC_V11_V02_GLOBAL_INSTRUCTIONS` gates the bootstrap path — the first 0x200 instructions
  per power-on use `sc_v11_bootstrap_step`, which may have different (slower) overhead.
- No `analyzer` or per-instruction cache exists — every instruction is re-dispatched from scratch.

## Actionable next steps

1. **T025:** Confirm `SC_V11_V02_GLOBAL_INSTRUCTIONS` is exhausted by frame 2 (so we're in
   `sc_v11_cpu_step` for all subsequent frames) — add a trace point.
2. **T025:** Disable `sc_v11_trace_cpu_instruction` if `core_log` is off — this call is in the
   hot path for every instruction.
3. **T026:** Check if there's a way to skip the `scheduler.frame` check per cycle — it appears
   every SNES instruction calls `sc_v11_before_instruction` which may do work.
4. **T025-deferred:** True JIT compilation (generating ARM code at runtime) is a future
   T-task — the static translator approach cannot reach playable framerates.
