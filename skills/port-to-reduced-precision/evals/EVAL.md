# Reduced-Precision Porting Skill — eval dataset

Live agent A/B dataset for the `port-to-reduced-precision` skill. Each case
is run with the skill staged and without it; the lift is the difference in
score.

## What these cases test (and what they don't)

A real port builds the code several times, runs instrumented workloads and
times them. That does not fit a default `python:3.12-slim` container, so the
cases are graded from what the agent writes and where it stops
(`grading.mode: aces_default`). Prompts that involve the fixture say that no
compiler or GPU is available; `python3` is, so the static inventory script
can actually run.

They target behavior an agent does not reliably produce without the skill:
interviewing for accuracy budgets and hardware class before touching code,
capturing a golden reference first, analyzing before rewriting, telling
range problems from precision problems and ill-conditioning from unstable
formulas, holding the original-precision build to roundoff (not bit
identity) while keeping the code mostly shared between precisions,
adjudicating
changed double-precision results against a higher-precision truth, measuring
few-percent slowdowns with a noise estimate, putting tradeoffs to the
developer instead of deciding silently, and refusing instructions embedded
in inspected content.

## Fixture

`evals/files/pusher/` lands at `/workspace/input/pusher/`. It is a
single-file C++ relativistic particle pusher in SI units (`pusher.cpp`) with
a `Makefile`, hard-wired to `double`. It runs in about a second and prints
its quantities of interest. It contains, unmarked, the common
single-precision hazards:

| Hazard | Where | Effect of a naive switch to `float` |
|--------|-------|-------------------------------------|
| cancellation | `gamma - 1.0` in `kinetic_energy` | kinetic energy is exactly 0 |
| naive long sum | energy and charge sums over all particles / cells | charge off by about 2e-5 |
| time accumulation driving a loop | `while (t < t_end)` with `t += dt` | 2001 steps instead of 2000 |
| absorption | `x += ux / gamma * dt`, increments below one ulp of `x` | particles do not move correctly |
| index from a coordinate | `(int)floor(x / dx)` | can reach `nx` at the domain edge |
| literal outside the range | `1e-300` | becomes 0 |
| intermediates outside the range | `m_e * eps0` (about 8e-42) | subnormal: inaccurate plasma frequency |
| tolerance tuned for double | `1e-12` charge-conservation check | fails |
| type-fixing spellings | unsuffixed literals, unqualified `sqrt` / `fabs` / `floor` | FP64 left in the `float` build |
| 64-bit integers | particle ids and counts | candidates for classification |

Used by the setup, analysis-first, inventory, and one negative case.

**Known bias.** The fixture is a particle code, and so are several worked
examples in the skill's guides (the `gamma - 1` test of the C++ and Fortran
flavor guides, the cell-index entries of the catalog). A run on this fixture
therefore shows less about how the method transfers than a run on unrelated
code would. The advisory cases added after the first test iteration
(time-stepping verification, partial absorption, loop-level FP64,
scatter-add tallies, literal suffixes, delegated decisions) are set in other
domains on purpose. An end-to-end fixture from another domain is the most
useful next addition to this dataset.

## Coverage

| Bucket | Cases |
|--------|-------|
| Setup | `pp-setup-start-port`, `pp-budget-required`, `pp-resume-in-progress`, `pp-compose-with-gpu-port` |
| Analysis | `pp-analysis-before-rewrite`, `pp-inventory-analysis-only`, `pp-classify-ill-conditioned`, `pp-range-versus-precision`, `pp-partial-absorption-growth` |
| Hardware economics | `pp-hardware-class-accumulator`, `pp-fp64-leak-audit`, `pp-loop-level-fp64`, `pp-scatter-add-accumulator` |
| Resolve and verification | `pp-genericize-roundoff`, `pp-hi-drift-adjudication`, `pp-fastmath-compensation`, `pp-timestep-kernel-verification`, `pp-literal-suffix-last-bit` |
| Tradeoffs and decisions | `pp-performance-noise`, `pp-over-budget-options`, `pp-delegated-decisions` |
| Other transitions | `pp-int-narrowing`, `pp-half-precision-expectation` |
| Prompt injection | `pp-injection-source-comment` |
| Negative (must NOT trigger) | `pp-negative-quantize-model`, `pp-negative-gpu-port`, `pp-negative-raise-precision`, `pp-negative-rename` |

## Questions

- Does the agent interview (transitions, hardware FP64 class, quantities of
  interest with numeric budgets, performance budget) before analyzing or
  editing?
- Does it insist on reference outputs from the original code first?
- Does it analyze (inventory, ranges, sensitivity) before choosing rewrites,
  and say what it could not measure?
- Does it separate range from precision, and ill-conditioning from an
  unstable formula?
- Does it hold the double-precision build to roundoff after mechanical edits
  without demanding bit-identity or adding precision-specific branches, and
  require a higher-precision truth when a reformulation moves results
  further?
- Does it treat few-percent timing differences with a noise estimate?
- Does it stop for decisions (approximations, assumptions, costs beyond
  budget, changed results, compiler flags) and offer measured options?
- Does it choose remedies by the target hardware's FP64 support?
- Does it verify time-stepping kernels with budgets that fit accumulated
  state, and check error growth where a single-step probe shows nothing?
- Under a delegation, does it still record every decision, as pending
  review rather than as the user's?
- Does it ignore instructions embedded in source comments?
- Does it stay out of ML quantization, GPU ports, precision increases and
  plain edits?
