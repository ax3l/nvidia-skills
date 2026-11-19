#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Summarize precision_probe output against a target number format.

Reads the JSON-lines files written by assets/probe/precision_probe.hpp or
precision_probe.f90, merges sites across processes (MPI ranks, repeated
runs), and judges every site against the format the code is being ported to:

  range sites   overflow / underflow in the target, squares that leave its
                range, integer-valued data beyond exact representation,
                dynamic range wider than the significand
  addsub sites  cancellation (leading bits lost) and absorption (operand gap:
                how many bits of the smaller term survive) relative to the
                target's significand
  demote sites  measured error amplification of a lower-precision evaluation
  count sites   integers that do not fit the narrow integer type

Usage:
    probe-summary.py [--target fp32|fp16|bf16] [--int-target int32|uint32]
                     [--amp-max 16] [--json summary.json] [--fail-on-blocker]
                     PATH [PATH ...]

PATH is a .jsonl file or a directory searched for precision_probe.*.jsonl.
Exit status: 0 (also when blockers are reported: this is an analysis, not a
gate), 2 = usage error; with --fail-on-blocker, 1 if any site is a blocker.

A site without a flag is not a clean bill of health: the summary judges
single operations. An accumulation that loses a few bits per step in the same
direction can exceed a budget over a run without any single step looking
bad; check error growth over the run length for every accumulation.
Only the Python standard library is required.
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import sys

# name: (significand bits incl. the implicit one, max finite, min normal)
FLOAT_TARGETS = {
    "fp32": (24, 3.4028234663852886e38, 1.1754943508222875e-38),
    "fp16": (11, 65504.0, 6.103515625e-05),
    "bf16": (8, 3.3895313892515355e38, 1.1754943508222875e-38),
}
INT_TARGETS = {
    "int32": (-2**31, 2**31 - 1),
    "uint32": (0, 2**32 - 1),
}
SOURCE_BITS = {"float64": 53, "float32": 24, "float_ext": 64}


def load(paths):
    records = []
    for path in paths:
        files = (sorted(glob.glob(os.path.join(path, "**", "precision_probe.*.jsonl"),
                                  recursive=True))
                 if os.path.isdir(path) else [path])
        if not files:
            print(f"warning: no precision_probe.*.jsonl under {path}", file=sys.stderr)
        for name in files:
            with open(name, "r", encoding="utf-8") as handle:
                for lineno, line in enumerate(handle, 1):
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        records.append(json.loads(line))
                    except json.JSONDecodeError as err:
                        print(f"warning: {name}:{lineno}: {err}", file=sys.stderr)
    return records


def merge_hist(into, other):
    for key, value in (other or {}).items():
        into[key] = into.get(key, 0) + value


def pick(a, b, fn):
    if a is None:
        return b
    if b is None:
        return a
    return fn(a, b)


def merge(records):
    """Combine records of the same site (several processes or runs)."""
    sites = {}
    for rec in records:
        key = (rec.get("site"), rec.get("kind"), rec.get("type"))
        cur = sites.get(key)
        if cur is None:
            cur = dict(rec)
            for name in ("binades", "lost_bits", "gap_bits", "log2_amplification"):
                if name in cur:
                    cur[name] = dict(cur[name])
            cur["_amp_weight"] = rec.get("count", 0)
            sites[key] = cur
            continue
        for name, value in rec.items():
            if name in ("site", "kind", "type", "file", "line", "eps_lo"):
                continue
            if isinstance(value, dict):
                merge_hist(cur.setdefault(name, {}), value)
            elif name in ("min", "min_abs_nonzero"):
                cur[name] = pick(cur.get(name), value, min)
            elif name in ("max", "max_abs", "max_lost_bits", "max_gap_bits"):
                cur[name] = pick(cur.get(name), value, max)
            elif name == "max_amplification":
                if value is not None and value > (cur.get(name) or 0):
                    cur[name] = value
                    cur["worst_ref"] = rec.get("worst_ref")
                    cur["worst_lo"] = rec.get("worst_lo")
            elif name == "mean_amplification":
                w_old, w_new = cur["_amp_weight"], rec.get("count", 0)
                if w_old + w_new > 0:
                    cur[name] = ((cur.get(name) or 0) * w_old + (value or 0) * w_new) / (w_old + w_new)
                cur["_amp_weight"] = w_old + w_new
            elif name in ("worst_a", "worst_b", "worst_ref", "worst_lo"):
                continue  # kept with the maximum they belong to
            elif isinstance(value, (int, float)) and not isinstance(value, bool):
                cur[name] = (cur.get(name) or 0) + value
    for cur in sites.values():
        cur.pop("_amp_weight", None)
    return list(sites.values())


