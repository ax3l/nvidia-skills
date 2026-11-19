# Resolve Guide (Phases 2–5)

Turn the precision map into verified source changes.

- [The Remedy Ladder](#the-remedy-ladder)
- [Phase 2: Plan](#phase-2-plan)
- [Phase 3: Genericize](#phase-3-genericize)
- [Phase 4: Stabilize](#phase-4-stabilize) (4a–4f, per item)
- [Phase 5: Assess](#phase-5-assess)

## The Remedy Ladder

Repairs are ordered by how exact, how local and how cheap they are. Take the
**lowest rung that makes the item pass**. Climbing a rung needs a reason
that the lower one failed, recorded in the plan.

| Rung | Remedy | What changes | Decision trigger |
|------|--------|--------------|------------------|
| **L1** | **Mechanical genericity**: role alias, generic literals and math calls, tolerances scaled with the type's epsilon, conversions at boundaries | nothing at `hi` | none |
| **L2** | **Algebraic reformulation**: an exact identity that avoids the cancellation, as a named helper | rounding only | only if `hi` results move or `hi` slows beyond budget |
| **L3** | **Reordering and rescaling**: keep intermediates in range, combine constants at higher precision once, evaluate differences before products | rounding only | same |
| **L4** | **Compensated arithmetic**: error-free transformations for sums, dot products, products that cancel | extra operations; forbids value-unsafe compiler flags for that code | performance budget; compiler flags |
| **L5** | **Representation change**: store a difference instead of two large values (relative coordinates, perturbation from a reference state, integer step count plus fraction), normalized units, base-plus-offset integers | data structures, possibly I/O | always |
| **L6** | **Algorithm-level mixed precision**: low-precision solve with a higher-precision residual, iterative refinement, low-precision preconditioner | the algorithm | always |
| **L7** | **Wider arithmetic for a few values**: a native higher-precision island, or double-word (float-float) emulation where FP64 is scarce or absent | types, conversions at the island's boundary | always |
| **L8** | **Approximation** with a stated validity range and error bound: series for small arguments, asymptotic forms, regularization | results | always |
| **L9** | **Accept**: relax the QoI budget, or leave the function at `hi` | scope of the port | always |

Why this order: L1–L3 are identities in real arithmetic, so they can only
change rounding; they are verifiable by a unit test alone and keep the code
one formula. L4 is still exact but costs operations and constrains compiler
flags. L5 and L6 are exact too but reach beyond the function. L7 depends on
the hardware. L8 changes what is computed. L9 changes what was asked for.

The catalog of concrete patterns for L2–L7 is
[reformulation-catalog.md](reformulation-catalog.md). Which L7 variant pays
off is decided by the hardware class and R
([performance guide](performance-guide.md#choosing-wider-arithmetic)).

## Phase 2: Plan

Build `.precision-port/porting_plan.json` from `precision_map.json`. Plan
only what the analysis supports: no item without evidence, no remedy chosen
because it is familiar.

1. **Select items.** In this order: functions whose class blocks a QoI
   (`tolerance`, `range`, `integer`, then `cancellation`, `absorption`,
   `accumulation`, `ill_conditioned`), then hot functions that are `benign`
   or `mechanical` (they are where the port pays off). Leave the rest for a
   later round and list them under `deferred`.
2. **Choose one remedy per item**: the lowest rung that the evidence allows.
   `A ≫ κ` allows L2–L4; `A ≈ κ` does not, so start at L5.
3. **List the decisions each item will need** (triggers from SKILL.md), so
   the user sees them before work starts.
4. **Scope Phase 3**: the files and roles that must become generic for this
   round's items to build at `lo`. With an existing switch this may be
   empty.
5. **Order**: Phase 3 scope first; then items that other items depend on
   (shared helpers, a role's representation); then by QoI impact.

```json
{
  "round": 1,
  "problem": "main",
  "overall_comment": "Two cancellations and one absorption block energy_gain; ...",
  "genericize": {
    "roles": [{"name": "particle", "alias": "ParticleReal", "hi": "double", "lo": "float"}],
    "files": ["src/pusher.cpp"],
    "boundaries": ["checkpoint writer keeps double (io_policy: keep)"]
  },
  "items": [
    {
      "id": "I-001", "file": "src/pusher.cpp", "function": "kinetic_energy",
      "class": "cancellation", "remedy": "L2: sqrt1pm1(u^2/c^2)",
      "evidence": "38 bits cancelled, A = 8e6, kappa = 2",
      "assumed_range": "|ux| in [1e3, 1e9] m/s",
      "expected_decisions": ["hi results move (improve) below u/c = 1e-4"],
      "depends_on": []
    }
  ],
  "deferred": [{"function": "deposit", "reason": "benign and 2 % of time"}]
}
```

### User review gate

Present: the items with class, evidence and remedy; the roles; what Phase 3
will touch; every expected decision; what is deferred. Ask: "Does this plan
look right? Add, remove or reorder anything?" On approval, seed
`progress.json:items[]` by `id` (schema in the
[setup guide](setup-guide.md#progressjson-schema)): keep completed entries,
reset the others, and set `plan_complete: true`. A later change of plan
(another remedy, a dropped item) is a deviation: ask first.

## Phase 3: Genericize

Make this round's scope independent of its precision **without changing
what the `hi` build computes** beyond roundoff. This is rung L1, done for
whole files rather than per function, because half-generic code does not
build at `lo`.

Follow the flavor guide for the syntax
([C/C++](../flavors/cpp/precision-types.md),
[Fortran](../flavors/fortran/precision-types.md)) and the
[maintainability guide](maintainability-guide.md) for the conventions. The
work, in order:

1. **Roles and aliases.** One header or module defines the role aliases and
   maps `PRECISION_PROFILE` to them. Nothing else in the code names a
   concrete floating-point type for role data.
2. **Declarations** in scope switch to the alias of their role.
3. **Literals** take the role's type explicitly, so that no expression is
   silently promoted to double (C/C++) or silently truncated to single
   (Fortran).
4. **Math calls** use the type-generic spelling.
5. **Tolerances and guards** that encode a precision (`1e-12`, `1e-300`,
   `DBL_EPSILON`) become expressions of the type's own limits. If the
   tolerance was a physical statement rather than a numerical one, leave the
   number and say so in a comment. If it is unreachable at `lo`, that is a
   decision (class `tolerance`), not a mechanical edit.
6. **Boundaries** convert once, explicitly, where a type is fixed from
   outside: MPI datatypes, file formats (`io_policy`), library calls with
   precision-specific entry points, C/Fortran interop. Name the boundary;
   do not scatter casts.
7. **Build wiring.** The build scripts translate `PRECISION_PROFILE`; each
   profile has its own build tree.

### Gate G1: `hi` within roundoff of REF

```bash
source .precision-port/env.sh && bash .precision-port/build_correctness.sh
source .precision-port/env.sh && bash .precision-port/run_correctness.sh
```

At `hi`, every QoI must be within its `hi` budget of the golden reference
(default: 1e-10 relative) and every count (steps, iterations) unchanged.
The edits of this phase change types and spellings, not meaning, so the
results may move in their last digits and no further: a literal rounded
once instead of twice, a contraction the compiler now applies differently, a
tolerance or guard written through the type's limits. Report the largest
deviation per QoI and go on; do not chase last-digit differences, and do not
bend the code to avoid them.

A QoI outside its `hi` budget, or a changed count, is a different matter: an
expression changed its type along the way (a literal that used to promote,
a math call that now resolves to another overload), or a rewritten tolerance
now decides a branch differently. Find it instead of accepting it.

Wrapping analysis probes (`PP_SUB`, `PP_ADD`) are removed in a commit of
their own before this phase starts, so that the mechanical diff reads
against the original source.

**Only if `budgets.hi_bit_identical` is set**, the gate is exact
(`qoi-compare.py` then compares the `hi` profile for identity; extract the
QoIs with all 17 digits). Three things then need care that otherwise do
not: decimal literals routed through `long double` by a user-defined suffix
(the scanner lists them as `literal_long_double_route`; write those as
`Real(value)`); tolerances and guards, which keep their original value at
`hi` (the larger of the original literal and the type's limit; flavor
guide); and value-unsafe compiler flags, which can break identity for
reasons unrelated to the edit (run this gate with them switched off for
both REF and the edited source, and note it).

### Gate G1b: `lo` builds, runs, and is really `lo`

```bash
source .precision-port/env.sh && PRECISION_PROFILE=lo bash .precision-port/build_correctness.sh
source .precision-port/env.sh && PRECISION_PROFILE=lo bash .precision-port/run_correctness.sh || true
scripts/fp64-audit.py "$PRECISION_PORT_BUILD_DIR/lo/app" --functions '{scope regex}'
scripts/fp64-audit.py "$PRECISION_PORT_BUILD_DIR/lo/app" --functions '{scope regex}' --loops
```

The `lo` run may still miss its budgets; that is what Phase 4 is for. What
must hold now: it builds without new conversion warnings in scope, it runs
to the end, and the audit shows no FP64 left in the functions that were
made generic except at the named boundaries. Remaining FP64 instructions are
leaks: an unsuffixed literal, a `double` local, a math call bound to the
wrong overload. Record the `lo` errors per QoI as the starting point of
Phase 4, commit, and set `genericize_complete: true`.

## Phase 4: Stabilize

One plan item at a time, through 4a–4f. Each item is a natural subagent
task: clear input (the item, its evidence, the guides), clear success
criterion (its gates).

### 4a Scaffold the standalone test

If Phase 1 built the function's test for the demotion measurement, reuse it.
Otherwise create it now as described in the
[verification guide](verification-guide.md#the-standalone-test):

- a **frozen reference copy** of the original function at the original
  precision (test-only, never linked into the application);
- a driver that feeds both the reference copy and the current function the
  same inputs, covering the item's `assumed_range` **including its ends**;
- where the function is a self-contained formula, a **truth** evaluated at
  higher precision.

It builds in both profiles. Commit; set `phases.scaffold`.

### 4b Record the starting numbers

Run the test before changing the function and keep its output. These are
the "before" rows of the item's accuracy table:

| variant | precision | meaning |
|---------|-----------|---------|
| original | `hi` | how accurate the original really was (often less than assumed) |
| original | `lo` | what plain lowering does: the error this item removes |

Also time the function (or its enclosing workload) at `hi`: the base of the
performance comparison. Set `phases.baseline_numbers`.

### 4c Apply the remedy

Implement the planned rung following the
[maintainability rules](maintainability-guide.md):

- express the stable form through a **named helper** (from
  `assets/stable-math/` or a project-specific one next to it), not inline;
- leave a comment at the use site with the textbook expression, the reason,
  and the validity range;
- keep the function generic: no precision `#ifdef` inside the kernel. If the
  two precisions truly need different code, select with the type's traits
  in one place and record why.

Do not combine rungs in one commit. If the rung does not reach the gates in
4d after three honest attempts at *that rung*, stop: either the
classification was wrong (back to analysis) or the next rung is needed
(a plan deviation: ask). Set `phases.remedy` when it builds in both
profiles.

Tool support for finding an L2 form: Herbie searches for more accurate
rewrites of an expression over a given input range and reports their
accuracy and cost (`racket -l herbie shell`, input in FPCore with
`:precision binary32` and a `:pre` range). Treat its output as a candidate
to be verified by 4d, and prefer a form a maintainer can recognize.

### 4d Verify

Run the standalone test in both profiles and apply the gates of the
[verification guide](verification-guide.md#gates):

| Gate | Build | Requirement |
|------|-------|-------------|
| G2 | `hi` | matches the frozen reference within the `hi` budget; **or**, if it does not, is shown to be no less accurate than the original against the truth (then trigger 3 applies) |
| G3 | `lo` | amplification within the function-level budget over the whole assumed range, non-finite count zero, and better than the "original at `lo`" row |
| properties | both | exact special values, symmetry, monotonicity, sign — whatever the function promises |

Print the before/after accuracy table; it goes into the decision record and
the round report. Commit the test with the remedy; set `phases.verify`.

### 4e Measure

Compare speed with the protocol of the
[performance guide](performance-guide.md#measuring-a-change):

- `hi` after against `hi` before: the **cost** of the remedy;
- `lo` after against `hi` before: the **gain** of the port for this function.

Measure the function in isolation when it is small relative to the
workload's noise, and the workload always. The `hi` budget binds for the
workload and for hot functions; for a cold function, report its slowdown
with its share of the run. Set `phases.measure`.

Where two remedies are feasible for one item (a carry word or a
representation change; a compensated sum or a wide accumulator), measure
them side by side on the standalone test, accuracy and time in both
profiles, before either goes into the application. The table of the
variants is the evidence of the decision record.

### 4f Decide and integrate

1. **Decisions.** If the item hit any trigger (results moved at `hi`, cost
   beyond budget, an assumption, a representation change, an island, an
   approximation), write the [decision record](decision-guide.md) with the
   measured options and wait for the user. An item with an open decision is
   not integrated.
2. **Integrate.** The application now uses the repaired function. Run the
   full correctness gate in both profiles:

   ```bash
   source .precision-port/env.sh && bash .precision-port/run_correctness.sh
   source .precision-port/env.sh && PRECISION_PROFILE=lo bash .precision-port/run_correctness.sh
   ```

   `hi` must pass its budgets. With an approved `hi` drift, re-capture
   exactly the affected QoIs into `.precision-port/gold_hi/{problem}/`
   (`qoi-compare.py --gold-hi` then judges `hi` against them and everything
   else against the original gold; see the verification guide). The `hi`
   tolerance stays as it is; `gold_hi` is never refreshed just to make a
   later item pass. `lo` must not get worse for any QoI; it need not pass
   every budget until the round's last item.
3. **Commit and record.** Update the item (`status: "completed"`,
   `phases.integrate`, `commit_hash`, `decision_ids`), append
   `"file:function"` to `resolved_items`, set `last_working_commit` to the
   commit just made (the state change itself is committed with the next
   step).

## Phase 5: Assess

After the round's last item, measure the whole picture and report.

1. **Accuracy.** Run the correctness gate in every profile. For `lo`, every
   approved QoI must be within budget; list each with its error.
2. **Speed.** For each problem, with the timing protocol:
   `hi` now against REF (the accumulated cost, against
   `budgets.hi_slowdown_pct`) and `lo` now against REF (the gain).
3. **FP64 remaining.** Audit the `lo` build
   (`scripts/fp64-audit.py --allow ...` with the approved islands). For
   `fp64_policy: "absent"`, any unapproved FP64 in device code fails the
   round. For GPU targets, add the executed counts from Nsight Compute for
   the hot kernels.
4. **Open items.** Deferred functions, QoIs outside budget, decisions that
   were postponed, assumptions awaiting confirmation.

Present the scorecard, then ask whether to continue:

````markdown
## Precision Port — Round {N} Complete ({problem})

### Accuracy at lo (against REF)
| QoI | budget | error | status |
|-----|--------|-------|--------|

### Cost and gain
| build | time | vs REF | interval | verdict |
|-------|-----:|-------:|----------|---------|
| hi (ported source) | | {+x %} | {95 % interval} | {within budget / over} |
| lo (ported source) | | {speedup} | | |
Noise floor of this workload: {A/A interval}

### What was done
| item | function | remedy | accuracy before → after [eps] | hi cost | decisions |
|------|----------|--------|-------------------------------|---------|-----------|

### FP64 remaining in the lo build
{approved islands with their reason; leaks found and fixed; leaks open}

### Decisions taken this round
{id, one line each; see .precision-port/DECISIONS.md}

### Open
{QoIs outside budget, deferred functions, assumptions to confirm}

### Likely next steps
1. ...

### Continue?
Run another round on this problem, or stop here?
````

Record the round in `progress.json:rounds[]`:

```json
{
  "round": 1, "problem": "main",
  "items_resolved": ["src/pusher.cpp:kinetic_energy"],
  "assess": {
    "qois": {"energy_gain": {"lo_error": 3.1e-6, "budget": 1e-4, "pass": true}},
    "hi_slowdown_pct": 1.2, "hi_interval_pct": [-0.8, 3.1],
    "lo_speedup": 1.7, "fp64_unapproved": 0
  },
  "user_continued": null
}
```
