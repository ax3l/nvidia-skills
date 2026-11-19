# Verification Guide

What "correct" means when the precision changes, and how each phase checks it.

- [The Three Things Being Compared](#the-three-things-being-compared)
- [Gates](#gates)
- [The Standalone Test](#the-standalone-test)
- [The Comparison Harness](#the-comparison-harness)
- [Kernels with State](#kernels-with-state)
- [Truth and Adjudication](#truth-and-adjudication)
- [Reviewing an Outlier Failure](#reviewing-an-outlier-failure)
- [Integers](#integers)
- [FP64 Left in the lo Build](#fp64-left-in-the-lo-build)
- [Long Runs, Chaos, Statistics](#long-runs-chaos-statistics)
- [Compiler Flags That Change Values](#compiler-flags-that-change-values)
- [Diagnosing Failures](#diagnosing-failures)
- [Rollback](#rollback)

## The Three Things Being Compared

| Name | What | Role |
|------|------|------|
| **REF** | original source, original precision, at `baseline_commit`; its outputs are the golden reference | the source of truth |
| **`hi`** | ported source built at the original precision | must agree with REF: the port must not have changed the code's meaning |
| **`lo`** | ported source built at the target precision | must agree with REF within the user's accuracy budgets |

`lo` is compared with **REF**, not with `hi`. If a repair changed `hi`
(approved), `lo` could agree with the new `hi` and both be wrong.

A GPU port adds no fourth case: device and host builds of the same profile
are compared as that skill prescribes; this guide's gates apply to whichever
build the correctness script runs.

## Gates

| Gate | Phase | Build | Compared with | Requirement |
|------|-------|-------|---------------|-------------|
| **G0** | 0 | REF | its own gold | passes; fails when a gold value is perturbed |
| **G1** | 3 | `hi` | gold | after mechanical edits every QoI within its `hi` budget, counts unchanged (identical only if `hi_bit_identical` was requested) |
| **G1b** | 3 | `lo` | — | builds, runs to completion, no unexplained FP64 in the generic scope |
| **G2** | 4d | `hi`, standalone | truth if one exists, else frozen reference | with a truth: within 16 eps of it, and any difference from the frozen reference is reported (decision trigger 3). Without: within the same-precision budget of the frozen reference |
| **G3** | 4d | `lo`, standalone | truth if one exists, else frozen reference | within the narrower-gate budget over the assumed range; no non-finite values; better than the original at `lo` |
| **G4** | 4f, 5 | `hi`, application | gold (or approved `gold_hi`) | every QoI within its `hi` budget |
| **G5** | 5 | `lo`, application | gold | every approved QoI within its `lo` budget |
| **G6** | 5 | `hi` and `lo`, timing | REF timing | `hi` cost within the performance budget, noise-aware ([performance guide](performance-guide.md)) |
| **G7** | 5 | `lo` binary | approved islands | no unapproved FP64 (hard gate for `fp64_policy: "absent"`) |

A gate that cannot be run (no GPU, a tool missing) is reported as **not
run**, never as passed.

The eps-based budgets of G2 and G3 are for one evaluation of a function. A
kernel that advances state over many steps is gated as described under
[Kernels with State](#kernels-with-state).

## The Standalone Test

One test per repaired function, kept in the repository next to the code:

```text
{parent}/precision_tests/test_{function}/
    {function}_reference.{ext}     frozen copy of the original, original precision
    test_{function}.{ext}          driver; built in every profile
```

**The frozen reference copy** is the original function body copied from
`baseline_commit` (`git show {baseline_commit}:{file}`), renamed
`{function}_reference`, with its floating-point types written out explicitly
at the original precision (no alias). It is a test-only artifact: never
linked into the application, never edited afterwards, never regenerated from
the modified function. Regenerating it would compare the new code with
itself and pass with a difference of exactly zero.

Callees of the original that are also being changed are frozen with it;
callees that stay untouched are called directly.

**The driver**:

1. Builds its inputs at the *reference* precision: a sweep over the item's
   assumed range **including both ends and the special values** (zeros,
   equal operands, sign changes), plus, when available, input samples
   captured from the real workload. Log-spaced sweeps for quantities that
   span decades.
2. Calls the reference copy with them.
3. Converts the same inputs to the working precision and calls the current
   function. The conversion is part of what is being tested: at `lo`, the
   function really receives rounded inputs.
4. Compares every output with the harness and returns non-zero on failure.

The driver source is identical for both profiles; only the working alias
differs. Realistic unit sizes; fast; deterministic (fixed seeds).

Where the function is a self-contained formula, add a **truth**: the stable
form evaluated at a clearly higher precision (see below). It turns the
before/after table from "differs from the original" into "is this accurate".

## The Comparison Harness

Shipped as source: `assets/test-harness/precision_compare.hpp` (namespace
`precision_port`) and `precision_compare.f90` (module
`precision_port_compare`). Copy the one for the project's language once into
the source tree; do not re-derive the metric per test.

| Call | Purpose |
|------|---------|
| `default_budget<Cand, Ref>()` / `default_budget(cand, ref)` | **function-level** gate chosen from the two types |
| `field_budget<Cand, Ref>(wmad, rel, floor)` / `field_budget(cand, ref, wmad, rel, floor)` | **field-level** gate from an explicit QoI budget |
| `absolute_budget<Cand, Ref>(abs_tol)` / `absolute_budget(cand, ref, abs_tol)` | **absolute** gate in the quantity's own unit, for positions, times, phases, angles |
| `compare_field(candidate, reference, [n,] budget [, weights])` | wmad, worst relative error, exceedance count, worst element |
| `report_and_gate(result, budget, label)` | prints the numbers, returns pass/fail |
| `error_stats`, `print_accuracy_row`, `no_worse_than` (C++) | accuracy against a truth, in units of the candidate's epsilon |

**The metric.** `wmad = Σ w|a − ref| / Σ w|ref|`: a normalized mean absolute
difference, dimensionless and scale-free, with optional physical weights
(cell volumes). Alongside it, each element is checked: it passes if its
absolute **or** relative difference is within tolerance. `wmad` beyond its
limit always fails; elements beyond theirs fail the outlier check, which is
[reviewable](#reviewing-an-outlier-failure). Non-finite values, empty
fields and shape mismatches fail unconditionally.

**Function-level defaults** (`default_budget`): every element is judged on
its own relative error, with no magnitude floor, so a sweep over many
decades cannot hide a wrong small result behind a large one.

| Gate | When | wmad | element relative |
|------|------|------|------------------|
| same precision | candidate as precise as the reference | 1e-12 (FP64 reference), 1e-6 (FP32) | 1e-10, 1e-4 |
| narrower | candidate narrower than the reference | 4 eps | 16 eps |

`eps` is the candidate type's epsilon (1.19e-7 for FP32, 9.77e-4 for FP16).
The gate follows from the two types passed in, not from the profile: the
`lo` build against the frozen double reference gets the narrower gate, and
so does *either* build against a truth held in a wider type.
The narrower-gate defaults describe a well-conditioned, stable kernel. If the
analysis measured a condition number `κ` well above 1 for the function, the
honest budget is `κ` times larger: set it explicitly in the test
(`budget.rel_tol = 16 * kappa * eps`) with a comment that cites the
measurement. Do not raise it for any other reason.

**Field-level budgets** (`field_budget`, and `scripts/qoi-compare.py` for
the application's QoIs) take their numbers from `config.json:qois[]`. Cells
far below a field's peak carry errors that are large relatively and
irrelevant physically; `rel_floor` says below which fraction of the peak a
cell is compared against the floor instead of its own magnitude. The floor
is part of the approved budget, not a knob to turn until a test passes.

**Absolute budgets** (`absolute_budget`). For a quantity whose zero is
arbitrary, a relative error says nothing: a position error of 1e-9 m is
"100 %" next to the origin and "1e-7" at the far end of the domain. Gate
such quantities on the absolute error in a natural unit (a fraction of a
cell, of the distance moved during the test, of a period), and say which
unit in the test. For a periodic quantity, compare modulo the period;
otherwise a wrap that happens one step apart in the two runs reads as an
error of a whole period.

Pass values to the harness in the type whose accuracy is being judged. A
double result converted to `long double` before the call is counted in
`long double` epsilons.

Environment overrides exist for experiments (`PP_WMAD_TOL`, `PP_REL_TOL`,
`PP_ABS_TOL`, `PP_REL_FLOOR`, `PP_OUTLIER_CHECK`, `PP_LO_WMAD_EPS`,
`PP_LO_AMP_MAX`). A committed test or script never depends on them.

**Never gate on a global sum.** A sum of a field hides cell-wise errors of
opposite sign and is ill-conditioned when its mean is near zero. Sums,
means and energies are good *diagnostics* and good QoIs in their own right
when they are conserved quantities, but they do not replace the field
comparison.

**An exact zero deserves a second look.** A `lo` candidate that matches its
reference to the last bit is almost always the `hi` code in disguise: the
profile did not reach the compiler, or the "candidate" is the reference
copy. Bit-exact agreement is expected only where nothing changed.

## Kernels with State

A function that is called once per step and updates state (a particle push,
a time integrator, an accumulator, a filter) is not judged like a formula.

**One step from a common state** is a pure function of that state: the
function-level gates apply as they are (absolute ones for positions and
phases).

**Many steps** need three adjustments.

1. **The budget is not an eps count.** Every precision drifts from a
   higher-precision truth as steps accumulate: after `N` plain additions the
   *original double code* is typically `sqrt(N)` to `N` eps away, so "within
   16 eps of the truth" fails for REF itself. Use instead:
   - `lo` against the truth: an absolute or relative budget **derived from
     the application's QoI budgets**, in the state's own units, at the run
     length the budget is meant for (or with the growth law of analysis
     step 1f, stated as a projection);
   - `hi` against the frozen reference: the same-precision budget when the
     update is unchanged; when it was repaired, a difference of up to
     `N × eps` of the state's magnitude is the original's own drift. Gate
     `hi` against the truth with the same budget as `lo`, and report the
     difference from the frozen reference with that explanation.
2. **The truth starts where the candidate starts.** Initialize the truth
   from the candidate's own state after its conversion to the working
   precision (reconstructed from the candidate's representation when that
   changed: cell index + offset, value + carry), and hand it the parameters
   **as the candidate uses them**: rounded where the candidate holds them
   in the working precision, unrounded where it holds them wide. A truth
   that starts from the double state, or wraps at an unrounded domain
   length, differs from a correct candidate by the input rounding
   multiplied by the run length. That effect is real and belongs to the
   application gates (G5), which see the whole path; the standalone test
   judges the update.
3. **The truth is the stable update at a wider type**, not the original
   one: a plain `long double` accumulation over thousands of steps is only
   a few digits better than a compensated `double` one. Writing the kernel
   as a template on its scalar type gives the lowered original (for the
   "before" number), the candidate and the truth from two function bodies.

What a repair cannot remove also shows here. A carry word makes the
*addition* exact, but the increment itself (`rate * dt`) is rounded the same
way every step, so the state still drifts by about one eps of the total
amount accumulated. If the budget is tighter than that, the parameters need
more precision than the working type has: a representation or role decision,
not a summation trick.

The C++ flavor guide has a complete example
([Standalone Test for a Kernel with State](../flavors/cpp/precision-types.md#standalone-test-for-a-kernel-with-state)).

## Truth and Adjudication

Many original formulas are inaccurate at the *original* precision in exactly
the regime a repair fixes. Then `hi` after the repair differs from REF, and
the difference is the improvement. To tell improvement from damage, compare
both against a truth:

- evaluate the **stable** form (not the textbook one) at a clearly higher
  precision: `long double` where it is wider than `double` (x86-64: 64-bit
  significand; AArch64 Linux: binary128; not on platforms where it equals
  `double`), `__float128` / `real128` where the compiler has it, or a
  multiprecision library;
- evaluate it **at the rounded inputs** the candidate received, so that the
  comparison measures the formula and not the input conversion (for a kernel
  with state this needs care: see [Kernels with State](#kernels-with-state)).

The C++ harness prints the table (`error_stats`, `print_accuracy_row`);
`assets/stable-math/test_stable_math.cpp` is a complete example:

| function | variant | precision | max err [eps] | p99 [eps] | mean [eps] | worst digits |
|---|---|---|---:|---:|---:|---:|
| sqrt1pm1 | naive | fp32 | 8.39e+06 | 8.39e+06 | 2.52e+06 | 0.0 |
| sqrt1pm1 | stable | fp32 | 1.18 | 1.02 | 0.237 | 6.9 |
| sqrt1pm1 | naive | fp64 | 6.28e+11 | 3.93e+11 | 1.32e+10 | 3.9 |
| sqrt1pm1 | stable | fp64 | 1.16 | 1 | 0.335 | 15.6 |

**With a truth, the truth is the gate.** Compare the candidate with the
truth in both profiles (`default_budget<Real, long double>()` gives 16 eps of
the working type). Compare it with the frozen reference as well, but as
information: where the original was inaccurate, the two *should* differ.

**G2 with adjudication.** If the repaired function at `hi` is outside the
same-precision budget against the frozen reference, it is acceptable only if
it is no less accurate than the original against the truth
(`no_worse_than`, or simply: it passes the gate against the truth and the
original does not do better). That outcome is decision trigger 3: the user is
told which QoIs move, by how much, and that existing regression baselines for
the `hi` build will have to be regenerated. If the user approves:

- record the decision;
- capture **only the affected QoIs** of the new `hi` build as
  `.precision-port/gold_hi/{problem}/{qoi}.dat` and pass that directory to
  the comparison (`scripts/qoi-compare.py --gold-hi`): the `hi` profile is
  then judged against the re-captured value for those QoIs and against the
  original gold for all others. The `hi` tolerance does not change; the
  baseline does, once, with a decision record that names the commit;
- keep the original gold as the reference for `lo` (`--gold-hi` is ignored
  for every other profile), widened only if the truth shows REF itself was
  wrong by more than the `lo` budget (then say so explicitly: the budget is
  being judged against a better reference).

`gold_hi` is captured when a decision approves a change of `hi` values, never
as routine after an item. If a QoI trips its `hi` budget after items that
should not have moved it, look at the QoI before the baseline:

- **An ill-conditioned QoI needs a `hi` budget that says so.** A QoI that
  is a difference of large numbers (an energy gain, a residual, a drift)
  amplifies roundoff-level changes of its operands by
  `κ = (|a| + |b|) / |a − b|`. The default `hi` budget (1e-10 relative) then
  fails on harmless reordering. Set `hi_budget.rel_tol` to `κ` times the
  default at setup, with the measured `κ` as its recorded reason, the same
  way a function test scales its budget by the condition number. This is a
  property of the QoI, decided once; it is not raised again to make an item
  pass.

Without a truth, a `hi` difference beyond the budget cannot be adjudicated
and the remedy is not accepted.

## Reviewing an Outlier Failure

`wmad` within its limit while some elements exceed theirs is common. Look
before deciding:

- **Scattered** exceedances in elements whose magnitude is far below the
  rest: usually rounding in a small result of a larger computation. For a
  *field*, raise `rel_floor` to the magnitude that is physically
  irrelevant, with the user's agreement, and record it in the QoI. For a
  *function sweep*, the small results are exactly what is being tested: this
  is a finding, not noise.
- **Structured** exceedances — one end of the sweep, a boundary row, a
  stride, every element below some input value: a regime the formula does
  not cover, an index or a wrap. A defect.
- **Growing** with problem size, step count or thread count: an
  accumulation or a race. A defect.

Never waive: non-finite values; a `wmad` failure; exceedances at or near the
peak; structured patterns; anything that grows.

Record what was seen (count, where, magnitude relative to the peak) and why
it was judged benign in the item's notes. A waived check that leaves no
trace is indistinguishable from an unverified one.

## Integers

Integer narrowing has no tolerance: a value fits or the result is wrong.

1. **Range argument.** From the configuration and the algorithm, bound each
   narrowed quantity (largest index, count, id). The probe's `INT_*` flags
   show this workload; the bound must cover the largest problem the user
   wants to support. That maximum is an assumption: decision trigger 2.
   Counters and indices held in *floating point* stop being exact at 2^24
   (FP32); the probe summary notes integer-valued data beyond that as
   `INTEGER_VALUED_LARGE`, which matters only where the data really is a
   count.
2. **Runtime guard.** Where a value enters its narrow representation
   (setup, redistribution, I/O), check it once (`checked_narrow`) and stop
   with a message. Not inside hot loops.
3. **Products.** Widen before multiplying (`widen_mul`): `nx * ny * nz`
   overflows 32 bits long before any factor does.
4. **Sanitizer run.** Build the tests once with the compiler's integer
   sanitizers (see the transition guide) and run the correctness case.
5. **Exact comparison.** Integer-valued QoIs use `"exact": true`.

## FP64 Left in the `lo` Build

```bash
scripts/fp64-audit.py {binary} [--mode cuda|host] --allow '{approved island}' --fail-on-fp64
scripts/fp64-audit.py {binary} --loops --functions '{kernels}' --fail-on-fp64
```

The first form lists, per function, the FP64 arithmetic, compares and
float↔double conversions that remain. Each one is either an approved island
(decision record, `--allow` pattern) or a leak to fix. Typical leaks: an
unsuffixed literal, a `double` local or accumulator, a math call resolved to
the double overload, `printf`-style variadic promotion in a debug path, a
constant table declared `double`.

The second form answers the question that matters where FP64 is rationed
rather than absent: **is there FP64 inside a loop?** One conversion of a
finished sum after the loop is harmless; one per element is not. `--loops`
reports the FP64 instructions inside backward-branch ranges of each
function, and the functions called from inside those ranges that contain
FP64 themselves or are double-precision math library entry points (`floor`
instead of `floorf`). With `--loops`, `--fail-on-fp64` fails only on those.
The ranges are conservative, calls are followed one level, and code inlined
into `main` is counted with `main`: keep the kernels as separate,
non-inlined functions in the build that is audited, and restrict the report
to them with `--functions`.

The counts are static (instructions in the binary). For CUDA kernels, the
executed counts per precision and the conversion count come from Nsight
Compute ([performance guide](performance-guide.md#gpu-kernels-what-limits-them)),
and `nvcc -Xptxas --warn-on-double-precision-use` flags FP64 use at compile
time. For `fp64_policy: "absent"`, zero unapproved FP64 in device code is a
hard gate (G7); for the other classes it is reported and explained.

## Long Runs, Chaos, Statistics

- **Verify at the run length that will be used.** If the analysis found
  error growing with the step count, a short test proves nothing about a
  long run. Either test at production length, or measure the growth law and
  include the projection in the report as an explicit, labelled estimate.
- **Chaotic dynamics.** Trajectories diverge at any precision. First
  shorten the comparison window to a few steps, where a point-wise check is
  still meaningful (unit tests always can). For the full run use
  statistical QoIs agreed at setup, and establish REF's own variability by
  running it with inputs perturbed by one epsilon: a `lo` deviation inside
  that spread is noise, outside it is a finding.
- **Conserved quantities** and **convergence order** are the strongest
  whole-program checks: a method that still converges at its design order
  down to the expected floor is working; one whose error stalls early has
  hit the precision.
- **Iterative solvers**: compare convergence histories, not only final
  values. A solver that needs twice the iterations at `lo` has changed.

## Compiler Flags That Change Values

Reassociating flags (fast-math and relatives, including their GPU
equivalents) invalidate two things this skill relies on (three where
bit-identical `hi` results were requested): error-free transformations
(compensated sums, double-word arithmetic,
`diff_of_products`), and the accuracy of division, square root and
transcendental functions on some GPU tool chains.

- Record the flags at setup. Do not change them on your own.
- If a remedy needs value-safe compilation, scope it as narrowly as the
  build system allows (one file, one target) and present it as a decision
  (trigger 8) with its measured cost.
- Prove it: build `assets/stable-math/test_stable_math.*` with the flags the
  project will actually use for that code. It fails when the compiler has
  removed the compensation.
- FMA contraction differs between compilers and between host and device.
  It changes results at the eps level, which is one reason the `hi` gates
  have a tolerance. Compare builds made the same way.

## Diagnosing Failures

| Symptom | Look at |
|---------|---------|
| G1: `hi` outside its budget, or a count changed | a literal or math call changed type; a rewritten tolerance decides a branch differently; the two builds used different flags |
| `lo` identical to `hi` | the profile did not reach the build (check the compile line), or a reference copy is being tested against itself |
| `lo` does not finish / different step count | tolerances, loop conditions, time accumulation (catalog E) |
| Inf / NaN / zero at `lo` only | range (catalog B): probe the intermediates |
| wrong by a factor, not by digits | an index, a unit, an uninitialized or truncated constant; not a precision problem |
| standalone test passes, application QoI fails | the workload's values lie outside the test's sweep (compare with the probe ranges); or the error accumulates over steps (analysis 1f) |
| error larger on the GPU than on the CPU at the same precision | approximate math functions, contraction; compare per function |
| passes once, fails on a larger case | accumulation length, integer range, values outside the assumed range |

Three failed attempts at the same step: stop and ask (SKILL.md).

## Rollback

Never automatic, at any engagement level.

1. Read `last_working_commit` from the working-tree `progress.json` (it
   names the last verified *code* commit, so it is an ancestor of the commit
   that recorded it); check that it exists and is an ancestor of `HEAD`
   (`git merge-base --is-ancestor "$GOOD" HEAD`). If not, change nothing and
   ask.
2. Show the user what would be undone:
   `git status --short`, `git log --oneline "$GOOD"..HEAD`.
3. Save a recovery patch first:
   `mkdir -p .precision-port/recovery && git diff --binary HEAD > .precision-port/recovery/{timestamp}.patch`;
   move agent-created untracked files there by explicit path.
4. With approval, `git revert --no-edit` the agent's commits after `$GOOD`,
   newest first, one at a time. No `reset --hard`, no history rewriting, no
   wildcard deletes. On a conflict: `git revert --abort`, stop, ask.
5. Write `$GOOD` back as `last_working_commit`, commit, re-read the plan,
   and ask how to proceed.
