#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A/B timing with a noise estimate, for small performance deltas.

A reduced-precision port is judged on deltas of a few percent (does the
unified source stay within the allowed slowdown at the original precision?).
Single runs cannot resolve that. This script runs two commands interleaved
(base, candidate, candidate, base, ...) so that slow drift such as thermal
throttling or a busy node hits both equally, then reports the difference of
the medians with a bootstrap confidence interval and a verdict against a
budget.

Usage:
    bench-compare.py --base "CMD" --cand "CMD" [--repeats 7] [--warmup 1]
                     [--budget-pct 5] [--metric-regex 'step time: ([0-9.eE+-]+)']
                     [--cpu-time] [--pin CPU[,CPU...]]
                     [--higher-is-better] [--json result.json] [--cwd DIR]
    bench-compare.py --aa "CMD" ...        # same command on both sides:
                                           # measures the noise floor

By default the wall time of each command is measured. With --metric-regex the
first capture group of the first match in the command's stdout is used
instead (for programs that print the time of their own compute phase, which
excludes start-up and I/O). With --cpu-time the user+system CPU time of the
command and its children is used, which is less sensitive to a busy machine
(serial and CPU-bound programs only; it says nothing about GPU or wait time).
--pin restricts the commands to the given CPUs (Linux), which matters on
machines whose cores differ in speed.

Verdicts (delta = candidate relative to base, positive = worse):
    better                the whole interval is below 0
    within_noise          the interval contains 0
    worse_within_budget   worse, and the whole interval is within the budget
    over_budget           the whole interval is beyond the budget
    inconclusive          the interval straddles the budget: add repeats or
                          reduce noise before deciding

Exit status: 0 better / within_noise / worse_within_budget, 1 over_budget,
3 inconclusive, 2 usage error or a failed command.
Only the Python standard library is required.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import statistics
import subprocess
import sys
import time

try:
    import resource
except ImportError:  # not available on every platform
    resource = None


def children_cpu_seconds():
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime + usage.ru_stime


def run_once(command, cwd, metric, timeout, cpu_time=False, pin=None):
    preexec = (lambda: os.sched_setaffinity(0, pin)) if pin else None
    cpu_before = children_cpu_seconds() if cpu_time else 0.0
    start = time.perf_counter()
    proc = subprocess.run(command, shell=True, cwd=cwd, capture_output=True,
                          text=True, timeout=timeout, preexec_fn=preexec)
    elapsed = time.perf_counter() - start
    if cpu_time:
        elapsed = children_cpu_seconds() - cpu_before
    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()[-5:]
        raise RuntimeError(f"command exited with {proc.returncode}: {command}\n"
                           + "\n".join(tail))
    if metric is None:
        return elapsed
    match = metric.search(proc.stdout)
    if not match:
        raise RuntimeError(f"--metric-regex did not match the output of: {command}")
    return float(match.group(1))


def relative_spread(values):
    """Robust relative noise: scaled median absolute deviation / median."""
    med = statistics.median(values)
    if med == 0:
        return 0.0
    mad = statistics.median(abs(v - med) for v in values)
    return 1.4826 * mad / abs(med)


