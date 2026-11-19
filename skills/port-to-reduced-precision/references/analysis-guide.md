# Analysis Guide (Phase 1)

Find out where precision matters before changing anything. The phase answers
four questions, cheapest first, and writes the answers to
`.precision-port/precision_map.json`:

| Step | Question | Cost |
|------|----------|------|
| [1a Inventory](#1a-inventory-the-static-precision-surface) | What in the source is tied to one precision? | seconds, static |
| [1b Timing](#1b-timing-where-lower-precision-buys-time) | Where would lower precision save time? | one profiled run |
| [1c Baseline gap](#1c-baseline-gap-what-happens-if-everything-is-lowered) | What breaks, and by how much, if everything is lowered? | one or a few runs |
| [1d Ranges](#1d-value-ranges) | Which values flow through the functions that matter? | one instrumented run |
| [1e Sensitivity](#1e-sensitivity-per-function) | Which functions lose digits, and is it the formula or the problem? | minutes per function |
| [1f Growth](#1f-error-growth-over-iterations) | How does the error grow over a run? | one run with checkpoints |

Then [classify](#classification), write the
[precision map](#precision_mapjson), and present the
[analysis report](#the-analysis-report). That report is a gate.

**Scope the expensive steps.** 1a to 1c cover the whole code. 1d to 1f are
done only for *candidate functions*: the union of

- the functions that hold most of the time (1b), because that is where the
  port pays off,
- the functions on the data path of a QoI that failed in 1c, and
- the functions the inventory flags for a known hazard (accumulators, time
  variables, constants outside the target range, tolerances).

Everything else is recorded under `not_analyzed` so the report can say what
was not looked at.

**Rounds.** In round 1 do all steps. In later rounds re-run 1c to 1e for the
functions touched or newly exposed; repairs unmask errors that a larger one
was hiding.

## 1a Inventory: the static precision surface

Setup already ran the scan. Read the full result now:

```bash
scripts/precision-scan.py "$SOURCE_DIR" --json .precision-port/inventory.json --top 30
```

What the categories mean for the port:

| Category | Why it matters | Feeds |
|----------|----------------|-------|
| `type_fp64`, `literal_fp64`, `literal_default`, `math_*` | Mechanical work of Phase 3; also FP64 "leaks" in a `lo` build | effort estimate |
| `precision_alias` | An existing switch and its roles | strategy |
| `literal_out_of_fp32_range` | Constants that become 0, Inf or subnormal | 1d, class `range` |
| `tolerance_candidate`, `epsilon_query` | Convergence tests and guards tuned for one precision | class `tolerance` |
| `time_accumulation`, `reduction` | Error that grows with the number of terms or steps | 1e, 1f |
| `float_equality`, `float_loop_counter`, `float_int_conversion` | Control flow and indices decided by rounding | 1e |
| `interop_*`, `bit_tricks` | Boundaries where a type is fixed by someone else | Phase 3 boundaries |
| build flags | Value-unsafe math, kind promotion | setup Q10, remedies L4 |

The scan is a regex heuristic that over-reports. For exact locations use the
compiler: the flavor guide lists the diagnostics per language
(`-Wdouble-promotion`, `-Wconversion`, `-Wshorten-64-to-32`, clang-tidy
checks, `-Wconversion-extra`). Run them on the files the scan ranks first,
not on the whole tree at once.

For a large code, delegate one subsystem per subagent with a narrow brief:
"triage the inventory findings of `{dir}`; return for each flagged site
`{file, line, category, real / false positive, note}`". Merge the answers;
do not read every flagged line yourself.

## 1b Timing: where lower precision buys time

Lower precision helps in three ways, and a function benefits according to
what limits it today:

| Limiter | Sign | Gain from lower precision |
|---------|------|---------------------------|
| FP64 arithmetic throughput | GPU: the FP64 pipe is the busiest resource | up to R (large on `rationed` devices, about 2 on `native` ones) |
| Memory bandwidth / cache | traffic dominates, arithmetic units idle | up to 2 (half the bytes); more when the working set starts to fit a cache |
| SIMD width (CPU) | vectorized loops | up to 2 (twice the lanes) |
| Latency, control, I/O, communication | none of the above saturated | little |

Get a ranking of where time goes for the active problem, then classify the
top entries. Follow the
[performance guide](performance-guide.md#finding-where-time-goes) for the
commands (Nsight Systems with NVTX ranges; Nsight Compute counters for GPU
kernels; the project's own timers are acceptable when they are synchronized
and cover the run).

Record per function `time_pct` and `limiter`
(`fp64`, `memory`, `simd`, `latency`, `unknown`). A function that is neither
hot nor on a failing QoI's path does not need the expensive analysis.

## 1c Baseline gap: what happens if everything is lowered

Run the correctness case with as much of the code as possible at the target
precision and compare every QoI with the golden reference. This gives the
size of the problem and, for QoIs whose budget is still `tbd`, the number the
user needs. Use the first method that applies:

| # | Method | Needs | Notes |
|---|--------|-------|-------|
| S1 | **Precision builds** | an existing switch | The real thing. With roles, also build one profile per role lowered alone: it shows which role breaks which QoI. |
| S2 | **Verrou** (CPU, no source change) | `valgrind --tool=verrou` | `--rounding-mode=float` runs every double operation in single precision on the unmodified binary. Slow (one to two orders of magnitude), so use a short case. |
| S3 | **Format emulation through the alias** | a `Real` alias; CCCL FP component | Swap the alias for `cuda::fp64_custom<8, 23>`: values stay 8 bytes, every operation rounds to FP32. Works in host and device code. See [caveats](#format-emulation-with-the-cccl-fptool). |
| S4 | **Kind-demotion compiler flag** (Fortran) | a self-contained code | e.g. `gfortran -freal-8-real-4`. An experiment only: library interfaces, MPI types and I/O do not follow the flag. |
| S5 | **Lowered scratch copy** (C, C++) | a small, self-contained code without an alias | Copy the sources to a scratch directory outside the repository, replace `double` by `float` textually, and build with GCC's `-fsingle-precision-constant` (Clang ignores that flag: use GCC for this experiment). Minutes of work for a single-file code; hopeless across library interfaces. An experiment only, never the port. |
| S6 | **None of the above** | — | Skip to 1d and 1e; the baseline gap is measured after Phase 3. Say so in the report. |

Record the outcome in `precision_map.json:baseline_gap`:

```json
{"qoi": "energy_gain", "method": "S1 profile lo", "lo_error": 1.0, "metric": "rel",
 "budget": 1e-4, "pass": false, "note": "result is exactly 0"}
```

Interpretation:

- **Crashes, hangs, non-finite values, changed step or iteration counts**
  come first. They point at tolerances, loop conditions, indices and range
  problems (classes `tolerance`, `range`, `integer`), which no amount of
  reformulated arithmetic fixes.
- **A QoI within budget** needs no numerical repair on its path; its
  functions are candidates only if they are hot.
- **A QoI far outside budget** (errors of 1e-3 and worse in float32) means
  digits are being destroyed somewhere, not merely rounded. 1e finds where.
- **A QoI slightly outside budget** (a few 1e-6 relative) may be at the
  limit of what the format gives after many operations. Check 1f before
  planning repairs: the remedy is then accumulation or the budget itself.

### Localizing with delta debugging (S2)

Verrou can search for the symbols or source lines whose single-precision
execution breaks a comparison. Provide a run script that executes the
program under Verrou into a directory, and a compare script that exits 0 when
that directory's results are acceptable (wrap `scripts/qoi-compare.py`):

```bash
verrou_dd_sym  /path/to/dd_run.sh /path/to/dd_cmp.sh    # which functions
verrou_dd_line /path/to/dd_run.sh /path/to/dd_cmp.sh    # which source lines (needs -g)
```

The result is a list of symbols or lines that must stay at high precision
for the comparison to pass: direct input for the candidate list. Verrou also
reports cancellations (`--check-cancellation=yes --cc-gen-file=FILE`,
thresholds `--cc-threshold-float` / `--cc-threshold-double`) and operands
that overflow a float (`--check-max-float=yes`). Check the option names
against `valgrind --tool=verrou --help` of the installed version.

### Format emulation with the CCCL fptool

The CCCL FP component (headers `<cuda/fptool>`, `<cuda/fpmp>`,
`<cuda/fpemu>`; present on recent CCCL, check with `scripts/env-check.sh`)
provides `cuda::fp64_custom<exponent_bits, mantissa_bits>`: a `double` that
is rounded to a narrower format after every operation. Compile with
`-DCCCL_ENABLE_FPTOOL` for the whole project.

| Alias target | Emulates |
|--------------|----------|
| `cuda::fp64_custom<8, 23>` | FP32 (range and precision) |
| `cuda::fp64_custom<11, 23>` | FP32 precision with FP64 range |
| `cuda::fp64_custom<5, 10>` | FP16 |
| `cuda::fp64_custom<8, 7>` | BF16 |
| `cuda::fp64_custom<cuda::fp_custom_dynamic_size, cuda::fp_custom_dynamic_size>` | set at run time, e.g. `cuda::fp_custom_set_host_mantissa_size(n)` |

Two uses that precision builds cannot offer:

- **Separate range from precision.** If `<11, 23>` meets the budget and
  `<8, 23>` does not, the problem is overflow or underflow, not significand
  width: the remedy is scaling or units (class `range`), not reformulation.
- **Sweep the significand.** With the dynamic size, plot QoI error against
  mantissa bits. The curve says how many bits the computation needs, how
  much margin FP32 has, and whether FP16 or BF16 are worth a thought.

Caveats to state with every result obtained this way:

- Only `sqrt` and `fma` round to the emulated format. **Every other math
  function (`exp`, `log`, `sin`, `fabs`, ...) silently evaluates in full
  FP64**, so the emulation is optimistic for code that leans on them.
- Rounding happens on operations, not on construction or comparison; the
  emulated format has no subnormal range (underflow flushes to zero).
- Values stay 8 bytes: memory layout, I/O and performance are those of the
  `hi` build. The emulation predicts accuracy, never speed.
- Explicit construction from `double` is required by default; define
  `CCCL_FP_CUSTOM_EXPLICIT_CASTS=0` when swapping an alias across an existing
  code base.

## 1d Value ranges

Record which values actually flow through the candidate functions, on the
real workload, in the `hi` build. Copy the probe into the project
(`assets/probe/precision_probe.hpp` or `.f90`) and place probes at:

- **function boundaries**: inputs and outputs, whole arrays at once
  (`PP_RANGE_ARRAY` / `pp_range`);
- **constants and products of constants** that the inventory flagged, and
  intermediates that multiply or divide extreme values;
- **accumulators**: the running value and the increment (`PP_ADD`);
- **integers** that could be narrowed: sizes, global indices, ids, counters
  (`PP_COUNT`).

The flavor guide shows the calls. Build with the probes enabled, run the
performance workload once (probe output goes to `PRECISION_PROBE_DIR`), and
summarize against the target format:

```bash
mkdir -p .precision-port/probes/{problem}/round_{N}
PRECISION_PROBE_DIR=.precision-port/probes/{problem}/round_{N} \
    bash .precision-port/run_performance.sh
scripts/probe-summary.py --target fp32 --int-target int32 \
    --json .precision-port/probes/{problem}/round_{N}/summary.json \
    .precision-port/probes/{problem}/round_{N} \
    | tee .precision-port/probes/{problem}/round_{N}/summary.md
```

| Flag | Meaning | Usual class |
|------|---------|-------------|
| `OVERFLOW`, `UNDERFLOW` | values outside the target's normal range | `range` |
| `SQUARE_OVERFLOWS`, `SQUARE_UNDERFLOWS` | the value fits, its square does not: check norms, energies, products | `range` |
| `WIDE_RANGE` | values span more binades than the significand has bits: sums lose the small terms | `accumulation` |
| `INTEGER_VALUED_LARGE` (info) | every value is an integer, some beyond 2^24 (FP32) or 2^11 (FP16) | `integer` if the data is a counter, an index or a sum of ones held in floating point; otherwise nothing (physical quantities are often integer-valued by accident) |
| `INT_OVERFLOW`, `INT_LITTLE_HEADROOM` | integers beyond or near the narrow type's limit | `integer` |
| `NONFINITE` | the reference run itself produces NaN or Inf here | investigate first |

The script exits with status 0 whether or not it reports blockers (it is an
analysis, not a gate; `--fail-on-blocker` changes that for scripted use).

Four cautions:

- **No flag is not a clean bill of health.** The summary judges one
  operation at a time. An update that loses a few bits per step, always in
  the same direction, passes every single-operation test and still breaks a
  budget over a run: that is what [1f](#1f-error-growth-over-iterations)
  measures.
- **A pass covers this workload only.** Values the run never produced were
  not judged. Compare the observed ranges with what the physics and the
  input parameters allow, and record the *assumed* range (with margin) for
  each quantity: that range is what the Phase 4 tests sweep and what a
  repair is valid for.
- **Intermediates overflow before results do.** A result of order 1 can be
  the ratio of two products that each leave the format. Probe the products.
- **Probes cannot run in device code.** Probe at kernel boundaries, or run
  this step on a CPU build of the same source.

Commit the probe calls (they compile away without `PRECISION_PROBE`) and the
summaries; the raw `.jsonl` files are ignored by the template `.gitignore`.

## 1e Sensitivity per function

For each candidate function, measure how much lowering the precision of
*that function alone* hurts, and decide whether the formula or the problem
is to blame.

### Add/subtract probes

Wrap the subtractions and accumulations the function performs in `PP_SUB` /
`PP_ADD` and read the summary of the same run as 1d:

- `CANCELLATION`: leading bits lost when nearly equal values are subtracted.
  The subtraction is exact; it exposes the rounding error already in its
  operands. Losing `k` bits leaves at most `p − k` good bits, where `p` is
  the target significand (24 for FP32, 11 for FP16).
- `ABSORPTION`: the exponent gap between the operands. A term `g` bits below
  its partner keeps `p − g` of its bits and none once `g ≥ p`. This is how
  `t += dt` stops advancing and `x += v * dt` stops moving.
- `PARTIAL_ABSORPTION` (warning): the gap exceeds half the significand
  without reaching it. Harmless in a single operation. In an accumulation
  (`x += small` every step) the rounding has the same sign for as long as
  `x` stays in one binade, so the *increments* are systematically wrong by
  up to `2^(g − p)` relative: a 17-bit gap in FP32 leaves 7 bits of each
  increment, an error of the accumulated change of order 1 %. Whether that
  breaks a QoI depends on the run length: measure it in 1f.

High counts are not errors by themselves: compensated summation produces a
cancellation in every step while being at its most accurate. The numbers say
where to look, the demotion test says whether it hurts.

### The demotion test

Build the function's standalone test now (it becomes the Phase 4 test; the
layout is in the [verification guide](verification-guide.md#the-standalone-test)).
Feed it inputs covering the assumed range from 1d, evaluate the function at
the reference precision and at the target precision, and record the
**amplification**: relative error divided by the target epsilon.

- With an existing switch or templated kernels, call the function at both
  precisions directly.
- Otherwise use a test-only lowered copy of the function, or evaluate it
  with the emulated format from 1c. Do not touch the production source yet.

`PP_DEMOTE(name, ref, lo)` records the distribution; the comparison harness
prints the same numbers for a test:

| Amplification `A` | Reading |
|-------------------|---------|
| up to about 16 | as accurate as the format allows after a few operations |
| 2^k | about `k` bits lost on top of rounding |
| about `1/eps` (8e6 for FP32) | no correct digits |

### Conditioning: formula or problem?

Some functions are inaccurate in low precision whatever the formula, because
their *result* is sensitive to the last bits of their *input*. Measure it:
at the reference precision, perturb each input by a random relative amount of
one target epsilon and divide the relative change of the output by that
epsilon. The maximum over the sweep is the effective condition number `κ`.

| Finding | Class | What helps |
|---------|-------|------------|
| `A` small | `benign` | nothing needed |
| `A ≫ κ` (κ of order 1, A large) | `cancellation` / `absorption` | reformulation: the problem is well-conditioned, the formula is not |
| `A ≈ κ ≫ 1` | `ill_conditioned` | no rewriting of this formula can help; change what is represented (a difference, a perturbation, a relative coordinate), compute this piece wider, or accept |

This test decides between the cheap rungs of the remedy ladder and the
expensive ones, so do it for every function that the demotion test flags.

### How many bits?

Where the format emulation of 1c is available, sweep the significand for the
flagged function alone. A function that reaches its budget at 30 bits will
not be rescued by micro-rewrites in FP32; one that reaches it at 20 has
margin.

## 1f Error growth over iterations

For time-stepping or iterative codes, compare a `lo`-like run with REF at
several points in time (QoIs after 1, 10, 100, 1000 steps).

**This step is required for every update of the form `state += increment`
inside a time or iteration loop** (the scanner lists them as
`accumulation_candidate` and `time_accumulation`), whatever the probe
summary says about the site: the error of such an update is invisible in one
step. If no lowered run of the application exists yet, measure the kernel
alone: a test-only template copy of the update, instantiated at the
reference and the target type and stepped from a state captured from the
workload ([flavor guide](../flavors/cpp/precision-types.md#standalone-test-for-a-kernel-with-state)).
State the result per accumulated quantity: gap in bits, bits of each
increment kept, error after the production number of steps.

| Growth | Likely cause | Direction |
|--------|--------------|-----------|
| flat, a few eps | per-step rounding only | nothing to do |
| proportional to steps | systematic rounding in an accumulation (`+=` of small increments, time, conserved sums) | compensated accumulation, a different representation, an integer step count |
| proportional to √steps | unbiased rounding random walk | usually acceptable; check the budget at the full run length |
| exponential, then saturating | physical chaos or a numerical instability | compare REF with itself under a tiny input perturbation: the same rate means chaos (use statistical QoIs), a faster rate at `lo` means an instability margin was eaten by rounding |
| sudden jump | a branch, tolerance or index decided differently | find the step, inspect counts and control flow |

Record the law and the projected error at the production run length. A port
verified on 100 steps says nothing about 10^6 if the growth is linear.

## Classification

Give each candidate function exactly one class: the dominant reason it needs
work. A function with two independent problems gets two plan items.

| `class` | Evidence | First rung to try |
|---------|----------|-------------------|
| `benign` | within budget, `A` small | L1 only |
| `mechanical` | fails only through a literal, a hard-wired type or math call, an FP64 leak | L1 |
| `tolerance` | fixed epsilon, convergence threshold, float equality, loop condition | L1 (scale with the type's epsilon) or a decision if the tolerance is unreachable |
| `range` | overflow / underflow flags; passes with FP64 range and FP32 significand | L3 (reorder, precombine constants), L5 (units) |
| `cancellation` | lost bits, `A ≫ κ` | L2 |
| `absorption` | operand gap; error grows with steps | L4 (a carry word per value: same variable, one more word of state) or L5 (store something else: offsets, cell + fraction, step counts). Measure both where both are feasible: they differ in cost at `hi`, in memory and in how much code they touch |
| `accumulation` | long sums or dot products, `WIDE_RANGE` | L4 |
| `ill_conditioned` | `A ≈ κ ≫ 1` | L5, L6, L7, or L9 |
| `integer` | integer flags | [int64-to-int32](../transitions/int64-to-int32.md) |
| `interop` | a library, file format or MPI type fixes the precision | boundary conversion in Phase 3; decision if it is on the hot path |
| `unknown` | evidence missing or contradictory | more analysis, never a guess |

## `precision_map.json`

Start from `assets/precision-map-template.json`. One entry per candidate
function:

```json
{
  "file": "src/pusher.cpp",
  "function": "kinetic_energy",
  "role": "particle",
  "time_pct": 3.1,
  "limiter": "fp64",
  "qois": ["energy_gain"],
  "evidence": {
    "inventory": ["math_unqualified", "reduction"],
    "ranges": {"site": "ke:ux", "assumed": "|ux| in [1e3, 1e9] m/s", "flags": []},
    "addsub": {"site": "ke:gamma-1", "max_lost_bits": 38, "flags": ["CANCELLATION"]},
    "demote": {"max_amplification": 8.4e6, "mean_amplification": 2.1e6},
    "kappa": 2.0,
    "growth": "n/a"
  },
  "class": "cancellation",
  "proposed_remedy": "L2: sqrt1pm1(u^2/c^2) instead of gamma - 1",
  "needs_decision": [],
  "notes": ""
}
```

`needs_decision` lists the [decision triggers](../SKILL.md#approval-gates-vs-engagement-level)
the proposed remedy would hit, so the report can announce them. Under
`sources`, record which methods produced the evidence and the paths of the
probe summary and timing files. Under `not_analyzed`, list what was left out
and why (not executed by this workload, cold and not on a QoI path, device
code that could not be probed).

## The Analysis Report

Present this in chat, then stop for the user. Fill every section; write
"not measured" rather than leaving one out.

````markdown
## Precision Analysis — {problem}, round {N}

### Verdict
{One paragraph: can this code reach the budgets at {target}? What stands in
the way, how much work is it, what will need the user's decision.}

### Baseline gap (everything lowered, method {S#})
| QoI | budget | error at lo | status |
|-----|--------|-------------|--------|

### Where lower precision buys time
| function | time % | limiter | expected gain |
|----------|-------:|---------|---------------|

### Where precision is lost
| # | function | class | evidence (bits lost, amplification, κ) | proposed remedy | decision needed |
|---|----------|-------|-----------------------------------------|-----------------|-----------------|

### Value ranges and assumptions
{Observed range per key quantity, the assumed range with margin, flags
against the target format. State each assumption as a sentence the user can
confirm or correct.}

### Mechanical work
{Counts from the inventory: types, literals, math calls, boundaries.
Existing switch and roles; proposed roles if none.}

### Not analyzed
{Code the workload did not execute, functions skipped, tools unavailable.}

### Budgets to confirm
{Every QoI still `tbd`, with the attainable accuracy measured above.}
````

Gate: the user confirms the assumptions and approves any `tbd` budget (set
`status: "approved"` in `config.json`). In analysis-only mode, stop here.
Set `analysis_complete: true` and commit the precision map, the probe
summaries and the report inputs.
