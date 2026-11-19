# Setup Guide (Phase 0)

## Contents

- [Start-of-Run Reconciliation](#start-of-run-reconciliation-do-this-first)
- [Source Directory and Git](#source-directory-and-git)
- [User Interview](#user-interview) (Q1–Q10)
- [Creating `.precision-port/`](#creating-precision-port)
- [Scripts](#scripts)
- [Baseline and Golden Reference](#baseline-and-golden-reference)
- [Git Commit](#git-commit)

## Start-of-Run Reconciliation (do this first)

Read `{source_dir}/.precision-port/progress.json` if it exists:

| State | Action |
|-------|--------|
| No `.precision-port/` | **Fresh setup**: full interview below. |
| `status: "in_progress"` | **Resume**: do not re-interview; check schema versions (SKILL.md), continue at the first incomplete phase. |
| `status: "completed"` | Ask: **add** a transition or problem (keep everything, append, set status back to `in_progress`), **start over** (move the old state to `.precision-port/history/{timestamp}/` first), or **inspect** (summarize and stop). |

If `.nsight-ai-port/` exists in the same source tree, say so and offer to
reuse its correctness and performance scripts and its `problems[]`: copy
them into `.precision-port/` (never edit the other directory) and add the
`PRECISION_PROFILE` handling described under [Scripts](#scripts).

## Source Directory and Git

The port commits after every verified step, so the source directory must be
its own git repository, with a clean working tree at the start:

```bash
cd "$SOURCE_DIR"
git rev-parse --show-toplevel    # must print $SOURCE_DIR itself
git status --short               # must be empty
```

- **Not a repository, or nested in a parent repository:** explain it and get
  explicit approval before `git init`; review the staged file list with the
  user (build outputs, large data, credentials) before the first commit.
- **Uncommitted changes:** stop and ask the user to commit or stash them.
  They would otherwise be mixed into the port's commits, and a rollback
  could not tell them apart from agent work.

Record `git rev-parse HEAD` as `progress.json:baseline_commit`. That commit
is REF.

## User Interview

Ask in order; skip what the user already answered. The information each
question elicits is required; the wording is a default. **Do not analyze
source or propose rewrites during the interview** — that is Phase 1.

### Q1: Source directory → `source_dir`

> "Where is the source code I should work with?"

Absolute path to the root of the source tree.

### Q1b: Detect language → `language`

Do not ask: count source files by extension under `source_dir`, set `cpp`
(C, C++, CUDA) or `fortran`, and confirm with the user. A mixed code base
uses the language of the code that holds the numerical kernels; say which
you picked.

### Q2: What to port → `transitions[]`

> "Which types should I port? Pick any that apply:"
>
> **a) float64 → float32** (default)
> **b) float32 → float16 / bfloat16**
> **c) 64-bit → 32-bit integers**

| Choice | Entry |
|--------|-------|
| (a) | `"fp64-to-fp32"` |
| (b) | `"fp32-to-fp16"` |
| (c) | `"int64-to-int32"` |

Read the transition file of each choice before Q5: it lists what that type
pair can and cannot represent, which shapes the accuracy budgets.

### Q3: Target hardware and goal → `target_hardware`

The same code change is a win on one device and a loss on another, so the
target is asked for explicitly.

> "What should the ported code run on, and what is the goal?"
>
> **a) Speed, on hardware where FP64 is fully supported** (CPUs; GPUs whose
> FP64 rate is about half their FP32 rate). Mixed precision is fine.
> **b) Speed, on hardware where FP64 is scarce** (GPUs whose FP64 rate is a
> small fraction of the FP32 rate). FP64 and conversions must leave the hot
> path.
> **c) It must run where FP64 does not exist.** No FP64 in device code at all.

| Choice | `fp64_policy` |
|--------|---------------|
| (a) | `"native"` |
| (b) | `"rationed"` |
| (c) | `"absent"` |

Record a one-line `description` of the device(s). For a CUDA GPU, get the
FP32:FP64 throughput ratio R from the vendor's figures or measure it
([performance guide](performance-guide.md#the-fp32fp64-ratio-r)) and store
it in `fp32_fp64_ratio` with its `ratio_source`. If the user names several
targets with different classes, record the most restrictive one and note the
others: a decision that is free on one may need to be revisited on another.

### Q4: Precision mechanism and strategy → `strategy`, `roles[]`

Detect before asking. Run the inventory and read only its header (the
"alias / switch candidates" and, for Fortran, the kind lists):

```bash
scripts/precision-scan.py "$SOURCE_DIR" --json .precision-port/inventory.json
```

Then confirm with the user which of these holds:

- **The project already has a precision switch** (a `Real` alias, a kind
  parameter, a build option). Record how it is selected in
  `precision_switch.existing_mechanism` and adopt its roles as they are.
  Phase 3 then only closes gaps.
- **No switch exists.** Choose the strategy:

> "How should precision be organized in the source?"
>
> **a) Roles** (recommended) — a handful of named aliases, one per kind of
> quantity (for example fields, particles, accumulators, time, indices).
> Each role can be single or double independently.
> **b) Global** — one alias for all floating-point data. Simplest; no way
> to keep, say, accumulators wider than the rest.

| Choice | `strategy` |
|--------|------------|
| (a) | `"roles"` |
| (b) | `"global"` |

For `"roles"`, propose the role list only after Phase 1 has shown which
quantities behave differently; at setup, record the roles the project
already has, or leave `roles` empty. Each entry:

```json
{"name": "particle", "alias": "ParticleReal", "covers": "positions, momenta, weights",
 "hi": "double", "lo": "float"}
```

Keep the number of roles small (two to five). Every role is a second axis
in the build matrix and a conversion at every boundary between roles.

### Q5: Quantities of interest and accuracy budgets → `qois[]`

Lower precision changes every result. Whether the change is acceptable is
decided per output, by the user, in numbers. Without a budget there is
nothing to verify against.

> "Which outputs must stay accurate, and how accurate?"

For each QoI record an `id`, what it is, how the correctness run extracts
it, and its `lo_budget`. Offer four ways to arrive at the number:

1. **State it.** "Total energy to 1e-5 relative; the density field to 1e-4
   of its peak."
2. **From the project's own tolerances.** Regression tests and convergence
   checks contain tolerances. Ask which are physically motivated and which
   merely record what double precision happened to deliver; only the former
   are budgets.
3. **Discretization yardstick.** Run REF at the working resolution and at a
   refined one (or with a smaller time step). Their difference estimates the
   discretization error that the results already carry. A precision error
   well below it (default: one tenth) cannot change a conclusion drawn from
   the simulation. Record both runs in the rationale.
4. **Decide after analysis.** Set `"status": "tbd"`. Phase 1 then reports the
   accuracy the `lo` build attains per QoI, and the budget is approved at the
   analysis gate. Phase 2 does not start with a `tbd` QoI.

```json
{
  "id": "energy_gain",
  "description": "kinetic energy gained over the run [J]",
  "kind": "scalar",
  "source": "stdout line 'energy gain [J]:'",
  "lo_budget": {"rel_tol": 1e-4},
  "hi_budget": {"rel_tol": 1e-10},
  "rationale": "discretization error at this resolution is 2e-3 (dt halved)",
  "status": "approved"
}
```

| Budget field | Meaning |
|--------------|---------|
| `rel_tol` | element-wise relative tolerance |
| `wmad_tol` | limit on `sum|a − ref| / sum|ref|` (defaults to `rel_tol`) |
| `abs_tol` | element-wise absolute tolerance (default 0), in the QoI's own unit: the natural budget for positions, times, phases and other quantities whose zero is arbitrary |
| `rel_floor` | floor on `|ref|` as a fraction of the field's peak (default 0); cells below it are judged against the floor |
| `exact` | values must be identical: step counts, iteration counts, integer results |

Guidance while collecting them:

- **Prefer fields and invariants over single sums.** A global sum hides
  cell-wise errors of opposite sign and is ill-conditioned when the mean is
  near zero. Conserved quantities and convergence orders are strong QoIs;
  "the output looks the same" is not one.
- **Chaotic or statistical outputs** need statistical QoIs (moments, spectra,
  distributions over an ensemble), not point-wise ones. Say so now rather
  than discovering it in Phase 5.
- **Include a count** (steps, solver iterations) as an `exact` QoI where
  the algorithm has one. A changed count is the earliest sign that a
  tolerance or a time accumulation reacts to the precision.
- A budget tighter than about 10 times the target type's epsilon (1e-6 for
  float32) is not attainable for a computed quantity. Say so and ask whether
  the quantity must then stay at the higher precision.
- **QoIs that jump.** A quantity that changes by a finite amount on a
  discrete event (a mean position when a particle wraps around a periodic
  boundary, a count of cells above a threshold, anything behind an `if`)
  can differ between two correct runs by far more than any rounding. Pair
  it with the event count as an `exact` QoI, prefer a formulation that is
  continuous across the event (compare modulo the period; use a smooth
  weight instead of a threshold), and check how close the reference run
  comes to an event: a QoI that passes only because no element happened to
  sit near the boundary is verified for this input alone.
- **Differences of large numbers** (an energy gain, a drift, a residual) are
  ill-conditioned by construction: with `κ = (|a| + |b|) / |a − b|`,
  roundoff-level changes of `a` and `b` appear `κ` times larger in the QoI.
  Compute `κ` from the reference run now, set `hi_budget.rel_tol` to `κ`
  times the default (1e-10) and note it in the rationale. Ask whether the
  `lo` budget was meant for the difference or for its operands.

### Q6: Performance budget → `budgets`

Repairs that make the code stable at `lo` can cost time at `hi`. The user
decides how much the original-precision build may slow down in exchange.

> "How much slower may the double-precision build become, in exchange for
> the same source being stable in single precision? I will measure every
> change and stop to ask if one exceeds this."

| Field | Default | Meaning |
|-------|---------|---------|
| `hi_slowdown_pct` | 5 | allowed slowdown of the `hi` build against REF, for each whole workload and for each hot function |
| `hot_function_share_pct` | 5 | a function is hot if it takes at least this share of REF's run time in some workload |
| `hi_slowdown_ceiling_pct` | 10 | never proposed beyond this without a redesign discussion |
| `hi_result_drift` | `"ask"` | `"ask"`: changes of `hi` results beyond roundoff are a decision; `"forbid"`: remedies that move `hi` results beyond roundoff are off the table |
| `hi_bit_identical` | `false` | `true`: the `hi` build must reproduce REF bit for bit at every step of the port |

Then ask the second half of the question:

> "By default the ported code runs mostly the same statements at every
> precision. The double build then agrees with today's results to roundoff
> (about ten digits per output), not bit for bit. If you need bit-identical
> double results, I can keep them, at a price: most repairs then branch by
> precision, and the double build no longer runs the repaired code. Which
> do you want?"

The default, `false`, is what this skill is built for: code that is mostly
shared between the precisions, which a developer can read and maintain.
With it, differences in the last digits of
the `hi` build are expected (a literal rounded another way, a tolerance
written through the type's epsilon, a stable formula that is slightly more
accurate) and are reported, not hunted down. Set `true` only on an explicit
yes; it implies `hi_result_drift: "forbid"`, makes every `hi` comparison
exact, and turns each repair that touches a `hi` value into a
precision-selected form (flavor guide) that the `hi` build never exercises.

A function below the hot share is reported with its own slowdown and with
what that means for the workload (47 % of a function that takes 0.1 % of the
run is 0.05 % of the run). It is not a decision by itself. Ask now whether
any cold function is hot somewhere that is not being measured (another
workload, a library entry point used by other codes); those are treated as
hot.

### Q7: Performance workload(s) → `performance_mode`, `problems[]`

> "Which workload should I use to measure performance and to observe the
> values that flow through the code? One case, or a sequence that exercises
> different code paths?"

Each problem is one program invocation with its own build and run script
(`build_performance_{id}.sh`, `run_performance_{id}.sh` under
`.precision-port/`; a single problem may use the unsuffixed names and id
`"main"`). Entry format:

```json
{"id": "main", "description": "", "build_performance": ".precision-port/build_performance.sh",
 "run_performance": ".precision-port/run_performance.sh", "working_dir": "",
 "notes": "", "expected_exit_code": 0, "covers": ""}
```

Two properties matter more here than for a plain profile:

- **Representative values.** Phase 1 records the value ranges of *this* run.
  A toy input with every value near 1 will certify a code that breaks on
  production inputs. Ask for inputs with production-like magnitudes, units
  and run length (step count matters for accumulations), scaled down in size
  only.
- **Timeable.** The criterion is the measured noise, not the run time: the
  interval of an A/A comparison must be narrower than the budget. Runs
  dominated by start-up (well under a second of compute) rarely manage.
  Measure it once now:

  ```bash
  scripts/bench-compare.py --aa "bash .precision-port/run_performance.sh" --repeats 5
  ```

  On a shared machine, or one whose cores differ in speed, add
  `--pin {cpu}` and, for a serial CPU-bound program, `--cpu-time`.

  If the reported interval is wider than the `hi_slowdown_pct` budget, the
  workload cannot resolve the budget: lengthen it, pin it, or have the
  program print the time of its compute phase and pass `--metric-regex`.
  Record the noise and how it was measured (metric, pinning) in the
  problem's `notes`; every later timing uses the same options.

### Q8: Correctness scripts → `build_correctness`, `run_correctness`

> "How do I build the code and check that its results are right?"

Copy existing scripts into `.precision-port/` (never edit the originals) or
author them there; see [Scripts](#scripts). `run_correctness` is the gate of
every later step, so it must extract **every QoI** and exit non-zero when one
leaves its budget. It may run several cases.

### Q9: Engagement level → `engagement`

> "How involved do you want to be in reviewing changes?"
>
> **a) Low** (default) — between approval points I work autonomously and
> report at the end of each round.
> **b) High** — after every major change I show the diff and wait.
>
> Either way I stop for: the scripts, the budgets, the analysis report, the
> plan, every tradeoff decision, and whether to run another round.

