#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Gate a run's quantities of interest (QoIs) against the golden reference.

The correctness script of a reduced-precision port extracts every QoI of a
run into a small text file of numbers, `<run_dir>/<qoi_id>.dat`, exactly as
it did once for the reference build into `<gold_dir>/<qoi_id>.dat`. This
script compares the two with the budget that `config.json` records for the
active precision profile:

    profile "hi"   each QoI's `hi_budget` (default: rel_tol 1e-10)
    any other      each QoI's `lo_budget`, as approved by the user

A QoI that is a difference of large totals, or otherwise ill-conditioned,
needs an `hi_budget` scaled by its condition number, recorded with its
reason; the default would fail on roundoff-level changes upstream.

Budget fields (all optional except one of rel_tol / wmad_tol / exact):
    rel_tol    element-wise relative tolerance
    wmad_tol   limit on sum|a - ref| / sum|ref|   (default: rel_tol)
    abs_tol    element-wise absolute tolerance    (default 0)
    rel_floor  floor on |ref| as a fraction of the peak |ref| (default 0)
    exact      true: values must be identical (step counts, integer results)

An element passes if its absolute OR its relative difference is within
tolerance; the QoI passes if wmad is within wmad_tol and no element fails.
A QoI whose status is "tbd" has no approved budget yet: it is reported and
not gated, and the run cannot be called verified until it is approved.

Usage:
    qoi-compare.py --config .precision-port/config.json --gold GOLD_DIR
                   --run RUN_DIR [--profile hi|lo|...] [--gold-hi DIR]
                   [--exact] [--json out.json]

--profile defaults to $PRECISION_PROFILE, then "hi".
--gold-hi names a directory with re-captured references for the "hi" profile
only: a QoI whose file exists there is compared against it instead of the
original reference. Use it for QoIs whose double-precision values moved by an
approved decision. Every other profile is always judged against --gold.
--exact requires every QoI to be identical to the reference, whatever its
budget. It is for projects that asked for bit-identical results at the
original precision: with `budgets.hi_bit_identical: true` in the config,
the "hi" profile is compared this way automatically (extract the QoIs with
all their digits, e.g. %.17g, for this to mean bit-identical). By default
the "hi" profile is judged with its budgets: roundoff-level differences
from the reference are expected and pass.
Exit status: 0 all gated QoIs pass, 1 a QoI fails or is missing, 2 usage.
Only the Python standard library is required.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

DEFAULT_HI = {"rel_tol": 1e-10, "abs_tol": 0.0, "rel_floor": 0.0}


def read_numbers(path):
    values = []
    with open(path, "r", encoding="utf-8") as handle:
        for token in handle.read().replace(",", " ").split():
            values.append(float(token))
    return values


