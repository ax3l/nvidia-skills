#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Static audit for 64-bit floating-point instructions left in a binary.

After a port to lower precision, every remaining FP64 instruction in the
reduced-precision build is either an approved "precision island" or a leak:
an unsuffixed literal, a `double` local, a math call that resolved to the
double overload, a conversion at a boundary. This script lists them per
function so that each one can be explained.

Two back ends:
  cuda   disassembles the device code of a CUDA binary or object with
         `cuobjdump -sass` and classifies the SASS opcodes of every kernel
  host   disassembles host code with `objdump -d` and classifies x86-64
         SSE/AVX instructions (AArch64 patterns are included but have not
         been verified on real output)

Usage:
    fp64-audit.py BINARY [--mode auto|cuda|host] [--functions REGEX]
                  [--allow REGEX ...] [--loops] [--top 30]
                  [--json audit.json] [--fail-on-fp64]

--allow marks functions whose FP64 use is an approved decision; they are
listed but do not count as leaks. With --fail-on-fp64 the exit status is 1
when any other function contains FP64 arithmetic, compares or conversions.

--loops answers "is there FP64 inside a loop?", which is the question on
hardware where FP64 is rationed and a conversion once per call, after the
loop, is acceptable. A loop is the address range closed by a backward branch
inside a function. Reported per function: the FP64 instructions inside such
ranges, and the functions called from inside them that contain FP64
themselves or are double-precision math library functions (calls are followed
one level, by name; indirect calls are not followed). With --loops,
--fail-on-fp64 judges only this: FP64 outside loops no longer fails.
The loop ranges are conservative: when the compiler places a rarely taken
block of a loop behind the code that follows the loop, that code is counted
as inside. For this audit, build kernels as separate, non-inlined functions,
or host code inlined around them is mixed into their counts.

Limits: the counts are STATIC (an instruction in a loop counts once; an
untaken branch counts too). Executed counts need Nsight Compute. Moves and
loads of 64-bit data are reported separately and never counted as leaks.
For the source line behind a device instruction, build with `-lineinfo`
and disassemble the extracted cubin with `nvdisasm -gi`, or use
`nvcc -Xptxas --warn-on-double-precision-use` at compile time.
Only the Python standard library is required.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from collections import Counter

FAMILIES = ["fp64_arith", "fp64_compare", "fp64_convert", "fp64_other",
            "fp64_move", "fp32_arith", "fp16"]
LEAK_FAMILIES = ["fp64_arith", "fp64_compare", "fp64_convert", "fp64_other"]

# ---------------------------------------------------------------- CUDA SASS
SASS_INSTRUCTION = re.compile(
    r"/\*([0-9a-f]+)\*/\s+(?:@!?U?P[T0-9]+\s+)?([A-Z][A-Z0-9_]*(?:\.[A-Z0-9_]+)*)\s*([^;/]*)")
SASS_BRANCH = {"BRA", "JMP", "BRX", "JMX"}
SASS_FUNCTION = re.compile(r"^\s*Function : (\S+)")
SASS_F64_ARITH = {"DADD", "DMUL", "DFMA", "DMMA"}
SASS_F64_COMPARE = {"DSETP", "DSET", "DMNMX"}
SASS_F32_ARITH = {"FADD", "FMUL", "FFMA", "FADD32I", "FMUL32I", "FFMA32I",
                  "FADD2", "FMUL2", "FFMA2"}
SASS_F16 = {"HADD2", "HMUL2", "HFMA2", "HADD2_32I", "HMUL2_32I", "HFMA2_32I"}
SASS_CONVERT = {"F2F", "F2I", "I2F", "F2FP", "I2FP", "F2IP"}