### Q10: Boundaries → `io_policy`, `fp_flags`

> "Should files the code writes (output, checkpoints) keep their current
> precision, or follow the working precision?"

| Choice | `io_policy` |
|--------|-------------|
| keep the current on-disk precision (default) | `"keep"` |
| write at the working precision | `"follow"` |
| decide per file format | `"ask"` |

Restart files deserve a separate sentence: a checkpoint written at `lo` and
read at `hi` (or the reverse) changes results at the restart.

Then report the value-changing compiler flags the inventory found (fast-math
and relatives, contraction, kind-promotion flags) and confirm them. Set
`fp_flags.value_unsafe` to `true` if any reassociating flag is active for
the code being ported, and record the flags. This matters twice: compensated
arithmetic is silently optimized away under such flags, and on some GPU tool
chains they replace division, square root and transcendentals by faster,
less accurate versions whose error is larger relative to float32 than to
float64. Changing the flags is a decision for the user, not a setup step.

## Creating `.precision-port/`

Create `{source_dir}/.precision-port/` with:

| File | From | Fill in |
|------|------|---------|
| `.gitignore` | `assets/gitignore-template` | — |
| `config.json` | `assets/config-template.json` | every interview answer; `tools` from `scripts/env-check.sh` |
| `progress.json` | `assets/progress-template.json` | `baseline_commit`; one `"pending"` entry per problem |
| `decisions.json` | `assets/decisions-template.json` | `project` |
| `inventory.json` | written by `scripts/precision-scan.py` in Q4 | — |