def hist_at_least(hist, threshold):
    return sum(v for k, v in (hist or {}).items() if int(k) >= threshold)


def hist_below(hist, threshold):
    return sum(v for k, v in (hist or {}).items() if int(k) < threshold)


def judge_range(site, bits, fmax, fmin):
    flags, notes = [], []
    max_abs, min_abs = site.get("max_abs"), site.get("min_abs_nonzero")
    hist = site.get("binades", {})
    if site.get("n_nonfinite"):
        flags.append(("blocker", "NONFINITE"))
        notes.append(f"{site['n_nonfinite']} non-finite values in the reference run")
    if max_abs is not None and max_abs > fmax:
        n = hist_at_least(hist, math.frexp(fmax)[1])
        flags.append(("blocker", "OVERFLOW"))
        notes.append(f"max |x| = {max_abs:.3g} exceeds the target maximum {fmax:.3g} ({n} values)")
    if min_abs is not None and min_abs < fmin:
        n = hist_below(hist, math.frexp(fmin)[1] - 1)
        flags.append(("blocker", "UNDERFLOW"))
        notes.append(f"min |x| > 0 is {min_abs:.3g}, below the target's smallest normal "
                     f"{fmin:.3g} ({n} values lose precision or flush to zero)")
    if max_abs is not None and max_abs <= fmax and max_abs * max_abs > fmax:
        flags.append(("warn", "SQUARE_OVERFLOWS"))
        notes.append(f"x*x overflows the target for |x| > {math.sqrt(fmax):.3g}: "
                     "check products and norms that use this value")
    if min_abs is not None and min_abs >= fmin and min_abs * min_abs < fmin:
        flags.append(("warn", "SQUARE_UNDERFLOWS"))
        notes.append(f"x*x underflows the target for |x| < {math.sqrt(fmin):.3g}: "
                     "check products of this value")
    if max_abs and min_abs:
        span = math.log2(max_abs / min_abs)
        site["dynamic_range_bits"] = round(span, 1)
        if span > bits:
            flags.append(("info", "WIDE_RANGE"))
            notes.append(f"values span {span:.0f} binades, more than the {bits}-bit significand: "
                         "sums over this data lose the small terms")
    count = site.get("count", 0)
    integral = count > 0 and site.get("n_nonintegral", 0) == 0 and max_abs is not None
    if integral and max_abs > 2 ** bits:
        # Informational: physical quantities are often integer-valued by
        # accident. It matters only for counters, indices and sums of ones.
        flags.append(("info", "INTEGER_VALUED_LARGE"))
        notes.append(f"all values are integers, up to {max_abs:.3g}, beyond 2^{bits} = {2 ** bits}: "
                     "if this is a counter, an index or a count held in floating point, "
                     "it is no longer exact in the target")
    return flags, notes


