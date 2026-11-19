# Reformulation Catalog

Patterns for Phase 4, grouped by the class the analysis assigned. Each entry
names the symptom, the textbook form, the stable form and its cost. Helpers
in `code font` exist in `assets/stable-math/` (C++ header and Fortran
module); standard-library functions are marked *(std)*.

Use the catalog after the analysis, not instead of it: a rewrite that the
measurements do not ask for only costs readability.

- [A. Cancellation](#a-cancellation-rung-l2)
- [B. Range: overflow and underflow](#b-range-overflow-and-underflow-rungs-l3-l5)
- [C. Accumulation](#c-accumulation-rung-l4)
- [D. Absorption and representation](#d-absorption-and-representation-rung-l5)
- [E. Tolerances, branches and indices](#e-tolerances-branches-and-indices-rung-l1)
- [F. Algorithm-level mixed precision](#f-algorithm-level-mixed-precision-rung-l6)
- [G. Wider arithmetic for a few values](#g-wider-arithmetic-for-a-few-values-rung-l7)
- [H. Approximations](#h-approximations-rung-l8)

## A. Cancellation (rung L2)

**Symptom:** `CANCELLATION` at a subtraction, amplification far above the
condition number. The result is small compared with its operands, and the
operands carry rounding error.

**Principle:** find an algebraically identical expression in which the small
result is *computed* (as a product, a quotient, a dedicated function)
instead of being *left over* from a subtraction.

| Textbook | Stable | Helper | Loses digits when |
|----------|--------|--------|-------------------|
| `sqrt(1 + x) − 1` | `x / (sqrt(1 + x) + 1)` | `sqrt1pm1(x)` | `\|x\| ≪ 1` |
| `sqrt(a) − sqrt(b)` | `(a − b) / (sqrt(a) + sqrt(b))` | — | `a ≈ b` |
| `a² − b²` | `(a − b)(a + b)` | `diff_of_squares(a, b)` | `a ≈ b` |
| `a·b − c·d` | FMA-corrected difference | `diff_of_products(a, b, c, d)` | products nearly equal |
| `1 − cos(x)` | `2 sin²(x/2)` | `one_minus_cos(x)` | `\|x\| ≪ 1` |
| `exp(x) − 1` | `expm1(x)` | *(std)*, Fortran `sm_expm1` | `\|x\| ≪ 1` |
| `log(1 + x)` | `log1p(x)` | *(std)*, Fortran `sm_log1p` | `\|x\| ≪ 1` |
| `log(1 − exp(x))`, `x < 0` | `log(−expm1(x))` or `log1p(−exp(x))` | `log1mexp(x)` | `x → 0⁻` |
| `log(1 + exp(x))` | `x + log1p(exp(−x))` for `x > 0` | `log1pexp(x)` | large `x` (overflow) |
| `log(a) − log(b)` | `log(a / b)`, or `log1p((a − b)/b)` when `a ≈ b` | — | `a ≈ b` |
| `1/a − 1/b` | `(b − a) / (a·b)` | — | `a ≈ b` |
| `x − y` then `/ (x + y)` etc. | keep the difference as the stored variable | section D | always, if `x ≈ y` by construction |
| roots of `a x² + b x + c` | larger root by the formula with the sign of `b`, the other as `c / q` | `quadratic_roots(a, b, c)` | `b² ≫ 4ac` |
| `acos(u·v)` for the angle between unit vectors | `atan2(\|u × v\|, u·v)` | — | small angles |
| `sqrt(x² + y²)` | `hypot(x, y)` | *(std)*; three components `norm3` | overflow/underflow of the squares |
| `a + t·(b − a)` | endpoint-exact two-branch form | `lerp(a, b, t)` (C++20: `std::lerp`) | must hit `b` exactly at `t = 1` |
| `mean(x²) − mean(x)²` | two passes: mean, then mean of squared deviations; or Welford's running update | — | small variance relative to the mean |

Domain examples that are the same identities in disguise:

- **Relativistic kinematics.** With `u = γv`: `γ − 1 = sqrt1pm1(u²/c²)`,
  so the kinetic energy `(γ − 1) m c²` is `m u² / (γ + 1)`. And
  `1 − β² = 1/γ²`: never form `1 − v²/c²` from a velocity close to `c`;
  carry `u` and derive `γ = sqrt(1 + u²/c²)`.
- **Small differences of exponentials** (decay, attenuation, Boltzmann
  factors): `exp(−a) − exp(−b) = −exp(−a)·expm1(a − b)`.
- **Geometry**: areas and orientation tests are differences of products;
  use `diff_of_products`. Predicates that must be *exact* (inside/outside)
  need adaptive exact arithmetic or stay at the higher precision (L7).

Notes:

- `diff_of_products` and every FMA-based form need a hardware FMA and
  value-safe compilation for that code.
- A reformulation moves `hi` results exactly where the original was
  inaccurate. That is the improvement, and it is also decision trigger 3:
  regression baselines will change in those digits.
- If no identity exists because the *inputs* already contain the damage
  (the function receives `x` and `y` and must return `x − y`), the function
  is ill-conditioned in this representation: go to section D.

## B. Range: overflow and underflow (rungs L3, L5)

**Symptom:** `OVERFLOW` / `UNDERFLOW` / `SQUARE_*` flags; non-finite or zero
results at `lo`; passes with FP32 significand and FP64 exponent range.
FP32 holds magnitudes from about 1e-38 to 3e38, FP16 from 6e-5 to 65504, so
products of physical constants in SI units leave the format long before the
final result does.

| Situation | Remedy | Rung |
|-----------|--------|------|
| A product of constants underflows or overflows (`q²`, `m·ε₀`, `ħ²`, `n²`) although the combined coefficient is ordinary | Compute the combined constant **once at higher precision**, at compile time or setup, and store the result in the working type. Name it for what it means. | L3 |
| An intermediate leaves the range though the result fits (`(a·b)/(c·d)`) | Reorder so that magnitudes alternate: `(a/c)·(b/d)`. Divide before multiplying when the factors are large, multiply first when they are small. | L3 |
| Sum of squares, norms | `hypot`, `norm3`, or scale by the largest component first | L3 |
| Products of many factors (likelihoods, partition sums) | Work with logarithms; `logsumexp` with the maximum pulled out | L3 |
| Variables themselves sit near the edge of the format across the code | **Normalized units**: choose code units in which typical values are of order 1 (natural units, a reference length/time/density). Convert at input and output. | L5 |
| "Infinity" and "zero" stand-ins (`1e300`, `1e-300`, `1e99`) | Use the type's own `max()`, `min()`, `epsilon()` or an explicit flag. A literal outside the format becomes Inf or 0 silently. | L1 |
| Values decay into the subnormal range | Decide with the user whether flush-to-zero is acceptable: subnormals are slow on many CPUs and absent in some emulated formats, and flushing changes results near zero. | decision |

A code in SI units is the common case: record in the plan which quantities
approach the format's limits (from the probe summary) and prefer the local
L3 remedies when only a few coefficients are affected. Changing the unit
system (L5) touches input, output and every hard-coded constant, so it is a
decision with a migration plan.

## C. Accumulation (rung L4)

**Symptom:** class `accumulation`; error grows with the number of terms;
`WIDE_RANGE`; a global diagnostic (total energy, charge, mass) is among the
worst QoIs although every term is accurate.

A naive sum of `n` terms can lose about `log2(n)` bits in the best case and
all of them when terms cancel. In FP32 a sum over 10^8 cells has few correct
digits left.

| Technique | Accuracy | Cost per term | Use when |
|-----------|----------|---------------|----------|
| **Pairwise (tree) summation** — `pairwise_sum` | error grows like `log n` | none | the loop can be restructured; reductions in most parallel frameworks are already trees |
| **Compensated summation** — `CompensatedSum`, `compensated_sum` (two_sum based) | as if summed in twice the precision | 6 extra additions, 2 words of state | long sums, cancelling terms, no wider type on the hot path |
| **Compensated dot product** — `compensated_dot` | as if in twice the precision | two_prod + two_sum per term | norms, inner products of nearly orthogonal vectors |
| **Wider accumulator** — sum the low-precision terms into a `double` (or double-word) variable | exact enough for any realistic `n` | one conversion + one wide add per term | `fp64_policy: "native"`; diagnostics off the hot path anywhere |
| **Carried running value** — `add_with_carry(value, carry, increment)` | value + carry tracks the exact total to about one rounding of the amount accumulated | 6 extra additions, one more word of state **per element** | a state variable advanced every step by increments far below its own magnitude (class `absorption`), when changing what is stored (section D) is not worth it |

Guidance:

- **Classic Kahan summation is not the recommended form.** Its correction is
  only exact when the running sum is larger than the term added; it degrades
  on data with wide dynamic range or cancelling terms. The two_sum based
  form used by the helpers has no such condition.
- **Parallel reductions** must combine partial accumulators with their
  compensation (`CompensatedSum::merge`), in every thread, rank and device
  kernel. A compensated loop inside a plain MPI sum throws the gain away at
  the last step. For MPI, reduce the (sum, compensation) pairs with a custom
  operation, or reduce in the wider type.
- **Which one on which hardware:** on `native` devices a `double`
  accumulator is the simplest accurate choice. On `rationed` devices the
  conversion and the FP64 add are the expensive part: use the compensated
  form in the working type. On `absent` devices only the compensated form
  exists. Measure; the decision is recorded.
- **Compiler flags.** Value-unsafe math removes the compensation without a
  warning. Compile these helpers value-safe (per file or per function), and
  run the helper self-test with the project's flags to prove it.
- **Diagnostics are cheap to make exact.** Sums that are computed once per
  step for output can simply run in the wider type on the host.

Running statistics: update mean and variance with Welford's recurrence
instead of accumulating `Σx` and `Σx²`.

**A carry word per element** is compensated summation applied to a state
variable, so it is rung L4: the variable keeps its meaning, the layout gains
one word that has to travel with it (copies, sorting, communication, restart
files). Two limits to state when proposing it:

- It makes the *addition* exact, not the *increment*. `rate * dt` is rounded
  the same way every step, so the state still drifts by about one epsilon of
  the total amount accumulated. Budgets tighter than that need the
  parameters themselves at a higher precision.
- Its cost appears in both profiles unless the form is selected by type
  (flavor guide, "Selecting a form by precision"). Weigh the two with
  numbers: shared code that is slower at `hi`, or a small helper that
  branches by precision and needs both branches tested.

**Scatter-add accumulators** (deposition onto a grid, histograms, tallies:
many threads add into shared cells, usually with an atomic add) do not take
a two-word compensated accumulator: one atomic operation cannot update both
words consistently. In order of preference:

1. **Often nothing is needed.** A cell that receives tens to thousands of
   terms of similar size and is reset every step loses a few bits at most.
   Check the probe (operand gap, `WIDE_RANGE`) before adding machinery.
2. **Two-level accumulation** when cells accumulate over many steps (time
   averages, dose, tallies): scatter each step's contributions into a
   working-precision scratch array with the plain atomic add, then fold the
   scratch array into the long-lived accumulator once per step with a
   per-cell carried or wide add. The fold is a plain map over cells, with no
   atomics, outside the scatter kernel.
3. **A wide atomic accumulator.** The simple choice on `native` hardware. On
   `rationed` hardware it puts a conversion and an FP64 add on every
   deposit: measure it against option 2 rather than assuming its cost in
   either direction.
4. **Sort, then reduce by key**: per-cell tree sums. An algorithm change,
   worth it only if the code sorts anyway.

## D. Absorption and representation (rung L5)

**Symptom:** `ABSORPTION` or `PARTIAL_ABSORPTION` (operand gap near or
beyond half the significand width); error proportional to the number of
steps; or class `ill_conditioned`: the function must return a small
difference of large stored values, so no formula can help.

The L4 alternative for absorption is a carry word per element (section C):
it leaves the variable's meaning alone and costs a word of state and six
additions per update. The entries below change *what is stored*. Which is
cheaper depends on the code: a carry word touches every place the variable
is copied; a representation change touches every place it is read. Where
both are feasible, measure both (accuracy, cost at `hi` and at `lo`, memory)
on the standalone test before one goes into the application.

**Principle:** store the small thing. A floating-point number has constant
*relative* resolution; if the quantity that matters is a small deviation
from something large, keep the deviation as the variable and the large part
as a (possibly integer, possibly higher-precision) reference.

| Situation | Representation | Notes |
|-----------|----------------|-------|
| Positions in a large domain with fine cells | integer cell index + offset within the cell in `[0, 1)` (`normalize_cell_offset`); or coordinates relative to the box / patch origin | The offset has full relative precision everywhere. Converting to a global coordinate is a boundary operation (output, diagnostics). Wrapping and bounds become integer operations. |
| `x += v·dt` where the increment is far below `ulp(x)` | relative coordinates as above; or the L4 carry word (`add_with_carry`, section C) | The carry doubles the storage of that variable; the representation change alters its layout for every reader. |
| Simulation time `t += dt` | integer step count; `t = t0 + n·dt` evaluated when needed, at the higher precision if it feeds a comparison | Loop on the integer, never on `t < t_end`. A handful of scalar doubles are free on any host. |
| Fields that are a small perturbation of a known background | store the perturbation; add the background analytically where needed | Changes the equations' source terms; verify conservation. |
| Differences of large, nearly equal state values (pressure gradients on a hydrostatic background, energies dominated by rest mass) | subtract the reference state analytically, evolve the remainder | Often already a known formulation of the model. |
| Angles and phases that grow without bound | reduce modulo the period as they are updated; keep integer turns separately if needed | `sin` of a large FP32 argument has no accurate digits left. |
| Table lookup `i = (x − x0)/dx` with `x ≈ x0` large | index relative to a local origin; carry index and fraction | |

Every entry here changes a data structure, an interface or a file format:
decision trigger 6, with a plan for input/output compatibility. Present it
with the alternative (usually L7: keep that variable wide) and their
measured costs.

## E. Tolerances, branches and indices (rung L1)

**Symptom:** class `tolerance` or `integer`; iteration counts change, loops
do not terminate, out-of-range indices, or QoIs jump rather than drift.

| Pattern | Problem at lower precision | Form that works at every precision |
|---------|----------------------------|-------------------------------------|
| `if (abs(r) < 1e-12)` | unreachable: the residual stagnates around `κ·eps` | tolerance as a multiple of the type's epsilon times the problem scale; **and** stop on stagnation (no progress in k iterations) |
| `while (t < t_end)` with `t += dt` | one step more or fewer | integer loop bound computed once |
| `for (x = a; x < b; x += h)` | trip count depends on rounding | integer counter, `x = a + i·h` |
| `a == b`, `x == 0.0` on computed values | true at one precision, false at another | decide what was meant: exact sentinel (keep, comment), or a tolerance |
| `(int)floor((x − x0)/dx)` | rounds up to `n` for a position inside the domain | `cell_index_clamped` |
| periodic wrap `if (x < 0) x += L` | returns exactly `L`, outside `[0, L)` | `wrap_periodic` |
| `max(x, 1e-300)`, `x + 1e-30` as guards | the literal is 0 at `lo` | the type's smallest normal number, or a physically meaningful floor. Only where `hi` must stay bit-identical: the larger of the original literal and the type's limit (`max(Real(1e-300), numeric_limits<Real>::min())`). Check first that the guard ever did anything: `1e-300 * small` underflows in double too |
| finite-difference step `h = 1e-8` (tuned as `sqrt(eps)` of double) | far below the optimum; the difference is noise | derive `h` from the type's epsilon and the scale of `x`; or use an analytic derivative |
| uniform random `u = bits / 2^32` in float | can return exactly 1.0 | `uniform01_f32` (24 bits); `uniform01_open_low_f32` for `log(u)` |
| counters and sums of ones kept in floating point | stop increasing at 2^24 (FP32), 2^11 (FP16) | integer counters |
| `float(i)` for large integers, e.g. global indices to coordinates | inexact above 2^24 | compute from local indices, or at the higher precision at the boundary |

A convergence tolerance that the user chose for accuracy (not as "machine
zero") may be unreachable at `lo`. That is not a mechanical edit: report the
attainable residual (the conditioning test of the analysis gives it) and ask
(decision trigger 9, or rung L6).

## F. Algorithm-level mixed precision (rung L6)

**Symptom:** class `ill_conditioned` in a solver, or a tolerance that `lo`
cannot reach, while the bulk of the time is spent in work that does not need
the precision.

| Pattern | Idea | Precision of each part |
|---------|------|------------------------|
| **Iterative refinement** | solve approximately, compute the residual of the *original* system, solve for a correction, repeat | factorization / inner solve at `lo`; residual and solution update at `hi` (or with compensated dot products) |
| **Inner–outer Krylov** | low-precision inner solver or preconditioner inside a flexible outer iteration | preconditioner at `lo` (or lower); outer iteration at `hi` |
| **Multigrid** | coarse levels and smoothers tolerate lower precision | fine-level residual at `hi` |
| **Newton with inexact Jacobian** | the Jacobian solve only needs a few digits | residual at `hi`, linear solve at `lo` |
| **Delta form** | iterate on the *update*, which is small and needs few digits, instead of the state | state at `hi` or in a perturbation representation, update at `lo` |
| **Matrix products on tensor cores** | split operands into low-precision slices, multiply exactly, recombine (Ozaki scheme) | vendor BLAS may offer it as FP64-emulation mode; applies to GEMM-dominated codes only |

The residual is the one quantity that must be accurate, because it is a
difference of nearly equal vectors by design. On devices without usable
FP64, compute it with `compensated_dot` or double-word arithmetic.

These change convergence behavior and iteration counts: decision trigger 7,
verified by convergence histories, not only by final values.

## G. Wider arithmetic for a few values (rung L7)

**Symptom:** a small number of values genuinely need more bits than the
target type has (the significand sweep says so), and no representation
change is acceptable.

| Option | What it is | Pays off when |
|--------|------------|---------------|
| **Native island** | those values keep the higher-precision type; explicit conversions at the island's boundary | `fp64_policy: "native"`; or host-side code anywhere. Keep whole loops inside the island: a conversion per element on a `rationed` device runs on the scarce FP64 units and can cost more than the arithmetic. |
| **Double-word arithmetic** | a value is an unevaluated sum of two working-precision numbers (about twice the significand, same exponent range); built from `two_sum` / `two_prod`. The CCCL FP component provides it as `cuda::fp32mp2` (float-float) and `cuda::fp64mp2`, with math functions, for host and device code | `rationed` (R above roughly 10 to 20, the cost of one emulated operation in working-precision operations) and `absent`. On `native` devices it is slower and less energy-efficient than plain FP64, except for division and square root. |
| **Emulated FP64** | IEEE double semantics built from integer and FP32 operations (`cuda::fp64emu`), bit-identical to `double` at its default accuracy | bit-exact double results are required where FP64 units are scarce or absent |

Rules:

- Double-word values have the **exponent range of the working type**: they
  fix precision, not overflow. Combine with section B if both are needed.
- Double-word arithmetic relies on exact error terms: value-safe compilation
  only.
- The island's boundary is part of the design. State where values enter and
  leave it, and audit the `lo` build so that FP64 appears there and nowhere
  else.
- Always present L7 next to the L5 alternative, with measured cost on the
  target device.

## H. Approximations (rung L8)

Only when no identity exists, and always as a decision with an error bound.

| Pattern | Example | What must be stated |
|---------|---------|---------------------|
| Series for small arguments | `x − sin(x) ≈ x³/6 − x⁵/120` for `\|x\| < x₀`; `(exp(x) − 1 − x)/x²` near 0 | the switch-over point `x₀`, the truncation error there, and that both branches agree at `x₀` to within the budget |
| Asymptotic form for large arguments | `log(cosh(x)) ≈ \|x\| − log 2` | same |
| Regularization | `1/sqrt(r² + ε²)` instead of `1/r` | the physical meaning of `ε` and its effect on the QoIs |
| Clamping | restrict an input to the range the formula was verified for | what happens outside, and that it is detected |
| Dropping a term below resolution | neglect a contribution smaller than `eps` times the leading one | the bound that guarantees it stays below |

The switch-over point of a two-branch formula depends on the precision:
derive it from the type's epsilon (the series error should equal the
cancellation error there) instead of hard-coding the double-precision value.

Test both branches and the neighborhood of the switch in the standalone
test, in both profiles.
