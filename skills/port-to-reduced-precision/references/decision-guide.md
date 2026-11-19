# Decision Guide

A reduced-precision port is a sequence of tradeoffs between accuracy, speed
at each precision, and the shape of the code. They belong to the developers
who will live with the result. This guide says when to stop, what to put in
front of them, and how to record what they chose.

## When to Stop and Ask

The nine triggers are listed in
[SKILL.md](../SKILL.md#approval-gates-vs-engagement-level). They apply at
every engagement level. In short: an approximation, an assumption, moved
`hi` results, `hi` slower than the budget, wider arithmetic in a `lo` build,
a changed data structure / unit system / file format, a changed algorithm,
changed compiler floating-point flags, a relaxed budget or dropped item.

Not a decision: rung L1 edits that leave `hi` within roundoff; an L2–L4 remedy
that stays within the `hi` accuracy and performance budgets; which exact
helper implements an identity; test layout. Do these and report them.

When in doubt whether something is a decision, it is one. The cost of
asking is a minute; the cost of a silent approximation in a production code
is found years later, if at all.

## What to Present

Present options side by side, with numbers that were measured in this
project, and a recommendation. Never present a single option as a question,
and never present an option without its evidence.

For each option give the same six facts:

| Fact | Content |
|------|---------|
| **What changes** | one or two sentences a maintainer can picture: which code, which data, which files |
| **Accuracy** | the before/after table rows of the standalone test (max / p99 error in eps at `hi` and `lo`), and the effect on each affected QoI against its budget |
| **Speed** | `hi` cost and `lo` gain with their intervals (function and workload), on the target device |
| **Validity** | the input range it was verified on; assumptions it relies on; what happens outside |
| **Code impact** | lines and files touched, new code paths, new dependencies or compiler-flag constraints, effect on I/O and restart compatibility |
| **Reversibility** | how hard it is to undo later |

Then state:

- what you **recommend** and the one reason that decides it. Where the
  evidence is close, recommend the option that shares the code between the
  precisions, even at a small `hi` cost inside the budget or a move of `hi`
  results that a truth confirms. A small helper that branches by precision
  is a fair option where the shared form costs too much; say that its
  `lo` branch is then code the `hi` build never runs;
- what you could **not measure** (no access to the target device, a tool
  missing, a workload that does not exercise the regime) — an option whose
  evidence is incomplete is labelled so, not presented as equal;
- whether the choice depends on the **hardware class**, and what would
  change the recommendation on another device.

### Template

````markdown
### Decision D-{nnn}: {one-line question}

**Item:** {id}, `{file}:{function}` · **Trigger:** {which of the nine}

**Why this needs you:** {what cannot be decided from measurements alone}

| | A: {name} | B: {name} | C: {name} |
|---|---|---|---|
| What changes | | | |
| Accuracy (function, eps) | hi: … lo: … | | |
| Accuracy (QoIs vs budget) | | | |
| Speed at hi vs REF | {+x % [lo, hi]} | | |
| Speed at lo vs REF | | | |
| Valid for / assumes | | | |
| Code impact | | | |
| Reversible? | | | |

**Recommendation:** {option} — {reason}.
**Not measured:** {gaps}.
**Would change on other hardware:** {yes/no, how}.
````

Keep it to what fits on a screen. The standalone test output, the
`bench-compare` JSON and the probe summary are the appendix: give their
paths.

### Typical decisions and their usual options

| Situation | Options to lay out |
|-----------|--------------------|
| A reformulation improves `hi` accuracy, so `hi` results move | accept and regenerate baselines (recommended when a truth confirms the new values: the code stays shared) · apply the stable form only at `lo` (two paths) · leave the function at `hi` |
| A stable form costs more than the `hi` budget | accept · cheaper variant · precision-selected form · island |
| Positions lose resolution in a large domain | relative coordinates (data structure change) · keep positions wide (island, memory) · restrict the supported domain size (assumption) |
| A global sum is inaccurate | wider accumulator · compensated sum in the working type · pairwise · accept and document the attainable accuracy |
| A constant or intermediate leaves the format | precombined constants (local) · normalized units (global) |
| A solver tolerance is unreachable | iterative refinement with a wide residual · tolerance derived from the type · keep the solver wide |
| Value-unsafe compiler flags defeat compensation | value-safe flags for the affected files (measured cost) · a remedy that does not rely on exact error terms · accept the naive accuracy |
| An integer may exceed 32 bits on larger problems | keep 64-bit for that quantity · narrow with a guard and a documented maximum · local index plus wide base |
| Output or checkpoint precision | keep on-disk precision (convert at the boundary) · follow working precision (smaller files, restart caveats) |

## Recording Decisions

Two files, both committed:

- `.precision-port/decisions.json` — the record, one object per decision;
- `.precision-port/DECISIONS.md` — the same content as prose and tables,
  written for a developer who was not in the conversation. Regenerate it
  whenever a decision is added.

```json
{
  "id": "D-003",
  "date": "2026-11-18",
  "item": "I-004",
  "location": "src/tally.cpp:accumulate_dose",
  "trigger": "wider arithmetic in a lo build",
  "question": "How should the dose tally keep its small per-step deposits over 10^6 steps in single precision?",
  "options": [
    {"label": "A: per-step scratch array, folded into the tally with a carry word per cell",
     "accuracy": "lo: 2e-7 relative after 1e6 steps (plain float: 3e-2)",
     "hi_cost_pct": 0.9, "hi_interval_pct": [-0.4, 2.2], "lo_speedup": 1.8,
     "valid_for": "any run length", "code_impact": "tally gains one array; checkpoint format gains one field",
     "reversible": "moderate"},
    {"label": "B: the tally stays double", "accuracy": "lo: as REF",
     "hi_cost_pct": 0.0, "lo_speedup": 1.2,
     "valid_for": "devices with FP64", "code_impact": "one role stays wide; a conversion and an FP64 add per deposit",
     "reversible": "easy"}
  ],
  "recommendation": "A",
  "not_measured": "target cluster GPU; numbers are from the development machine",
  "chosen": "A",
  "chosen_by": "user",
  "status": "confirmed",
  "rationale": "must also run where FP64 is scarce",
  "evidence": [".precision-port/probes/main/round_1/summary.md",
               "src/precision_tests/test_accumulate_dose/"],
  "commit": "",
  "revisit_when": "a target with native FP64 becomes the only platform"
}
```

Rules:

- `chosen` and `chosen_by` are filled only after the user has answered, in
  their words where they gave a reason. An agent never records its own
  recommendation as the user's decision.
- **Delegated decisions.** If the user delegated the decisions for this run
  (SKILL.md, "Delegated decisions"), the agent takes its recommendation and
  records `"chosen_by": "agent (delegated)"` and
  `"status": "pending_review"`, with the user's delegating words in
  `rationale`. `status` becomes `"confirmed"` only when the user says so;
  until then every report lists the decision as open, and `DECISIONS.md`
  starts with the list of decisions awaiting review, each with what would
  have to be changed to take the other option.
- **Who writes the files.** Some agent environments do not let a delegated
  subagent write report-style Markdown files. `decisions.json` is the
  record; if a subagent cannot write `DECISIONS.md`, it returns the text and
  the agent that owns the port writes the file.
- Fill `commit` when the change lands. A decision that is later reversed
  gets a new record that references the old one; records are not edited
  away.
- **Assumptions are decisions too.** "Particle count per rank stays below
  2^31", "|u|/c stays below 10^3", "the domain is at most 10^5 cells
  wide": record each with where it is checked at run time, or that it is
  not.
- Where a source comment explains a repair, it stands on its own (what, why,
  valid range). It may mention the decision id; it must not require reading
  the record to be understood.

## At Wrap-up

`DECISIONS.md` ends with three lists the developers can act on:

1. **Assumptions in force**, each with its run-time guard or "unchecked".
2. **Costs accepted**: where `hi` became slower, by how much, for what.
3. **Left wide or unported**, with the reason and what would be needed to
   change it.