def judge_addsub(site, bits):
    flags, notes = [], []
    source_bits = SOURCE_BITS.get(site.get("type"), 53)
    if source_bits <= bits:
        return [("info", "TARGET_TYPE_SITE")], ["recorded at the target precision already (informational)"]
    count = site.get("count", 0) or 1
    lost = site.get("max_lost_bits", 0) or 0
    gap = site.get("max_gap_bits", 0) or 0
    half = bits // 2
    n_half = hist_at_least(site.get("lost_bits"), half)
    n_all = hist_at_least(site.get("lost_bits"), bits) + (site.get("n_total_cancel") or 0)
    if lost >= bits or n_all:
        flags.append(("blocker", "CANCELLATION"))
        notes.append(f"up to {lost} leading bits cancel (worst operands {site.get('worst_a')!r}, "
                     f"{site.get('worst_b')!r}); in the target only {max(0, bits - lost)} of {bits} "
                     f"bits can survive, and {n_all} of {count} operations would keep none")
    elif lost >= half:
        flags.append(("warn", "CANCELLATION"))
        notes.append(f"up to {lost} leading bits cancel, leaving {bits - lost} of {bits} bits in the "
                     f"target; {n_half} of {count} operations lose at least half the significand")
    n_absorbed = hist_at_least(site.get("gap_bits"), bits)
    n_most = hist_at_least(site.get("gap_bits"), bits - 4)
    n_half = hist_at_least(site.get("gap_bits"), bits // 2)
    site["smaller_operand_bits_kept"] = max(0, bits - gap)
    if n_absorbed:
        flags.append(("blocker", "ABSORPTION"))
        notes.append(f"operand gap up to {gap} bits: in {n_absorbed} of {count} operations the "
                     f"smaller term is below one ulp of the larger in the target and has no effect")
    elif n_most:
        flags.append(("warn", "ABSORPTION"))
        notes.append(f"operand gap up to {gap} bits: in {n_most} of {count} operations the smaller "
                     f"term keeps fewer than 4 of its bits in the target")
    elif n_half:
        flags.append(("warn", "PARTIAL_ABSORPTION"))
        notes.append(f"operand gap up to {gap} bits: the smaller term keeps only {bits - gap} of "
                     f"{bits} bits in the target ({n_half} of {count} operations lose at least half). "
                     "Harmless for a single operation; in an accumulation (x += small, every step) "
                     "the rounding can repeat with the same sign and grow with the number of steps: "
                     "measure the error growth over the run")
    return flags, notes


def judge_demote(site, amp_max):
    flags, notes = [], []
    worst = site.get("max_amplification") or 0.0
    mean = site.get("mean_amplification") or 0.0
    count = site.get("count", 0) or 1
    threshold_bin = max(1, math.ceil(math.log2(amp_max)) + 1)
    n_over = hist_at_least(site.get("log2_amplification"), threshold_bin)
    if site.get("n_lo_nonfinite"):
        flags.append(("blocker", "LO_NONFINITE"))
        notes.append(f"{site['n_lo_nonfinite']} values are finite in the reference but overflow "
                     "or become NaN at the lower precision")
    if site.get("n_ref_zero_mismatch"):
        flags.append(("warn", "ZERO_MISMATCH"))
        notes.append(f"{site['n_ref_zero_mismatch']} values are exactly zero in the reference "
                     "but not at the lower precision")
    if worst > amp_max:
        severity = "blocker" if worst > amp_max ** 2 else "warn"
        flags.append((severity, "AMPLIFIED"))
        notes.append(f"error is {worst:.3g} eps at worst (mean {mean:.3g} eps), about "
                     f"{math.log2(worst):.0f} bits lost; {n_over} of {count} values exceed "
                     f"{amp_max:g} eps; worst reference value {site.get('worst_ref')!r} became "
                     f"{site.get('worst_lo')!r}")
    return flags, notes


def judge_count(site, lo, hi, name):
    flags, notes = [], []
    vmin, vmax = site.get("min"), site.get("max")
    if site.get("n_above_int64"):
        flags.append(("blocker", "INT_OVERFLOW"))
        notes.append(f"{site['n_above_int64']} values exceed the signed 64-bit range")
    if vmax is not None and vmax > hi:
        flags.append(("blocker", "INT_OVERFLOW"))
        notes.append(f"max {vmax} does not fit {name} (limit {hi})")
    if vmin is not None and vmin < lo:
        flags.append(("blocker", "INT_OVERFLOW"))
        notes.append(f"min {vmin} does not fit {name} (limit {lo})")
    if vmax is not None and 0 < vmax <= hi:
        headroom = math.log2(hi / vmax)
        site["headroom_bits"] = round(headroom, 1)
        if headroom < 1.0 and not flags:
            flags.append(("warn", "INT_LITTLE_HEADROOM"))
            notes.append(f"max {vmax} is within a factor 2 of the {name} limit: this workload "
                         "fits, a larger one will not")
    return flags, notes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="+")
    parser.add_argument("--target", choices=sorted(FLOAT_TARGETS), default="fp32")
    parser.add_argument("--int-target", choices=sorted(INT_TARGETS), default="int32")
    parser.add_argument("--amp-max", type=float, default=16.0,
                        help="demotion error, in eps of the lower precision, that counts "
                             "as amplified (default 16)")
    parser.add_argument("--json", dest="json_path")
    parser.add_argument("--fail-on-blocker", action="store_true",
                        help="exit with status 1 if any site is a blocker")
    args = parser.parse_args()

    bits, fmax, fmin = FLOAT_TARGETS[args.target]
    ilo, ihi = INT_TARGETS[args.int_target]
    sites = merge(load(args.paths))
    if not sites:
        print("error: no probe records found", file=sys.stderr)
        return 2

    order = {"blocker": 0, "warn": 1, "info": 2, "ok": 3}
    for site in sites:
        kind = site.get("kind")
        if kind == "range":
            flags, notes = judge_range(site, bits, fmax, fmin)
        elif kind in ("addsub", "cancel"):
            flags, notes = judge_addsub(site, bits)
        elif kind == "demote":
            flags, notes = judge_demote(site, args.amp_max)
        elif kind == "count":
            flags, notes = judge_count(site, ilo, ihi, args.int_target)
        else:
            flags, notes = [("info", "UNKNOWN_KIND")], [f"unknown site kind {kind!r}"]
        site["flags"] = [name for _, name in flags]
        site["notes"] = notes
        site["status"] = min((sev for sev, _ in flags), key=order.get, default="ok")
    sites.sort(key=lambda s: (order[s["status"]], s.get("site") or "", s.get("kind") or ""))

    n_block = sum(1 for s in sites if s["status"] == "blocker")
    n_warn = sum(1 for s in sites if s["status"] == "warn")
    summary = {"target": args.target, "int_target": args.int_target,
               "amp_max": args.amp_max, "n_sites": len(sites),
               "n_blocker": n_block, "n_warn": n_warn, "sites": sites}
    if args.json_path:
        with open(args.json_path, "w", encoding="utf-8") as handle:
            json.dump(summary, handle, indent=1)
            handle.write("\n")

    print(f"# Probe summary against {args.target} ({bits}-bit significand, "
          f"range {fmin:.3g} .. {fmax:.3g}) and {args.int_target}")
    print(f"{len(sites)} sites: {n_block} blocker, {n_warn} warn\n")
    print("| status | site | kind | type | count | flags | range / worst case |")
    print("|---|---|---|---|---:|---|---|")
    for site in sites:
        kind = site.get("kind")
        if kind == "range":
            detail = f"|x| in [{site.get('min_abs_nonzero')}, {site.get('max_abs')}]"
        elif kind in ("addsub", "cancel"):
            detail = (f"lost bits <= {site.get('max_lost_bits')}, "
                      f"gap bits <= {site.get('max_gap_bits', 'n/a')}")
        elif kind == "demote":
            detail = f"max {site.get('max_amplification') or 0:.3g} eps"
        else:
            detail = f"[{site.get('min')}, {site.get('max')}]"
        print(f"| {site['status']} | {site.get('site')} | {kind} | {site.get('type')} | "
              f"{site.get('count', 0)} | {' '.join(site['flags']) or '-'} | {detail} |")
    flagged = [s for s in sites if s["notes"] and s["status"] in ("blocker", "warn")]
    if flagged:
        print("\n## Details\n")
        for site in flagged:
            where = f"{site.get('file')}:{site.get('line')}" if site.get("file") else ""
            print(f"- **{site.get('site')}** ({site.get('kind')}, {site.get('type')}) {where}")
            for note in site["notes"]:
                print(f"  - {note}")
    print("\nA flag describes this workload only, and one operation at a time. Values the "
          "run never produced were not judged, and slow growth over many steps does not "
          "show here: add margin, and check error growth for every accumulation.")
    return 1 if (n_block and args.fail_on_blocker) else 0


if __name__ == "__main__":
    sys.exit(main())