`schema_version` is copied from the templates and never edited.

`profiles` records what each role is in each profile. With an existing
switch, write down the mapping to the project's own options, for example:

```json
"profiles": {
  "hi": {"field": "double", "particle": "double"},
  "lo": {"field": "float",  "particle": "float"}
}
```

Additional profiles (one role lowered at a time) are added by Phase 1 when it
uses them; `hi` and `lo` always exist.

### `progress.json` schema

| Field | Purpose |
|-------|---------|
| `status` | `"in_progress"` or `"completed"` |
| `baseline_commit` | REF: the commit the golden reference was taken from |
| `last_working_commit` | SHA of the last verified *code* commit (`baseline_commit` until the first change). A commit cannot contain its own hash: the state file that records it is committed with the next step, or in a state-only commit, so the recorded SHA is always an ancestor of the commit that carries it |
| `current_problem`, `current_round` | active problem id and round (0 before the first) |
| `setup_complete`, `analysis_complete`, `plan_complete`, `genericize_complete` | phase flags of the active round |
| `items[]` | this round's plan items and their phase flags (seeded in Phase 2) |
| `resolved_items[]` | `"file:function"` of every item completed, cumulative |
| `problems[]` | per-problem status, REF timing, rounds completed; cumulative |
| `rounds[]` | one scorecard record per finished round; cumulative |