def bootstrap_interval(base, cand, resamples=10000, seed=12345):
    """95 % percentile interval of the relative difference of the medians."""
    rng = random.Random(seed)
    deltas = []
    for _ in range(resamples):
        b = statistics.median(rng.choices(base, k=len(base)))
        c = statistics.median(rng.choices(cand, k=len(cand)))
        if b != 0:
            deltas.append(100.0 * (c - b) / b)
    deltas.sort()
    lo = deltas[int(0.025 * (len(deltas) - 1))]
    hi = deltas[int(0.975 * (len(deltas) - 1))]
    return lo, hi


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--base", help="reference command")
    parser.add_argument("--cand", help="candidate command")
    parser.add_argument("--aa", help="run this command as both sides (noise floor)")
    parser.add_argument("--repeats", type=int, default=7, help="timed pairs (default 7)")
    parser.add_argument("--warmup", type=int, default=1, help="untimed pairs first (default 1)")
    parser.add_argument("--budget-pct", type=float, default=5.0,
                        help="allowed worsening in percent (default 5)")
    parser.add_argument("--metric-regex", help="take the metric from stdout (first group)")
    parser.add_argument("--cpu-time", action="store_true",
                        help="measure user+system CPU time of the command instead of wall time")
    parser.add_argument("--pin", help="comma-separated CPU numbers to run the commands on (Linux)")
    parser.add_argument("--higher-is-better", action="store_true",
                        help="the metric is a rate, not a time")
    parser.add_argument("--label-base", default="base")
    parser.add_argument("--label-cand", default="candidate")
    parser.add_argument("--cwd", default=None)
    parser.add_argument("--timeout", type=float, default=None, help="seconds per run")
    parser.add_argument("--json", dest="json_path")
    args = parser.parse_args()

    if args.aa:
        base_cmd = cand_cmd = args.aa
    elif args.base and args.cand:
        base_cmd, cand_cmd = args.base, args.cand
    else:
        parser.error("give --base and --cand, or --aa")
    if args.repeats < 3:
        parser.error("--repeats must be at least 3")
    metric = re.compile(args.metric_regex) if args.metric_regex else None
    if args.cpu_time and metric:
        parser.error("--cpu-time and --metric-regex are mutually exclusive")
    if args.cpu_time and resource is None:
        parser.error("--cpu-time needs the 'resource' module (Unix)")
    pin = None
    if args.pin:
        if not hasattr(os, "sched_setaffinity"):
            parser.error("--pin needs os.sched_setaffinity (Linux)")
        try:
            pin = {int(c) for c in args.pin.split(",")}
        except ValueError:
            parser.error("--pin takes comma-separated CPU numbers")

    base, cand = [], []
    try:
        for i in range(args.warmup + args.repeats):
            # alternate the order inside each pair to cancel drift
            order = [("base", base_cmd), ("cand", cand_cmd)]
            if i % 2 == 1:
                order.reverse()
            for side, command in order:
                value = run_once(command, args.cwd, metric, args.timeout, args.cpu_time, pin)
                if i >= args.warmup:
                    (base if side == "base" else cand).append(value)
    except (RuntimeError, subprocess.TimeoutExpired, ValueError, OSError) as err:
        print(f"error: {err}", file=sys.stderr)
        return 2

    med_base, med_cand = statistics.median(base), statistics.median(cand)
    if med_base == 0:
        print("error: base median is zero", file=sys.stderr)
        return 2
    sign = -1.0 if args.higher_is_better else 1.0  # make positive = worse
    delta = sign * 100.0 * (med_cand - med_base) / med_base
    lo, hi = bootstrap_interval(base, cand)
    lo, hi = sorted((sign * lo, sign * hi))
    budget = args.budget_pct

    if hi < 0:
        verdict = "better"
    elif lo <= 0 <= hi:
        verdict = "within_noise"
    elif hi <= budget:
        verdict = "worse_within_budget"
    elif lo > budget:
        verdict = "over_budget"
    else:
        verdict = "inconclusive"

    result = {
        "base": {"label": args.label_base, "command": base_cmd, "values": base,
                 "median": med_base, "relative_spread": relative_spread(base)},
        "candidate": {"label": args.label_cand, "command": cand_cmd, "values": cand,
                      "median": med_cand, "relative_spread": relative_spread(cand)},
        "metric": "stdout regex" if metric else ("cpu seconds" if args.cpu_time else "wall seconds"),
        "pinned_to": sorted(pin) if pin else None,
        "higher_is_better": args.higher_is_better,
        "repeats": args.repeats,
        "delta_pct_worse": delta,
        "ci95_pct_worse": [lo, hi],
        "ratio_base_over_candidate": med_base / med_cand if med_cand else None,
        "budget_pct": budget,
        "verdict": verdict,
    }
    if args.json_path:
        with open(args.json_path, "w", encoding="utf-8") as handle:
            json.dump(result, handle, indent=1)
            handle.write("\n")

    unit = "" if metric else " s"
    print(f"| side | median{unit} | spread | n |")
    print("|---|---:|---:|---:|")
    for side in ("base", "candidate"):
        entry = result[side]
        print(f"| {entry['label']} | {entry['median']:.6g} | "
              f"{100 * entry['relative_spread']:.2f} % | {len(entry['values'])} |")
    direction = "worse" if delta > 0 else "better"
    print(f"\n{args.label_cand} vs {args.label_base}: {abs(delta):.2f} % {direction} "
          f"(95 % interval {lo:+.2f} % .. {hi:+.2f} %, positive = worse), "
          f"budget {budget:g} %")
    print(f"speed ratio base/candidate: {result['ratio_base_over_candidate']:.3f}"
          if not args.higher_is_better else
          f"rate ratio candidate/base: {med_cand / med_base:.3f}")
    print(f"VERDICT: {verdict}")
    if verdict == "inconclusive":
        print("The interval straddles the budget. Add repeats, lengthen the workload, "
              "or quiet the machine, then measure again.")
    return {"over_budget": 1, "inconclusive": 3}.get(verdict, 0)


if __name__ == "__main__":
    sys.exit(main())