def classify_sass(opcode: str):
    base, *mods = opcode.split(".")
    if base in SASS_F64_ARITH:
        return "fp64_arith"
    if base in SASS_F64_COMPARE:
        return "fp64_compare"
    if base in SASS_CONVERT:
        return "fp64_convert" if "F64" in mods else None
    if base == "FRND":
        return "fp64_other" if "F64" in mods else None
    if base == "MUFU":  # RCP64H / RSQ64H seed FP64 division and square root
        return "fp64_other" if any(m.endswith("64H") for m in mods) else None
    if base in SASS_F32_ARITH:
        return "fp32_arith"
    if base in SASS_F16:
        return None if "MMA" in mods else "fp16"  # MMA form materializes constants
    return None


def audit_cuda(binary: str):
    if shutil.which("cuobjdump") is None:
        raise RuntimeError("cuobjdump not found (it ships with the CUDA toolkit)")
    proc = subprocess.run(["cuobjdump", "-sass", binary], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"cuobjdump failed: {proc.stderr.strip()[:300]}")
    functions, current = {}, None
    for line in proc.stdout.splitlines():
        match = SASS_FUNCTION.match(line)
        if match:
            name = match.group(1)
            # inline templates appear once per translation unit: keep the first copy
            current = None if name in functions else functions.setdefault(name, new_function())
            continue
        if current is None:
            continue
        match = SASS_INSTRUCTION.search(line)
        if match:
            address, opcode, operands = int(match.group(1), 16), match.group(2), match.group(3)
            family = classify_sass(opcode)
            target = callee = None
            base = opcode.split(".")[0]
            hexes = re.findall(r"0x([0-9a-f]+)", operands)
            if base in SASS_BRANCH and hexes:
                target = int(hexes[-1], 16)
            elif base == "CALL":
                callee = operands.strip() or "(indirect)"
            record(current, address, family, target, callee)
    return functions


# ------------------------------------------------------------- host objdump
HOST_SYMBOL = re.compile(r"^[0-9a-f]+ <(.+)>:$")
HOST_INSTRUCTION = re.compile(r"^\s*([0-9a-f]+):\s+([a-z][a-z0-9.]*)\s*(.*)$")
HOST_PREFIXES = {"bnd", "notrack", "rep", "repz", "repnz", "lock", "data16", "addr32"}
# the symbol may itself contain <...> (templates): match to the last >
HOST_TARGET = re.compile(r"^\*?([0-9a-f]+) <(.+)>\s*$")
A64_BRANCH = re.compile(r"^(?:b|b\.\w+|cbn?z|tbn?z)$")
# double-precision entry points of the C math library (the float ones end in f)
LIBM_DOUBLE = {
    "sqrt", "cbrt", "exp", "exp2", "exp10", "expm1", "log", "log2", "log10", "log1p", "pow",
    "sin", "cos", "tan", "sincos", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh",
    "asinh", "acosh", "atanh", "floor", "ceil", "round", "trunc", "rint", "nearbyint",
    "fmod", "remainder", "hypot", "fabs", "fma", "erf", "erfc", "tgamma", "lgamma", "ldexp",
    "frexp", "modf", "copysign", "fmin", "fmax", "fdim", "lround", "llround", "lrint",
    "llrint", "nextafter", "scalbn", "logb", "ilogb"}
X86_ARITH = (r"v?(?:add|sub|mul|div|sqrt|max|min|round|rndscale|scalef|getexp|getmant|"
             r"reduce|range|fmadd\d+|fmsub\d+|fnmadd\d+|fnmsub\d+|fmaddsub\d+|fmsubadd\d+|"
             r"hadd|hsub|addsub|dp|rcp14|rsqrt14)")
X86_F64_ARITH = re.compile(r"^" + X86_ARITH + r"[sp]d$")
X86_F32_ARITH = re.compile(r"^" + X86_ARITH + r"[sp]s$")
X86_F64_COMPARE = re.compile(r"^v?(?:u?comisd|cmp\w*[sp]d)$")
X86_F64_CONVERT = re.compile(
    r"^v?cvt(?:t?[sp]d2\w+|[sp]s2[sp]d|si2sd|usi2sd|dq2pd|udq2pd|qq2pd|uqq2pd|ph2pd|sh2sd)[lq]?$")
