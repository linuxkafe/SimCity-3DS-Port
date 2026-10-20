# T025 — Core Performance Deep-Dive (Static Recompiler)

**Status:** NEW
**Priority:** HIGH
**Blocker for:** Playable framerate on 3DS

## Background

T023 profiling shows the core consumes 92.7% of the frame budget:
- `core = 3,407,572 µs` per frame
- `loop = 3,674,675 µs` per frame
- **Effective framerate: 0.27 fps** (target: 30 fps)

The gap between expected and actual performance is ~100×. This ticket investigates whether the static recompiler is:
1. Actually generating native code, or just interpreting via switch-dispatch
2. Doing redundant work (re-translating, re-analyzing)
3. Blocked on something that could be parallelized

## Hypothesis (SD-META-003)

`sc_v11_cpu_step` is a **switch-dispatch interpreter** with one case per 1024-byte block:
```c
switch(address>>10u){
case 0x20u: result=sc_v11_cpu_group_00020(r,address,m,x); break;
...
}
```

Each SNES instruction costs ~282 µs of 3DS time to dispatch through this table.
That's 1000× slower than native ARM execution.

## Investigation Steps

### Step 1: Confirm interpreter vs JIT
- [ ] Add trace point: when does `SC_V11_V02_GLOBAL_INSTRUCTIONS` expire?
- [ ] Count how many times `static_cpu_step` calls `sc_v11_bootstrap_step` vs `sc_v11_cpu_step`
- [ ] Check if there's any native code generation path (`sc_v11_cpu_group_*` functions)

### Step 2: Measure per-instruction overhead
- [ ] Instrument `sc_v11_cpu_step` to count instructions per tick
- [ ] Compare `instructions` from `state` verb vs `master` clock delta
- [ ] Calculate: cycles/SNES-instruction = `loop_ticks / instructions_delta`

### Step 3: Identify hot paths
- [ ] Which `sc_v11_cpu_group_*` functions are called most frequently?
- [ ] Is there a hot loop in the SNES ROM that the interpreter handles poorly?
- [ ] Check if `sc_v11_trace_cpu_instruction` is a no-op when logging is disabled

### Step 4: Check for missed optimizations
- [ ] Is `SIMCITY_FRAME_INSTRUCTION_GUARD` (50,000) being hit? If so, frames are being truncated.
- [ ] Is `static_cpu_step` being called for every SNES instruction, or can blocks be batched?
- [ ] Check if `sc_v11_before_instruction` does anything expensive

## Expected Outcome

Either:
- The static recompiler IS generating efficient native code but the 3DS is too slow → need hardware upgrade or algorithmic optimization
- The static recompiler is NOT generating native code → need to fix the recompiler or use a different emulation strategy

## Files

- `frontend/3ds/3ds_main.c:315-344` — core phase timing
- `static-recomp/src/simcity_static_recomp.c:123` — `static_cpu_step`
- `static-recomp/generated/sc_v11_cpu_dispatch_index.c:23` — `sc_v11_cpu_step` switch dispatch
- `aes/shadow/SD-META-003.md` — interpreter vs JIT analysis
