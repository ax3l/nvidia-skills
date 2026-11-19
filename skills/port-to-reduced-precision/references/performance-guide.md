# Performance Guide

How to find where lower precision pays off, how to measure a change of a few
percent honestly, and how the target hardware decides between remedies.

- [The FP32:FP64 Ratio R](#the-fp32fp64-ratio-r)
- [Finding Where Time Goes](#finding-where-time-goes)
- [GPU Kernels: What Limits Them](#gpu-kernels-what-limits-them)
- [The Build Matrix](#the-build-matrix)
- [Measuring a Change](#measuring-a-change)
- [The hi Budget](#the-hi-budget)
- [Choosing Wider Arithmetic](#choosing-wider-arithmetic)
- [What Lower Precision Does Not Speed Up](#what-lower-precision-does-not-speed-up)

Profiling uses NVIDIA Nsight Systems (`nsys`) for timelines and NVIDIA
Nsight Compute (`ncu`) for per-kernel counters, with the same NVTX naming
grammar as the `nsight-port-cpu-to-gpu` skill (`iter:`, `phase:compute:`,
`phase:host:`, `function:<file>:<function>`). When that skill is installed,
its profiling guide is the detailed reference for capturing and distilling
`nsys` reports; never load a binary report into context, only text
distillations.

## The FP32:FP64 Ratio R

R is how many times faster the target device executes single-precision
arithmetic than double-precision arithmetic. It varies by more than an order
of magnitude between devices that look alike from the source code, and it
decides almost every FP64-related tradeoff of the port.

| R | `fp64_policy` | Typical of | What it means |
|---|---------------|-----------|---------------|
| about 2 | `native` | CPUs (SIMD width), GPUs built for FP64-heavy computing | FP64 is a first-class citizen. `lo` gains come mostly from halved memory traffic and footprint. Keeping a few values in `double` is cheap. |
| about 4 to 10 | `native` to `rationed` | in between | Compute-bound FP64 kernels gain noticeably; islands are affordable off the hot path. |
| 16 to 64 | `rationed` | GPUs designed around FP32 and lower | An FP64 instruction costs as much as R FP32 ones, **and so does every float↔double conversion**, which executes on the FP64 units. A kernel that mixes precisions can be slower than the all-double one. |
| ∞ | `absent` | devices and APIs without FP64 | FP64 in device code does not compile or is emulated at great cost. |

Obtain R, in this order of preference:

1. The vendor's published FP32 and FP64 throughput of the device.
2. Measure: build and run `assets/microbench/fp_rate.cu` on the target GPU.
   It times identical fused multiply-add chains in `float` and `double`
   and prints the ratio and a class. Read it to one significant digit:
   clocks, power caps and thermal state move the absolute rates, and on
   power-limited devices the two precisions may even run at different
   clocks.
3. For a CPU, use 2 for vectorized code (twice the lanes per register) and
   about 1 for scalar, latency-bound code.

Record R and its source in `config.json:target_hardware`. If the code must
run on devices of different classes, every hardware-dependent decision is
taken for the most restrictive one and re-checked on the others.

## Finding Where Time Goes

Phase 1 needs a ranking of functions by time for the active problem. Any
trustworthy source will do; in order of preference:

**GPU code — kernel table by NVTX range:**

```bash
source .precision-port/env.sh
nsys profile -t cuda,nvtx -o "$PROFILE_DIR/ref" --force-overwrite=true \
    bash .precision-port/run_performance.sh
nsys stats --force-export=true --report cuda_gpu_kern_sum:nvtx-name \
    --format csv "$PROFILE_DIR/ref.nsys-rep" > "$PROFILE_DIR/kernels.csv"
nsys stats --force-export=true --report nvtx_sum \
    --format csv "$PROFILE_DIR/ref.nsys-rep" > "$PROFILE_DIR/nvtx.csv"
```

`PROFILE_DIR` is `.precision-port/profiles/{problem}/round_{N}`. Report
names differ between `nsys` versions; list them with
`nsys stats --help-reports`. Always pass `--force-export=true`, or a stale
export may be reused. Keep the top rows (enough to cover about 90 % of the
kernel time) as text; the binary report stays local.

**CPU code — sampling or instrumentation:**

- `nsys profile --sample=process-tree` when CPU sampling is permitted on the
  machine (`scripts/env-check.sh` reports it);
- otherwise NVTX ranges around the suspected functions, which need no
  sampling permission;
- or the project's own timers, if they cover the run and, for GPU regions,
  are synchronized.

Add `function:` ranges for every function that becomes a candidate, so that
later rounds attribute time the same way. Commit instrumentation separately
from numerical changes.

## GPU Kernels: What Limits Them

Whether a hot kernel gains from lower precision depends on what limits it.
Collect per-kernel counters for the hot kernels of a few iterations:

```bash
ncu --kernel-name "regex:{hot kernel names}" --launch-count {n} \
    --metrics {comma-separated list} -o "$PROFILE_DIR/ncu_ref" \
    bash .precision-port/run_performance.sh
```

| Question | Metrics (validate with `ncu --query-metrics`) |
|----------|-----------------------------------------------|
| How much FP64 / FP32 / FP16 arithmetic runs? | `sm__sass_thread_inst_executed_op_{d,f,h}{add,mul,fma}_pred_on.sum`; class totals `sm__sass_thread_inst_executed_op_fp64_pred_on.sum`, `..._fp32_pred_on.sum`, `..._fp16_pred_on.sum` |
| How many float↔double (and float↔int) conversions run? | `smsp__sass_thread_inst_executed_op_conversion_pred_on.sum` |
| Is the FP64 pipe the bottleneck? | `sm__pipe_fp64_cycles_active.avg.pct_of_peak_sustained_active`, next to `sm__pipe_fma_cycles_active.avg.pct_of_peak_sustained_active` |
| Is memory the bottleneck? | `dram__throughput.avg.pct_of_peak_sustained_elapsed`, `dram__bytes_read.sum`, `dram__bytes_write.sum` |

Metric names depend on the GPU architecture and the `ncu` version. Query
them on the target before relying on a name, and note that profiling may
need permissions the system administrator controls.

Classify each hot kernel (`limiter` in the precision map):

| Finding | `limiter` | Expect from lowering its precision |
|---------|-----------|-------------------------------------|
| FP64 pipe near its peak | `fp64` | large gain, approaching R when the whole kernel converts and nothing else becomes the limit |
| memory throughput near its peak, pipes idle | `memory` | up to 2 from halved bytes; nothing if the data layout does not change |
| neither | `latency` | uncertain; measure with builds, do not estimate |

Two more things the counters show that timings do not:

- **Conversions.** A `lo` or mixed kernel with many executed conversions is
  paying FP64-rate instructions for crossing a precision boundary inside the
  loop. Move the boundary out of the loop or make the kernel uniform.
- **FP64 that should not be there.** FP64 counts in a kernel of the `lo`
  build are leaks or islands; the static list with function names comes
  from `scripts/fp64-audit.py`.

## The Build Matrix

Performance statements compare builds, not estimates:

| Build | Source | Precision | Used for |
|-------|--------|-----------|----------|
| REF | baseline commit | original | the timing baseline and the golden reference |
| `hi` | ported | original | the **cost** of the port: `hi` against REF |
| `lo` | ported | target | the **gain** of the port: `lo` against REF |
| one role lowered | ported | mixed | which role carries the gain; optional, when roles exist |

Rules:

- Same compiler, same flags, same machine, same input, for every build
  compared. Only the profile differs.
- **Equal work.** Check that the builds did the same amount of work: same
  step and iteration counts, same particle or cell counts. A `lo` run that
  takes fewer solver iterations is not faster arithmetic.
- **Measured builds are the ground truth.** Counter-based models explain
  and extrapolate them; they do not replace them. A model that predicts a
  gain the build does not show is wrong about what limits the kernel.
- Results are specific to the input, the problem size and the device. Say
  which ones every number belongs to.

## Measuring a Change

The budget for slowing the `hi` build is a few percent; ordinary run-to-run
variation is often of the same size. A single pair of runs cannot decide.

```bash
scripts/bench-compare.py \
    --base "bash .precision-port/run_ref.sh" --label-base REF \
    --cand "bash .precision-port/run_performance.sh" --label-cand hi \
    --repeats 7 --budget-pct 5 --metric-regex 'compute time: ([0-9.eE+-]+)' \
    --json "$PROFILE_DIR/hi_vs_ref.json"
```

The script interleaves the two commands, compares medians, and reports a
95 % interval and one of five verdicts:

| Verdict | Meaning | Action |
|---------|---------|--------|
| `better` | faster beyond noise | report |
| `within_noise` | no resolvable difference | report as "within noise", not as "no cost" |
| `worse_within_budget` | slower, inside the budget | report the number |
| `over_budget` | slower beyond the budget | decision trigger 4 |
| `inconclusive` | the interval straddles the budget | measure better before deciding |

Protocol:

1. **Know the noise floor.** Run `--aa` (the same command on both sides)
   once per workload and machine state. If its interval is wider than the
   budget, the setup cannot resolve the budget: lengthen the run, time only
   the compute phase (`--metric-regex`), pin the run (`--pin {cpu}`; on
   machines with fast and slow cores an unpinned run can land on either),
   count CPU time instead of wall time for a serial CPU-bound program
   (`--cpu-time`), use an idle node. Use the same options for every later
   measurement of that workload.
2. **Keep REF runnable.** Build REF once from `baseline_commit` into its own
   tree (a git worktree keeps the working tree untouched) with
   `build_ref.sh` and `run_ref.sh` next to the other scripts
   ([setup guide](setup-guide.md#performance-scripts)), so that base and
   candidate can alternate.
3. **Time the phase that matters.** Start-up, I/O and initialization dilute
   and add noise. Prefer a time the program prints for its main loop.
4. **Per function and per workload.** A helper that is 3 % of the run can
   double in cost and stay invisible in the workload time, yet matter in
   another workload. For small functions, time the standalone test's loop
   (same compiler flags as the application) in addition to the workload:
   call the original and the candidate alternately over the same inputs,
   several trials each, and compare the minimum times; a volatile sink keeps
   the compiler from removing the loop. The `hi` budget binds for functions
   at or above `budgets.hot_function_share_pct` of a workload; for colder
   ones report the slowdown together with its share of the run.
5. **GPUs change clocks.** On power- or thermally limited devices, kernels
   of different precision can run at different clock rates, and a profiler
   attached to the run can change them too. Log the device state during
   timed runs and compare like with like:

   ```bash
   nvidia-smi --query-gpu=clocks.current.sm,power.draw,temperature.gpu \
       --format=csv,noheader -lms 500 > "$PROFILE_DIR/gpu_state.csv" &
   ```

   Time with profilers detached; use profilers for attribution only.
6. **Report the interval**, never a bare percentage.

## The `hi` Budget

`budgets.hi_slowdown_pct` (default 5, ceiling
`hi_slowdown_ceiling_pct`, default 10) bounds how much the ported source may
slow down at the original precision, in exchange for being stable at the
target precision. It applies twice:

- **per item** (Phase 4e): this remedy's cost in the workload, and in its
  function if the function is hot (`hot_function_share_pct`, default 5 % of
  a workload's run time). A cold function's slowdown is reported with its
  share and does not trigger a decision on its own;
- **cumulative** (Phase 5): `hi` now against REF, for every problem.

When a remedy exceeds it, the options to put before the user
([decision guide](decision-guide.md)) are, typically:

| Option | Cost at `hi` | Cost in maintainability |
|--------|--------------|-------------------------|
| accept the slowdown | measured | none: the code stays shared |
| a cheaper rung or variant (pairwise instead of compensated; hoisting; a different identity) | measured | none |
| select the stable form by precision in one small helper (the textbook form at `hi`, the stable form at `lo`) | none | a branch to test in both builds and keep in sync |
| keep the function at `hi` (island) | none | a boundary; conversions; not possible when FP64 is absent |

The third option is a fair one when the first two do not fit, as long as
the branch stays in a small, readable helper. It is not the default: the
`hi` build then no longer exercises the code that `lo` runs.

Speed is not the only currency. A remedy that costs 3 % at `hi` and turns a
wrong `lo` result into a correct one is not comparable with one that costs
3 % for nothing; say what the cost buys.

## Choosing Wider Arithmetic

When a few values need more bits than the target type (remedy L7), the
right implementation depends on R:

| Need | `native` | `rationed` | `absent` |
|------|----------|------------|----------|
| accurate long sum | accumulate in `double` | compensated sum in the working type (`CompensatedSum`, or a float-float accumulator) | compensated sum |
| a state advanced by small increments every step | keep that variable `double`, or a carry word | carry word in the working type (`add_with_carry`), or a representation change | carry word, or a representation change |
| accumulation by scatter-add (atomics) over many steps | `double` accumulator | scatter into a working-precision scratch array, fold per cell with a carry word (two words cannot share one atomic) | the same |
| a few critical operations at higher precision | native `double` island, conversions outside inner loops | double-word arithmetic (`cuda::fp32mp2`); no conversions to `double` in the loop | double-word arithmetic |
| bit-exact double semantics | native | emulated FP64 (`cuda::fp64emu`) | emulated FP64 |
| more than double | double-double (`cuda::fp64mp2`) | software quad | — |

The reasoning to give the user:

- One double-word operation costs on the order of 10 to 20 working-precision
  operations (addition and multiplication near 10, fused multiply-add near
  20; the library's specification has the exact counts). It beats native
  FP64 only when R is larger than that. On `native` devices it is several
  times slower, and it executes that many more instructions, so it is less
  energy-efficient as well. Division and square root are the exception:
  they are multi-step routines in native FP64 too, and emulation can win
  even at small R.
- In memory-bound code, a float pair is as large as a double and gains
  nothing over it.
- A conversion between `float` and `double` is an FP64-rate instruction. On
  `rationed` devices keep conversions out of inner loops entirely: convert
  at the kernel boundary, or stay in the working type.
- A double-word value has the working type's exponent range.

Whatever is chosen is measured on the target device and recorded with the
alternative that was not chosen.

## What Lower Precision Does Not Speed Up

Say this when the analysis shows it, before the user expects a factor of two:

- communication, I/O, start-up, and anything latency-bound;
- integer and index work, unless the indices are narrowed too;
- kernels dominated by atomics or irregular memory access, whose cost is
  not the arithmetic;
- transcendental-heavy kernels gain less than arithmetic-heavy ones on some
  devices and more on others; measure;
- the parts of a mixed-precision code that pay for conversions at the
  boundaries.

And what it does beyond speed: half the memory footprint for the lowered
data, which may allow a larger problem per device or fewer devices per
problem. Report it alongside the timings.