X86_F64_OTHER = re.compile(r"^v?(?:and|andn|or|xor|blendv?|shuf|unpck[hl]|perm\w*|movmsk)pd$")
X86_F64_MOVE = re.compile(r"^v?mov(?:sd|apd|upd|hpd|lpd|ddup|ntpd)$")
X86_F16 = re.compile(r"^v\w+[sp]h$")
A64_FP = re.compile(r"^(?:f(?:add|sub|mul|div|n?madd|n?msub|sqrt|max\w*|min\w*|abs|neg|"
                    r"rint\w*|cmp|cmpe|ccmp\w*|csel|cvt\w*)|[su]cvtf)$")
A64_DOUBLE_OPERAND = re.compile(r"\bd\d+\b|\.2d\b")
A64_SINGLE_OPERAND = re.compile(r"\bs\d+\b|\.4s\b")


def classify_host(mnemonic: str, operands: str):
    if X86_F64_CONVERT.match(mnemonic):
        return "fp64_convert"
    if X86_F64_COMPARE.match(mnemonic):
        return "fp64_compare"
    if X86_F64_ARITH.match(mnemonic):
        return "fp64_arith"
    if X86_F64_OTHER.match(mnemonic):
        return "fp64_other"
    if X86_F64_MOVE.match(mnemonic):
        # movsd without operands is the string instruction, not an FP move
        return "fp64_move" if "xmm" in operands or "ymm" in operands or "zmm" in operands else None
    if X86_F32_ARITH.match(mnemonic):
        return "fp32_arith"
    if X86_F16.match(mnemonic):
        return "fp16"
    if A64_FP.match(mnemonic):
        has_d = bool(A64_DOUBLE_OPERAND.search(operands))
        if mnemonic.startswith("fcvt") or mnemonic.endswith("cvtf"):
            return "fp64_convert" if has_d else None
        if has_d:
            return "fp64_compare" if mnemonic.startswith(("fcmp", "fccmp")) else "fp64_arith"
        if A64_SINGLE_OPERAND.search(operands):
            return "fp32_arith"
    return None


def audit_host(binary: str):
    if shutil.which("objdump") is None:
        raise RuntimeError("objdump not found (binutils)")
    proc = subprocess.run(["objdump", "-d", "-C", "--no-show-raw-insn", binary],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"objdump failed: {proc.stderr.strip()[:300]}")
    functions, current = {}, None
    for line in proc.stdout.splitlines():
        match = HOST_SYMBOL.match(line)
        if match:
            current = functions.setdefault(match.group(1), new_function())
            continue
        if current is None:
            continue
        match = HOST_INSTRUCTION.match(line)
        if match:
            address, mnemonic, operands = int(match.group(1), 16), match.group(2), match.group(3)
            if mnemonic in HOST_PREFIXES and operands:
                mnemonic, _, operands = operands.partition(" ")
                operands = operands.strip()
            family = classify_host(mnemonic, operands)
            target = callee = None
            where = HOST_TARGET.search(operands)
            if mnemonic.startswith("call") or mnemonic in ("bl", "blr"):
                callee = where.group(2) if where else "(indirect)"
            elif (mnemonic.startswith(("j", "loop")) or A64_BRANCH.match(mnemonic)) and where:
                target = int(where.group(1), 16)
            record(current, address, family, target, callee)
    return functions


def new_function():
    """Per-function record: family counts plus what the loop analysis needs."""
    counts = Counter()
    counts.instructions = []   # (address, family or None, callee or None)
    counts.branches = []       # (address, target address)
    return counts


def record(counts, address, family, target, callee):
    counts["n_inst"] += 1
    if family:
        counts[family] += 1
    counts.instructions.append((address, family, callee))
    if target is not None:
        counts.branches.append((address, target))