`problems[]` entry:

```json
{"id": "main", "status": "active", "ref_seconds": 0.412,
 "ref_noise_pct": [-1.1, 1.1], "ref_metric": "cpu seconds, pinned to CPU 2, 7 pairs",
 "rounds_completed": 0}
```

`status` is `"pending"`, `"active"` or `"completed"`.

`items[]` entry, seeded when the plan is approved:

```json
{
  "id": "I-003", "file": "src/pusher.cpp", "function": "kinetic_energy",
  "class": "cancellation", "remedy": "L2: sqrt1pm1",
  "status": "pending",
  "phases": {"scaffold": false, "baseline_numbers": false, "remedy": false,
             "verify": false, "measure": false, "integrate": false},
  "test_dir": "", "decision_ids": [], "commit_hash": ""
}
```

## Scripts

All scripts live in `.precision-port/`, are approved by the user before use,
and are the only build and run commands the port executes.

### One switch: `PRECISION_PROFILE`

Every build and run script reads one environment variable and translates it
into whatever the build system needs:

```bash
#!/usr/bin/env bash
# .precision-port/build_correctness.sh
set -euo pipefail
PROFILE="${PRECISION_PROFILE:-hi}"

case "$PROFILE" in
  hi) PRECISION_DEFINES="-DAPP_PRECISION=DOUBLE" ;;
  lo) PRECISION_DEFINES="-DAPP_PRECISION=SINGLE" ;;
  *)  echo "unknown PRECISION_PROFILE: $PROFILE" >&2; exit 2 ;;
esac

# One build tree per profile: two configurations must never share objects.
cmake -S "$PRECISION_PORT_SOURCE_DIR" \
      -B "$PRECISION_PORT_BUILD_DIR/$PROFILE" $PRECISION_DEFINES
cmake --build "$PRECISION_PORT_BUILD_DIR/$PROFILE" -j 6
```

