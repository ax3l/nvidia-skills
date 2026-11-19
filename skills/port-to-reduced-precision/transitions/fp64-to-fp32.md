# Transition: float64 → float32

Facts of the type pair for `"fp64-to-fp32"`. Read before the interview's
budget question and again when planning.

## The Two Formats

| | float64 (binary64) | float32 (binary32) |
|---|---|---|
| significand bits (with the implicit one) | 53 | 24 |
| machine epsilon | 2.2e-16 | 1.2e-7 |
| decimal digits | 15–16 | 6–7 |
| largest finite | 1.8e308 | 3.4e38 |
| smallest normal | 2.2e-308 | 1.2e-38 |
| integers exactly representable up to | 2^53 ≈ 9.0e15 | 2^24 = 16 777 216 |

Three consequences cover most of what goes wrong:

1. **29 fewer bits.** Any step that loses `k` bits by cancellation leaves
   `24 − k` instead of `53 − k`. A formula that silently wasted 30 bits in
   double still printed plausible numbers; in single it returns zero.
2. **A range that physical constants leave.** Squares and products of SI
   constants and of large densities overflow or underflow float32 although
   the final results are ordinary.
3. **Exact integers end at 16.8 million.** Counters, indices and sums of
   ones held in floating point stop being exact, and `x + 1 == x` beyond.

## What Is Attainable

Rules of thumb for setting and reading budgets (relative errors):

| Computation | Best case in float32 |
|-------------|----------------------|
| one operation or a library math call | about 6e-8 |
| a well-conditioned kernel of tens of operations | a few 1e-7 |
| a naive sum of `n` same-sign terms | about `sqrt(n)`·6e-8 typical, `n`·6e-8 worst; 1e-4 or worse for 10^7 terms |
| the same sum, pairwise or compensated | about 1e-7 |
| a result with condition number `κ` | `κ`·6e-8, whatever the formula |
| an iterative solver's residual | stagnates near `κ`·1e-7 |
| time-stepping over `N` steps | per-step error times `N` (systematic) or `sqrt(N)` (unbiased) |

A QoI budget below about 1e-6 for a computed quantity is not attainable in
float32. Budgets between 1e-6 and 1e-4 need the accumulation and
cancellation repairs to be done well; above 1e-4 most well-conditioned codes
pass once the hazards below are removed.

## Hazards, in the Order They Usually Appear

| # | Hazard | First seen as | Catalog |
|---|--------|---------------|---------|
| 1 | literals and math calls that fix or promote the type | does not compile, or FP64 left in the `lo` build | flavor guide, L1 |
| 2 | tolerances and guards written for double (`1e-12`, `1e-300`) | iteration counts change, loops do not end, division by zero | [E](../references/reformulation-catalog.md#e-tolerances-branches-and-indices-rung-l1) |
| 3 | time and loop variables accumulated in floating point | a different number of steps | E, [D](../references/reformulation-catalog.md#d-absorption-and-representation-rung-l5) |
| 4 | indices computed from coordinates | out-of-range access at domain edges | E |
| 5 | constants and intermediates leaving the range | Inf, NaN or zero | [B](../references/reformulation-catalog.md#b-range-overflow-and-underflow-rungs-l3-l5) |
| 6 | cancellation in formulas | a QoI with no correct digits | [A](../references/reformulation-catalog.md#a-cancellation-rung-l2) |
| 7 | small increments onto large values | quantities that stop evolving; or, with a gap well short of 24 bits, an error that grows with the steps and that no single-step check shows | [C](../references/reformulation-catalog.md#c-accumulation-rung-l4) (carry word), D |
| 8 | long sums and global diagnostics | conserved quantities drift or are noisy | [C](../references/reformulation-catalog.md#c-accumulation-rung-l4) |
| 9 | ill-conditioned sub-problems (solvers, differences of large states) | a QoI stuck just outside its budget | [F](../references/reformulation-catalog.md#f-algorithm-level-mixed-precision-rung-l6), [G](../references/reformulation-catalog.md#g-wider-arithmetic-for-a-few-values-rung-l7) |

Work in this order: the early ones change control flow and make the later
measurements meaningless until they are fixed.

## Quantities That Often Stay Wide

Candidates for a wide role in every profile, to be confirmed by the analysis
and decided with the user:

- **simulation time** and step counters (a handful of scalars; free);
- **global accumulators and diagnostics** (cheap off the hot path; on the
  hot path the choice between a wide and a compensated accumulator depends
  on the hardware class);
- **coordinates in large domains**, unless a relative representation is
  adopted;
- **reference and background states** that perturbations are measured
  against;
- **input parsing and unit conversion**, which run once.

On hardware where FP64 is scarce or absent, "stays wide" on the device must
be replaced by a representation change or by double-word arithmetic; on the
host it remains free.

## Things Specific to This Pair

- **Subnormals arrive 270 decades earlier.** Decaying quantities reach the
  float32 subnormal range (below 1.2e-38) in ordinary runs. Subnormal
  arithmetic is slow on many CPUs and flushes to zero on some devices and
  under some compiler options: results near zero may differ between
  platforms. Decide deliberately.
- **Math library accuracy.** Single-precision library functions are usually
  accurate to about one unit in the last place on CPUs; device math
  libraries and fast-math modes may be several units off or use approximate
  division and square root. A few eps matter more when there are only 24
  bits: measure per function on the target, not only on the host.
- **Random numbers.** A float32 uniform has 24 random bits and must not
  return 1; distributions built from `log(u)` lose their far tails (the
  smallest positive `u` is 2^-24). Generators with 64-bit state can stay as
  they are; only the conversion to a float changes.
- **Mixed precision has a price.** Every crossing between float32 and
  float64 is a conversion instruction; on devices with scarce FP64 it runs
  at the FP64 rate. A kernel that is "mostly single" can be slower than the
  all-double one. Count conversions (Nsight Compute, FP64 audit) before
  recommending a mixed design.
- **Memory.** Halving the bytes also halves checkpoint and output size when
  `io_policy` follows the working precision, and changes restart
  reproducibility: say both.

## Done Means

- every approved QoI within its budget at `lo`, at the production run
  length;
- step and iteration counts explained where they differ from REF;
- no unapproved FP64 in the `lo` build's hot code;
- `hi` within its accuracy and performance budgets against REF;
- every assumption about value ranges recorded and, where possible,
  guarded.