def loop_ranges(counts):
    """Address ranges [target, branch] of the backward branches inside one
    function, merged where they overlap."""
    if not counts.instructions:
        return []
    first = counts.instructions[0][0]
    spans = sorted((target, source) for source, target in counts.branches
                   if first <= target < source)
    merged = []
    for lo, hi in spans:
        if merged and lo <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    return merged


def callee_key(name):
    """`floor@plt` -> floor, `helper(float)+0x10` -> helper(float)."""
    name = re.sub(r"\+0x[0-9a-f]+$", "", name.strip())
    return re.sub(r"@(?:plt|@?GLIBC\w*[\d.]*)$", "", name)


def analyze_loops(functions):
    """Annotate every function with its FP64 use inside loops."""
    totals = {name: sum(counts[f] for f in LEAK_FAMILIES) for name, counts in functions.items()}
    result = {}
    for name, counts in functions.items():
        ranges = loop_ranges(counts)
        inside = Counter()
        callees = set()
        for address, family, callee in counts.instructions:
            if not any(lo <= address <= hi for lo, hi in ranges):
                continue
            if family in LEAK_FAMILIES:
                inside[family] += 1
            if callee:
                callees.add(callee_key(callee))
        flagged = []
        for callee in sorted(callees):
            if callee in LIBM_DOUBLE:
                flagged.append(f"{callee} (double math library)")
            elif callee != name and totals.get(callee, 0) > 0:
                flagged.append(f"{callee} ({totals[callee]} FP64)")
        result[name] = {"n_loops": len(ranges),
                        "fp64_in_loops": sum(inside.values()),
                        "fp64_in_loops_by_family": dict(inside),
                        "calls_in_loops": sorted(callees),
                        "fp64_callees_in_loops": flagged}
    return result


def demangle(names):
    if shutil.which("c++filt") is None or not names:
        return {n: n for n in names}
    proc = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True)
    out = proc.stdout.splitlines()
    return dict(zip(names, out)) if len(out) == len(names) else {n: n for n in names}