Rules:

- The variable is passed as a prefix to the command
  (`PRECISION_PROFILE=lo bash .precision-port/build_correctness.sh`), never
  set in `env.sh`.
- Before Phase 3 has given the project a precision switch, only `hi` builds.
  The script must then **fail** for any other profile with a clear message,
  not fall back to `hi`: a "lo" run that is secretly double passes every
  test and proves nothing.
- Each profile gets its own build tree
  (`$PRECISION_PORT_BUILD_DIR/hi`, `/lo`, ...). Compiler and flag decisions
  live in the build system, not in the scripts.
- Keep value-affecting flags identical between profiles. A `lo` build with
  different optimization flags measures the flags, not the precision.
- Idempotent, non-zero on failure, limited parallelism on shared machines.

### `env.sh`

Static paths only:

```bash
export PRECISION_PORT_SOURCE_DIR="/absolute/path/to/project"
export PRECISION_PORT_BUILD_DIR="/absolute/path/to/project/build-precision"
```

### `run_correctness.sh`

The run script executes the case(s), extracts each QoI into
`<run_dir>/<qoi_id>.dat` (plain numbers, whitespace separated), and gates
them with the shipped comparison:

```bash
#!/usr/bin/env bash
# .precision-port/run_correctness.sh
set -euo pipefail
PROFILE="${PRECISION_PROFILE:-hi}"
cd "$PRECISION_PORT_SOURCE_DIR"
RUN_DIR=".precision-port/run/$PROFILE"
mkdir -p "$RUN_DIR"

"$PRECISION_PORT_BUILD_DIR/$PROFILE/app" inputs/case.in > "$RUN_DIR/stdout.txt"

# Extract the DATA, never compare raw stdout: formatting, timings and
# status lines differ between builds without being errors.
awk -F: '/^energy gain/  {print $2}' "$RUN_DIR/stdout.txt" > "$RUN_DIR/energy_gain.dat"
awk     '/^steps:/       {print $2}' "$RUN_DIR/stdout.txt" > "$RUN_DIR/steps.dat"
cp output/density.txt "$RUN_DIR/density.dat"

"$SKILL_DIR/scripts/qoi-compare.py" --config .precision-port/config.json \
    --gold .precision-port/gold/main --gold-hi .precision-port/gold_hi/main \
    --run "$RUN_DIR" --profile "$PROFILE"
```

