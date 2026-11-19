# Transition: float32 → float16 / bfloat16

Facts of the type pair for `"fp32-to-fp16"`. Half precision is a storage and
throughput format with three decimal digits. It rarely replaces single
precision wholesale in numerical simulation; it replaces it *selectively*.
Set expectations accordingly at the interview.

## The Formats

| | float32 | float16 (binary16) | bfloat16 |
|---|---|---|---|
| significand bits (with the implicit one) | 24 | 11 | 8 |
| machine epsilon | 1.2e-7 | 9.8e-4 | 7.8e-3 |
| decimal digits | 6–7 | 3 | 2 |
| largest finite | 3.4e38 | 65 504 | 3.4e38 |
| smallest normal | 1.2e-38 | 6.1e-5 | 1.2e-38 |
| integers exactly representable up to | 16 777 216 | 2 048 | 256 |

- **float16** keeps three digits and has a tiny range: nine decades.
- **bfloat16** keeps float32's range and two digits. It trades precision
  for not having to rescale.

## Where Half Precision Is Supported

Support differs between *storing* half-precision values and *computing* with
them. Establish both for the target before planning.

| Platform | Storage / conversion | Arithmetic |
|----------|----------------------|------------|
| x86-64 CPUs | conversion instructions are common (F16C) | native only with AVX-512 FP16; otherwise compute in float32 |
| AArch64 CPUs | yes | native where the half-precision extension is present |
| CUDA GPUs | `__half` / `__half2` with conversions and arithmetic intrinsics (`cuda_fp16.h`) | yes; matrix products additionally through tensor cores in vendor libraries |
| C++ | `std::float16_t` (`<stdfloat>`, C++23), `_Float16` (GCC, Clang) | compiler-dependent: may be emulated through float32 |
| Fortran | compiler-specific; ask with `selected_real_kind(3, 4)` and check `storage_size` | often absent |

`scripts/env-check.sh` reports the CPU features it can see. If arithmetic is
not native on the target, "half precision" means half-precision *storage*
with float32 arithmetic, which is the common and useful case anyway.

## Usage Patterns, From Safe to Demanding

| Pattern | What is half | Gain | Needs |
|---------|--------------|------|-------|
| **Storage only** | arrays at rest and in transit; converted to float32 on load, computed in float32, rounded on store | half the memory traffic and footprint of those arrays | values within range (scale them), and a QoI budget that tolerates 1e-3 relative noise injected at every store |
| **Selected inputs** | coefficients, material tables, geometry factors, preconditioner data: read often, written rarely | memory traffic | the data's own accuracy is no better than three digits |
| **Low-precision inner work** | a preconditioner, a coarse-grid solve, a smoother inside an iteration whose residual is computed at higher precision (rung L6) | arithmetic and memory | the outer iteration absorbs the error; convergence must be re-verified |
| **Whole kernels in half** | state and arithmetic | largest | almost never meets a simulation's accuracy budget; machine-learning inference is the usual home |

Never half: accumulators, time, coordinates, indices, counters, anything
that integrates over many steps, anything compared for equality.

## Hazards

| Hazard | Detail | Remedy |
|--------|--------|--------|
| Range (float16) | anything outside 6e-5 … 65 504 overflows or loses precision | scale each array to order 1 with a stored scale factor (per array or per block), or use bfloat16 |
| Three digits | every store rounds at 5e-4 relative | keep increments out of half storage: store the state in float32 and only slowly varying data in half |
| Repeated rounding | load–modify–store cycles accumulate rounding at 1e-3 per cycle | compute in float32 across the whole update; round once |
| Exact integers end at 2 048 | counts and indices in half are wrong almost immediately | integers |
| Tolerances | nothing below 1e-3 is resolvable | tolerances from the type's epsilon; expect far fewer attainable digits |
| Conversions | float16↔float32 conversions are cheap where native, slow where emulated | check the target; keep conversions at array boundaries |

## Analysis Specifics

- Probe summaries: `scripts/probe-summary.py --target fp16` or
  `--target bf16`. Range flags are the first thing to read: most float32
  data does not fit float16 unscaled.
- Format emulation: `cuda::fp64_custom<5, 10>` (FP16) and
  `cuda::fp64_custom<8, 7>` (BF16) through the role alias, or a significand
  sweep from 24 bits downwards, show how many bits each role tolerates
  before a QoI leaves its budget. That curve, per role, is the core of the
  analysis report for this transition: it tells the user which arrays can go
  to half at all.
- Storage-only candidates can be tested without touching arithmetic: round
  the array to half and back at the point where it would be stored, and
  measure the QoIs.

## Budgets

With three digits per value, QoI budgets for anything computed from
half-precision data are of order 1e-3 to 1e-2 unless the data enters only
through a well-conditioned average. Say this at setup. If the user's budgets
are tighter, the realistic scope is storage of selected inputs and
low-precision inner work, and the analysis should rank arrays by memory
traffic and tolerance instead of trying to lower everything.

## Verification Specifics

- The comparison harness works with any candidate type that has
  `numeric_limits` (C++: `std::float16_t`); the narrower gate is then in
  units of 9.8e-4.
- Test the scale factors: an array whose scale is stale or zero overflows
  silently to Inf in float16.
- With the half role switched off, a storage-only change must leave the
  `hi` and plain float32 builds within roundoff of what they were.