def has_device_code(binary: str) -> bool:
    if shutil.which("cuobjdump") is None:
        return False
    proc = subprocess.run(["cuobjdump", "-lelf", binary], capture_output=True, text=True)
    return proc.returncode == 0 and "ELF file" in proc.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("binary")
    parser.add_argument("--mode", choices=["auto", "cuda", "host"], default="auto")
    parser.add_argument("--functions", help="only functions whose (demangled) name matches")
    parser.add_argument("--allow", action="append", default=[],
                        help="regex of functions with approved FP64 use (repeatable)")
    parser.add_argument("--loops", action="store_true",
                        help="report and judge FP64 inside loops (backward-branch ranges)")
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--json", dest="json_path")
    parser.add_argument("--fail-on-fp64", action="store_true")
    args = parser.parse_args()

    mode = args.mode
    if mode == "auto":
        mode = "cuda" if has_device_code(args.binary) else "host"
    try:
        functions = audit_cuda(args.binary) if mode == "cuda" else audit_host(args.binary)
    except RuntimeError as err:
        print(f"error: {err}", file=sys.stderr)
        return 2
    if not functions:
        print(f"error: no {mode} code found in {args.binary}", file=sys.stderr)
        return 2

    pretty = demangle(list(functions)) if mode == "cuda" else {n: n for n in functions}
    select = re.compile(args.functions) if args.functions else None
    allowed = [re.compile(rx) for rx in args.allow]
    loops = analyze_loops(functions) if args.loops else {}
    rows = []
    for name, counts in functions.items():
        shown = pretty[name]
        if select and not select.search(shown):
            continue
        leak = sum(counts[f] for f in LEAK_FAMILIES)
        row = {"function": shown, "n_inst": counts["n_inst"],
               **{f: counts[f] for f in FAMILIES}, "fp64_total": leak,
               "approved": any(rx.search(shown) for rx in allowed)}
        if args.loops:
            row.update(loops[name])
            row["fp64_callees_in_loops"] = [pretty.get(c.split(" (")[0], c.split(" (")[0])
                                            + " (" + c.split(" (", 1)[1]
                                            for c in row["fp64_callees_in_loops"]]
        rows.append(row)
    if args.loops:
        hot = lambda r: r["fp64_in_loops"] > 0 or bool(r["fp64_callees_in_loops"])
        rows.sort(key=lambda r: (-r["fp64_in_loops"], -len(r["fp64_callees_in_loops"]),
                                 -r["fp64_total"], r["function"]))
    else:
        hot = lambda r: r["fp64_total"] > 0
        rows.sort(key=lambda r: (-r["fp64_total"], r["function"]))
    with_fp64 = [r for r in rows if r["fp64_total"] > 0 or hot(r)]
    leaks = [r for r in with_fp64 if hot(r) and not r["approved"]]

    result = {"binary": args.binary, "mode": mode, "loops": args.loops,
              "n_functions": len(rows), "n_with_fp64": len(with_fp64),
              "n_unapproved": len(leaks), "functions": rows}
    if args.json_path:
        with open(args.json_path, "w", encoding="utf-8") as handle:
            json.dump(result, handle, indent=1)
            handle.write("\n")

    print(f"# FP64 audit ({mode} code): {args.binary}")
    if args.loops:
        print(f"{len(rows)} functions, {len(with_fp64)} contain FP64 instructions; "
              f"{len(leaks)} have FP64 inside a loop and are not on the approved list\n")
        if with_fp64:
            print("| function | loops | fp64 in loops | fp64 elsewhere | "
                  "called from loops, with FP64 | approved |")
            print("|---|---:|---:|---:|---|---|")
            for row in with_fp64[:args.top]:
                name = row["function"] if len(row["function"]) <= 90 else row["function"][:87] + "..."
                status = "yes" if row["approved"] else ("NO" if hot(row) else "-")
                print(f"| `{name}` | {row['n_loops']} | {row['fp64_in_loops']} | "
                      f"{row['fp64_total'] - row['fp64_in_loops']} | "
                      f"{', '.join(row['fp64_callees_in_loops']) or '-'} | {status} |")
            if len(with_fp64) > args.top:
                print(f"\n... {len(with_fp64) - args.top} more; see --json or raise --top")
        print("\nA loop is the address range closed by a backward branch; the ranges are "
              "conservative (see --help). Calls are followed one level, by name. Static "
              "counts: conversions between FP32 and FP64 execute on the FP64 units and "
              "count as FP64.")
        return 1 if (args.fail_on_fp64 and leaks) else 0
    print(f"{len(rows)} functions, {len(with_fp64)} contain FP64 instructions, "
          f"{len(leaks)} of those are not on the approved list\n")
    if with_fp64:
        print("| function | instr. | fp64 arith | fp64 cmp | fp64 convert | fp64 other | "
              "fp32 arith | approved |")
        print("|---|---:|---:|---:|---:|---:|---:|---|")
        for row in with_fp64[:args.top]:
            name = row["function"] if len(row["function"]) <= 90 else row["function"][:87] + "..."
            print(f"| `{name}` | {row['n_inst']} | {row['fp64_arith']} | {row['fp64_compare']} | "
                  f"{row['fp64_convert']} | {row['fp64_other']} | {row['fp32_arith']} | "
                  f"{'yes' if row['approved'] else 'NO'} |")
        if len(with_fp64) > args.top:
            print(f"\n... {len(with_fp64) - args.top} more; see --json or raise --top")
    print("\nStatic instruction counts: loops count once. Conversions between FP32 and "
          "FP64 execute on the FP64 units and are counted as FP64.")
    return 1 if (args.fail_on_fp64 and leaks) else 0


if __name__ == "__main__":
    sys.exit(main())