- `SKILL_DIR` is the installed path of this skill; alternatively copy
  `scripts/qoi-compare.py` into `.precision-port/` so the gate does not
  depend on where the skill is installed. Say which you did.
- The comparison applies `hi_budget` (default: `rel_tol` 1e-10) for `hi`
  and the approved `lo_budget` for every other profile, and refuses to call
  a QoI verified while its status is `tbd`.
- `--gold-hi` names a directory that does not exist until a decision
  approves a change of `hi` values. From then on it holds the re-captured
  values of exactly those QoIs; the `hi` profile is compared against them,
  every other profile against the original gold
  ([verification guide](verification-guide.md#truth-and-adjudication)).
- Under `set -e`, a command whose empty result is acceptable (`grep` that
  matches nothing) needs `|| true`, or the script dies on its passing path.
- Extract numbers with more digits than the tightest budget needs: at
  least 12 significant for the default `hi` budget, all 17 if bit-identical
  results are required. A gate can only be as strict as the output it
  reads. If the program prints fewer digits and its output must not change,
  in order of preference: read the QoIs from a full-precision file the program
  already writes; or let the *build script* make a gate-only variant from a
  copy of the source in the build tree with the print format widened (the
  copy is generated, never committed, and the shipped program is
  unchanged); or run the gate on the digits there are and say so wherever
  that gate is reported.
- For large fields or binary formats, extract with the project's own tools
  into text, or compare with the test harness
  (`assets/test-harness/`, `field_budget`); the gate must still exit
  non-zero on failure.

### Performance scripts

`run_performance_{id}.sh` runs **exactly one** case and honors
`PRECISION_PROFILE` the same way; `build_performance_{id}.sh` mirrors the
correctness build unless the performance case needs a different
configuration. If the program can report the time of its compute phase,
print it on one recognizable line: `scripts/bench-compare.py --metric-regex`
then times the phase that matters instead of start-up and I/O.

`build_ref.sh` and `run_ref.sh` do the same for **REF**: they build
`baseline_commit` from a git worktree
(`git worktree add --detach "$PRECISION_PORT_BUILD_DIR/ref-src" {baseline_commit}`)
into `$PRECISION_PORT_BUILD_DIR/ref` and run the same performance case.
Every later timing alternates REF with the current build, so REF has to
stay runnable next to it for the whole port.

### Presenting the scripts

Show every script and get approval before running any of them:

> "I've drafted the build and run scripts. Please review them and tell me
> what to change. I need your approval before I use them."

## Baseline and Golden Reference

With the source still untouched at `baseline_commit`:

1. **Build and run REF:**
   ```bash
   source .precision-port/env.sh && bash .precision-port/build_correctness.sh
   ```
2. **Capture the golden reference.** Run the case(s), extract every QoI as
   the run script does, and store the files under
   `.precision-port/gold/{problem_id}/`. This is the only moment the
   original behavior is available untouched. Commit them.
3. **Self-check the gate:**
   ```bash
   source .precision-port/env.sh && bash .precision-port/run_correctness.sh
   ```
   must pass (REF against its own gold) and must fail when you perturb a
   gold value by more than the budget; restore the file afterwards. A gate
   that cannot fail is not a gate.
4. **Time REF.** Build and run each problem's performance pair once, then
   record the median of several runs as
   `progress.json:problems[].ref_seconds`, with the noise from Q7.
5. **Confirm nothing in the tree was modified:** `git status --short` shows
   only `.precision-port/`.

If any of this fails, do not proceed: fix the scripts with the user first.

## Git Commit

```bash
git add .precision-port/
git commit -m "precision-port: initialize configuration and golden reference"
```

Set `setup_complete: true`. `last_working_commit` is `baseline_commit`: no
code has changed yet.