def compare(run, gold, budget):
    result = {"n": len(gold), "wmad": None, "max_rel": None, "n_exceed": 0, "worst": None}
    if len(run) != len(gold):
        return dict(result, verdict="FAIL", reason=f"{len(run)} values, reference has {len(gold)}")
    if not gold:
        return dict(result, verdict="FAIL", reason="no values to compare")
    if any(not math.isfinite(v) for v in run + gold):
        return dict(result, verdict="FAIL", reason="non-finite value")
    if budget.get("exact"):
        diff = [i for i, (a, b) in enumerate(zip(run, gold)) if a != b]
        if diff:
            return dict(result, n_exceed=len(diff), worst=diff[0], verdict="FAIL",
                        reason=f"not identical (first difference at index {diff[0]}: "
                               f"{run[diff[0]]!r} vs {gold[diff[0]]!r})")
        return dict(result, wmad=0.0, max_rel=0.0, verdict="PASS", reason="identical")

    rel_tol = budget.get("rel_tol")
    wmad_tol = budget.get("wmad_tol", rel_tol)
    if rel_tol is None:
        rel_tol = wmad_tol
    if wmad_tol is None:
        return dict(result, verdict="FAIL", reason="budget has neither rel_tol, wmad_tol nor exact")
    abs_tol = budget.get("abs_tol", 0.0)
    peak = max(abs(v) for v in gold)
    floor_v = budget.get("rel_floor", 0.0) * peak
    num = den = max_rel = 0.0
    worst, n_exceed = 0, 0
    for i, (a, b) in enumerate(zip(run, gold)):
        ad = abs(a - b)
        num += ad
        den += abs(b)
        scale = abs(b) + floor_v
        rd = ad / scale if scale > 0 else (math.inf if ad > abs_tol else 0.0)
        if rd > max_rel:
            max_rel, worst = rd, i
        if ad > abs_tol and rd > rel_tol:
            n_exceed += 1
    wmad = num / den if den > 0 else num
    result.update(wmad=wmad, max_rel=max_rel, n_exceed=n_exceed, worst=worst)
    if wmad > wmad_tol:
        return dict(result, verdict="FAIL", reason=f"wmad {wmad:.3e} > {wmad_tol:.3e}")
    if n_exceed:
        return dict(result, verdict="FAIL",
                    reason=f"{n_exceed} element(s) beyond rel {rel_tol:.3e} (worst index {worst})")
    return dict(result, verdict="PASS", reason="within budget")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--config", required=True)
    parser.add_argument("--gold", required=True, help="directory with <qoi_id>.dat of the reference")
    parser.add_argument("--run", required=True, help="directory with <qoi_id>.dat of this run")
    parser.add_argument("--profile", default=os.environ.get("PRECISION_PROFILE", "hi"))
    parser.add_argument("--gold-hi", dest="gold_hi",
                        help="re-captured references used for the hi profile only")
    parser.add_argument("--exact", action="store_true",
                        help="require identical values for every QoI")
    parser.add_argument("--json", dest="json_path")
    args = parser.parse_args()

    try:
        with open(args.config, "r", encoding="utf-8") as handle:
            config = json.load(handle)
    except (OSError, json.JSONDecodeError) as err:
        print(f"error: cannot read {args.config}: {err}", file=sys.stderr)
        return 2
    qois = config.get("qois") or []
    if not qois:
        print("error: config.json lists no QoIs; nothing can be verified", file=sys.stderr)
        return 2

    hi = args.profile == "hi"
    exact = args.exact or (hi and bool((config.get("budgets") or {}).get("hi_bit_identical")))
    rows, failed, ungated = [], 0, 0
    for qoi in qois:
        qid = qoi.get("id", "?")
        row = {"id": qid, "profile": args.profile}
        if exact:
            budget = {"exact": True}
        else:
            budget = (qoi.get("hi_budget") or DEFAULT_HI) if hi else qoi.get("lo_budget")
        gated = hi or exact or (qoi.get("status") == "approved" and budget)
        gold_path = os.path.join(args.gold, f"{qid}.dat")
        if hi and args.gold_hi and os.path.exists(os.path.join(args.gold_hi, f"{qid}.dat")):
            gold_path = os.path.join(args.gold_hi, f"{qid}.dat")
            row["reference"] = "gold_hi"
        run_path = os.path.join(args.run, f"{qid}.dat")
        try:
            gold, run = read_numbers(gold_path), read_numbers(run_path)
        except (OSError, ValueError) as err:
            row.update(verdict="FAIL", reason=f"cannot read data: {err}")
            failed += 1
            rows.append(row)
            continue
        row.update(compare(run, gold, budget or {"rel_tol": math.inf}))
        if not gated:
            row["verdict"] = "NOT GATED"
            row["reason"] = "no approved budget (status is not 'approved')"
            ungated += 1
        elif row["verdict"] != "PASS":
            failed += 1
        rows.append(row)

    if args.json_path:
        with open(args.json_path, "w", encoding="utf-8") as handle:
            json.dump({"profile": args.profile, "qois": rows}, handle, indent=1)
            handle.write("\n")

    print(f"QoI comparison, profile '{args.profile}', reference {args.gold}"
          + (" (exact: bit-identical results required)" if exact else ""))
    print("| QoI | n | wmad | max rel | verdict | note |")
    print("|---|---:|---:|---:|---|---|")
    for row in rows:
        fmt = lambda v: "-" if v is None else f"{v:.3e}"
        note = row.get("reason", "") + (" (against gold_hi)" if row.get("reference") else "")
        print(f"| {row['id']} | {row.get('n', '-')} | {fmt(row.get('wmad'))} | "
              f"{fmt(row.get('max_rel'))} | {row['verdict']} | {note} |")
    if ungated:
        print(f"\n{ungated} QoI(s) have no approved budget: this run is NOT verified for them.")
    if failed:
        print(f"\nQOI CHECK FAILED: {failed} of {len(rows)}")
        return 1
    print("\nQOI CHECK PASSED" + (" (gated QoIs only)" if ungated else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
