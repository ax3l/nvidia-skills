#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Static inventory of the precision surface of a C/C++ or Fortran code base.

Counts, per file, the constructs that tie a code to one numeric precision or
that tend to break when the precision is lowered: hard-wired types and
literals, precision-specific math calls, tolerances, I/O and library
boundaries, bit-level tricks, reductions, time accumulation, and
value-changing compiler flags.

This is a regex scan, not a parser. It over-reports on purpose and is meant
to (1) size the mechanical work, (2) rank files for a closer look, and
(3) seed the list of sites to probe at run time. Compiler diagnostics
(-Wdouble-promotion, -Wconversion, ...) and clang-tidy give exact locations
for the constructs they cover; use them on the files this scan ranks first.

Usage:
    precision-scan.py SOURCE_DIR [--json inventory.json] [--top 20]
                      [--exclude GLOB ...] [--findings-per-file 5]

Only the Python standard library is required. Nothing is modified.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import sys
from collections import Counter, defaultdict
from fractions import Fraction

CPP_EXT = {".c", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hh", ".hpp", ".hxx",
           ".inl", ".tpp", ".ipp", ".cu", ".cuh"}
CPP_EXT_CASED = {".C", ".H"}  # checked case-sensitively
FORTRAN_FREE_EXT = {".f90", ".f95", ".f03", ".f08", ".f18"}
FORTRAN_FIXED_EXT = {".f", ".for", ".ftn", ".f77"}
BUILD_NAMES = {"cmakelists.txt", "makefile", "gnumakefile", "meson.build",
               "configure.ac", "setup.py", "make.inc"}
BUILD_EXT = {".cmake", ".mk", ".mak", ".make"}

DEFAULT_EXCLUDES = [
    ".git", ".hg", ".svn", "build*", "_build*", "_deps", "cmake-build-*",
    "node_modules", ".precision-port", ".nsight-ai-port", "third_party",
    "thirdparty", "third-party", "extern", "external", "vendor", "vendored",
    "submodules", "__pycache__", ".venv", "venv",
]

FP32_MAX = 3.4028234663852886e38
FP32_MIN_NORMAL = 1.1754943508222875e-38
FP32_EPS = 1.1920928955078125e-07

# category -> (weight for ranking files, one-line meaning)
CATEGORIES = {
    "literal_out_of_fp32_range": (10.0, "constant that overflows or underflows in FP32"),
    "time_accumulation": (8.0, "t += dt style accumulation (stalls at low precision)"),
    "bit_tricks": (6.0, "type punning / raw bytes (layout depends on the type)"),
    "tolerance_candidate": (5.0, "tolerance or epsilon tuned for the current precision"),
    "epsilon_query": (2.0, "epsilon/tiny/huge/limits query (check which type it names)"),
    "interop_mpi": (4.0, "MPI datatype tied to a precision"),
    "interop_io": (4.0, "I/O layout or type tied to a precision"),
    "interop_library": (4.0, "precision-specific library entry point (BLAS/LAPACK/FFT/...)"),
    "interop_language": (2.0, "C/Fortran interoperability boundary"),
    "math_precision_specific": (3.0, "math call hard-wired to one precision"),
    "math_unqualified": (1.0, "unqualified C math call (double in C; check overloads in C++)"),
    "float_equality": (3.0, "floating-point compared with == or /= against a non-zero literal"),
    "float_equality_zero": (0.5, "floating-point compared with == or /= against zero"),
    "float_loop_counter": (3.0, "loop driven by a floating-point counter"),
    "reduction": (2.0, "sum / dot / reduction (error grows with length)"),
    "accumulation_candidate": (2.0, "scalar updated with += / -= (a running sum or an accumulated state)"),
    "float_int_conversion": (1.0, "float <-> integer conversion or rounding to an index"),
    "literal_fp64": (0.3, "literal hard-wired to double precision"),
    "literal_converted": (0.0, "literal passed through an explicit conversion, e.g. T(0.5)"),
    "literal_long_double_route": (0.0, "double literal that changes in its last bit if routed "
                                       "through long double (a `1.1_r` user-defined literal). "
                                       "Matters only if the double build must stay bit-identical: "
                                       "then write it as Real(1.1)"),
    "literal_default": (0.3, "literal of the language's default real kind"),
    "type_fp64": (0.1, "variable or argument declared in 64-bit floating point"),
    "type_fp32": (0.1, "variable or argument declared in 32-bit floating point"),
    "type_fp_extended": (1.0, "extended / quad precision type"),
    "type_int64": (0.1, "64-bit integer"),
    "type_size": (0.05, "size_t / ptrdiff_t style size type"),
    "implicit_typing": (3.0, "implicit typing statement that fixes a real kind"),
    "precision_alias": (0.0, "existing precision alias / kind parameter / switch"),
    "half_precision": (0.0, "half-precision type already in use"),
}

FP_LITERAL = r"(?:\d+\.\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?|\d+[eE][+-]?\d+)"

# Names that suggest a tolerance. "eps0" / "epsilon_0" style names are the
# vacuum permittivity in physics codes, not a tolerance, and are left out.
TOLERANCE_HINT = re.compile(
    r"(?i)\b(tol\w*|eps|epsilon|eps_(?!0\b|r\b)\w+|\w+_eps|small\w*|tiny\w*|"
    r"thresh\w*|converge\w*|accuracy|rtol|atol|cutoff)\b")

C_MATH = (r"sqrt|cbrt|fabs|pow|exp|exp2|expm1|log|log2|log10|log1p|sin|cos|tan|"
          r"asin|acos|atan|atan2|sinh|cosh|tanh|floor|ceil|round|trunc|fmod|"
          r"hypot|erf|erfc|tgamma|lgamma|fmin|fmax|copysign|fma")


def _compile(patterns):
    return [(cat, re.compile(rx)) for cat, rx in patterns]


CPP_PATTERNS = _compile([
    ("type_fp_extended", r"\blong\s+double\b|\b__float128\b|\b_Float128\b"),
    ("type_fp64", r"(?<!long\s)\bdouble\b"),
    ("type_fp32", r"\bfloat\b"),
    ("type_int64", r"\bu?int64_t\b|\blong\s+long\b|\b(?:unsigned\s+)?long\b(?!\s+(?:long|double))"),
    ("type_size", r"\b(?:std::)?(?:size_t|ptrdiff_t|ssize_t)\b"),
    ("half_precision", r"\b__half2?\b|\bfloat16_t\b|\b_Float16\b|\b__fp16\b|\bbfloat16\w*\b"),
    ("math_precision_specific", r"(?<![\w:.])(?:" + C_MATH + r")[fl]\s*\("),
    ("math_unqualified", r"(?<![\w:.>])(?:" + C_MATH + r"|abs)\s*\("),
    ("epsilon_query", r"\b(?:DBL|FLT|LDBL)_(?:EPSILON|MIN|MAX|DIG|MANT_DIG)\b|"
                      r"numeric_limits\s*<\s*(?:double|float|long\s+double)\s*>"),
    ("interop_mpi", r"\bMPI_(?:DOUBLE|FLOAT|LONG_DOUBLE|LONG_LONG(?:_INT)?|LONG|"
                    r"INT64_T|UINT64_T|UNSIGNED_LONG(?:_LONG)?|C_DOUBLE_COMPLEX|"
                    r"C_FLOAT_COMPLEX|DOUBLE_PRECISION|REAL[48]?)\b"),
    ("interop_io", r"\bH5T_(?:NATIVE|IEEE)_\w*(?:DOUBLE|FLOAT|F64|F32|LLONG|INT64)\w*|"
                   r"\bNC_(?:DOUBLE|FLOAT|INT64)\b|"
                   r"sizeof\s*\(\s*(?:double|float|long|long\s+long|u?int64_t)\s*\)|"
                   r"scanf\s*\([^;]*%l[efg]|\bfwrite\s*\(|\bfread\s*\("),
    ("interop_library", r"\bcblas_[sdcz]\w+|\bLAPACKE_[sdcz]\w+|"
                        r"\b[sdcz](?:gemm|gemv|axpy|dot[uc]?|nrm2|scal|copy|swap|"
                        r"getrf|getrs|gesv|potrf|potrs|posv|syev\w*|geev|gesvd|"
                        r"gels|trsm|trsv|ger|syrk)_?\s*\(|"
                        r"\bfftwf?_(?:plan|execute|malloc|complex)\w*|"
                        r"\bcublas[SDCZ]\w+|\bcusparse[SDCZ]\w+|\bcusolver\w+[SDCZ]\w+|"
                        r"\bcufft(?:Plan|Exec)\w+"),
    ("interop_language", r'extern\s+"C"'),
    ("bit_tricks", r"\breinterpret_cast\b|\bbit_cast\b|\bmemcpy\s*\(|\bunion\b"),
    ("float_equality", r"[=!]=\s*[-+]?" + FP_LITERAL + r"|" + FP_LITERAL + r"[fFlL]?\s*[=!]=(?!=)"),
    ("float_loop_counter", r"\bfor\s*\(\s*(?:const\s+)?(?:double|float|\w*[Rr]eal\w*)\s+\w+\s*="),
    ("reduction", r"reduction\s*\(\s*[+*]|\bstd::(?:accumulate|reduce|inner_product|"
                  r"transform_reduce)\b|\bMPI_SUM\b|\bparallel_reduce\b|"
                  r"\bParallelReduce\w*|\bReduceOps\b|\bParReduce\b|"
                  r"\bthrust::(?:reduce|inner_product|transform_reduce)\b"),
    ("float_int_conversion", r"\bstatic_cast\s*<\s*(?:int|long|unsigned|(?:std::)?size_t|"
                             r"u?int(?:8|16|32|64)_t)\s*>|\(\s*int\s*\)|\(\s*long\s*\)|"
                             r"\b(?:std::)?(?:l?l?round|l?l?rint|nearbyint)\s*\("),
    ("time_accumulation", r"\b(?:t|time|\w*_time|time_\w*|t_(?:cur|new|now))\s*\+=\s*[\w.>:\-\[\]]*dt\w*"),
    # sum += expr;  total -= expr;  obj.sum += expr  (not i += 1, not a[i] += ...)
    ("accumulation_candidate", r"^\s*[A-Za-z_]\w*(?:(?:\.|->)\w+)*\s*[+\-]=\s*(?!\d+\s*;)"),
])

CPP_ALIAS = re.compile(
    r"\btypedef\s+(?:double|float)\s+(\w+)\s*;|"
    r"\busing\s+(\w+)\s*=\s*(?:double|float)\s*;|"
    r"#\s*define\s+(\w*(?:REAL|PRECISION|FLOAT|DOUBLE|SINGLE)\w*)\b|"
    r"\b(amrex::(?:Particle)?Real|Kokkos::Experimental::half_t)\b|"
    r"(?<=\d)(_rt|_prt)\b")

FORTRAN_KIND = r"(?:kind\s*=\s*)?"
FORTRAN_PATTERNS = _compile([
    ("type_fp_extended", r"(?i)\breal\s*\*\s*16\b|\breal\s*\(\s*" + FORTRAN_KIND + r"(?:16|real128|qp|c_long_double)\s*\)"),
    ("type_fp64", r"(?i)\bdouble\s+precision\b|\breal\s*\*\s*8\b|\bcomplex\s*\*\s*16\b|"
                  r"\bdouble\s+complex\b|"
                  r"\b(?:real|complex)\s*\(\s*" + FORTRAN_KIND + r"(?:8|dp|dbl|r8|rk8|real64|c_double|kind\s*\(\s*1\.0?d0\s*\))\s*\)"),
    ("type_fp32", r"(?i)\breal\s*\*\s*4\b|\bcomplex\s*\*\s*8\b|"
                  r"\b(?:real|complex)\s*\(\s*" + FORTRAN_KIND + r"(?:4|sp|r4|rk4|real32|c_float)\s*\)|"
                  r"^\s*real\s*(?:::|,|\s+[a-z_])"),
    ("type_int64", r"(?i)\binteger\s*\*\s*8\b|"
                   r"\binteger\s*\(\s*" + FORTRAN_KIND + r"(?:8|i8|ik8|int64|c_long|c_long_long|c_int64_t|c_size_t|mpi_offset_kind)\s*\)"),
    ("implicit_typing", r"(?i)^\s*implicit\s+(?:real|double\s+precision|complex)"),
    ("literal_fp64", r"(?i)(?<![\w.])(?:\d+\.?\d*|\.\d+)d[+-]?\d+"),
    ("math_precision_specific", r"(?i)(?<![\w%])(?:dsqrt|dexp|dlog|dlog10|dsin|dcos|dtan|"
                                r"dasin|dacos|datan|datan2|dsinh|dcosh|dtanh|dabs|dmax1|"
                                r"dmin1|dsign|dmod|dble|dfloat|dreal|dimag|dcmplx|dconjg|"
                                r"idint|idnint|dint|dnint|ddim|dprod|cdabs|cdsqrt|cdexp|"
                                r"zabs|derf|derfc|dgamma|alog|alog10|amax1|amin1|amod|"
                                r"sngl|float|csqrt|cabs)\s*\("),
    ("epsilon_query", r"(?i)(?<![\w%])(?:epsilon|tiny|huge|precision|range|digits|spacing|nearest)\s*\("),
    ("interop_mpi", r"(?i)\bMPI_(?:DOUBLE_PRECISION|REAL[48]?|DOUBLE_COMPLEX|COMPLEX(?:8|16)?|"
                    r"INTEGER8|DOUBLE|FLOAT)\b"),
    ("interop_io", r"(?i)\brecl\s*=|\baccess\s*=\s*['\"](?:direct|stream)|"
                   r"\bform\s*=\s*['\"]unformatted|\bnf90?_(?:double|float|real|int64)\b|"
                   r"\bh5t_native_(?:double|real)\w*|\binquire\s*\(\s*iolength"),
    ("interop_library", r"(?i)\bcall\s+[sdcz](?:gemm|gemv|axpy|scal|copy|swap|getrf|getrs|"
                        r"gesv|potrf|potrs|posv|syev\w*|geev|gesvd|gels|trsm|trsv|ger|syrk)\b|"
                        r"(?<![\w%])[sd](?:dot|nrm2|asum)\s*\(|\b[sd]?fftw_\w+"),
    ("interop_language", r"(?i)\bbind\s*\(\s*c\b|\bc_loc\s*\(|\bc_f_pointer\s*\("),
    ("bit_tricks", r"(?i)(?<![\w%])transfer\s*\(|^\s*equivalence\s*\("),
    ("float_equality", r"(?i)(?:==|/=|\.eq\.|\.ne\.)\s*[-+]?(?:\d+\.\d*|\.\d+)"),
    ("reduction", r"(?i)(?<![\w%])(?:sum|dot_product|norm2|product)\s*\(|reduction\s*\(\s*[+*]|\bMPI_SUM\b"),
    ("float_int_conversion", r"(?i)(?<![\w%])(?:nint|anint|floor|ceiling|aint|int)\s*\("),
    ("time_accumulation", r"(?i)\b(t|time|\w*_time|time_\w*)\s*=\s*\1\s*\+\s*[\w%]*dt\w*"),
    # s = s + expr  (scalar self-update; not i = i + 1)
    ("accumulation_candidate", r"(?i)^\s*([a-z_]\w*)\s*=\s*\1\s*[+\-]\s*(?!\d+\s*$)"),
])

FORTRAN_ALIAS = re.compile(
    r"(?i)\bselected_real_kind\s*\(|\bselected_int_kind\s*\(|"
    r"\b(real32|real64|real128|int32|int64|c_double|c_float)\b|"
    r"\binteger\s*,\s*parameter\s*::\s*(\w+)\s*=\s*(?:kind|selected_real_kind|real64|real32|8|4)\b")
FORTRAN_KIND_USE = re.compile(r"(?i)\b(?:real|complex|integer)\s*\(\s*(?:kind\s*=\s*)?([a-z_]\w*)\s*\)")
FORTRAN_KIND_LITERAL = re.compile(r"(?<![\w.])(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?_([A-Za-z]\w*)")
FORTRAN_DEFAULT_LITERAL = re.compile(r"(?<![\w.])(?:\d+\.\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?|\d+[eE][+-]?\d+)(?![\w.])")
CPP_FP64_LITERAL = re.compile(r"(?<![\w.'])" + FP_LITERAL + r"(?![\w.'])")
ANY_FP_LITERAL = re.compile(r"(?<![\w.])((?:\d+\.\d*|\.\d+|\d+)(?:[eEdD][+-]?\d+)?)")

# an equality whose literal is zero: 0. / 0.0 / .0 / 0.0e0, with optional suffix
ZERO_COMPARE = re.compile(r"(?<![\d.])(?:0+\.0*|\.0+)(?:[eEdD][+-]?\d+)?(?![\d])")

BUILD_FLAGS = re.compile(
    r"(?<![\w-])(-ffast-math|-Ofast|-fassociative-math|-funsafe-math-optimizations|"
    r"-freciprocal-math|-ffinite-math-only|-fno-signed-zeros|-fno-trapping-math|"
    r"--use_fast_math|-use_fast_math|-fp-model[= ]\w+|-fp-model=\w+|/fp:\w+|-Kfast|-Mfprelaxed|"
    r"-ffp-contract=\w+|-Mfma|-Mnofma|-mfma|-ffloat-store|-fexcess-precision=\w+|"
    r"-fdefault-real-8|-fdefault-double-8|-freal-[48]-real-(?:4|8|10|16)|-r8|-r4|"
    r"-real-size[= ]\d+|-double-size[= ]\d+|-autodouble|-fdefault-integer-8|-i8|"
    r"-ftz|-no-ftz|-daz|-fno-fast-math|-menable-unsafe-fp-math)(?![\w-])")


def strip_cpp(text: str) -> str:
    """Blank out comments, string and character literals; keep line breaks."""
    out = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif ch == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif ch == "R" and nxt == '"':
            m = re.match(r'R"([^()\\ ]{0,16})\(', text[i:])
            if m:
                end = text.find(")" + m.group(1) + '"', i + m.end())
                end = n if end < 0 else end + len(m.group(1)) + 2
                out.append("".join(c if c == "\n" else " " for c in text[i:end]))
                i = end
            else:
                out.append(ch)
                i += 1
        elif ch == '"' or (ch == "'" and not (i > 0 and text[i - 1].isalnum())):
            quote = ch
            j = i + 1
            while j < n and text[j] != quote and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(quote + " " * max(0, j - i - 2) + (quote if j - i >= 2 else ""))
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def strip_fortran(text: str, fixed_form: bool) -> str:
    """Blank out comments and character literals; keep line breaks."""
    lines = []
    for line in text.split("\n"):
        if fixed_form and line[:1] in ("c", "C", "*", "!"):
            lines.append("")
            continue
        out, quote = [], None
        for ch in line:
            if quote:
                if ch == quote:
                    quote = None
                    out.append(ch)
                else:
                    out.append(" ")
            elif ch in ("'", '"'):
                quote = ch
                out.append(ch)
            elif ch == "!":
                break
            else:
                out.append(ch)
        lines.append("".join(out))
    return "\n".join(lines)


def literal_value(token: str):
    try:
        return float(token.lower().replace("d", "e"))
    except ValueError:
        return None


def language_of(path: str):
    base = os.path.basename(path)
    ext = os.path.splitext(base)[1]
    if ext in CPP_EXT_CASED or ext.lower() in CPP_EXT:
        return "cpp"
    if ext.lower() in FORTRAN_FREE_EXT:
        return "fortran"
    if ext.lower() in FORTRAN_FIXED_EXT:
        return "fortran-fixed"
    if base.lower() in BUILD_NAMES or ext.lower() in BUILD_EXT:
        return "build"
    return None


def round_to_bits(value: Fraction, bits: int) -> Fraction:
    """Round a positive rational to `bits` significant bits, ties to even."""
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    if Fraction(2) ** exponent > value:
        exponent -= 1
    scale = Fraction(2) ** (exponent - bits + 1)
    scaled = value / scale
    whole = scaled.numerator // scaled.denominator
    rest = scaled - whole
    if rest > Fraction(1, 2) or (rest == Fraction(1, 2) and whole % 2 == 1):
        whole += 1
    return whole * scale


def differs_via_long_double(token: str) -> bool:
    """True if decimal -> x87 long double (64-bit significand) -> double gives
    another double than decimal -> double. A user-defined literal that takes
    `long double` (the common `1.1_r` idiom) rounds this way, so replacing
    the plain literal by it would change the double-precision build in the
    last bit: about 1 literal in 4000 on x86-64. Not an issue where long
    double is binary64 (MSVC) and negligible where it is binary128."""
    try:
        exact = Fraction(token.replace("'", ""))
    except (ValueError, ZeroDivisionError):
        return False
    if not (Fraction(1, 10 ** 300) < exact < Fraction(10 ** 300)):
        return False
    return round_to_bits(round_to_bits(exact, 64), 53) != round_to_bits(exact, 53)


def scan_file(path: str, lang: str, findings_per_file: int):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            raw = handle.read()
    except OSError as err:
        return None, [f"{path}: {err}"]

    counts, findings, extras = Counter(), [], defaultdict(Counter)
    raw_lines = raw.split("\n")

    def note(category: str, lineno: int):
        counts[category] += 1
        if sum(1 for f in findings if f["category"] == category) < findings_per_file:
            findings.append({"category": category, "line": lineno,
                             "text": raw_lines[lineno - 1].strip()[:160]})

    if lang == "build":
        for lineno, line in enumerate(raw_lines, 1):
            for match in BUILD_FLAGS.finditer(line):
                extras["build_fp_flags"][match.group(1)] += 1
                findings.append({"category": "build_fp_flag", "line": lineno,
                                 "text": line.strip()[:160]})
        return {"counts": counts, "findings": findings, "extras": extras}, []

    fortran = lang.startswith("fortran")
    code = strip_fortran(raw, lang == "fortran-fixed") if fortran else strip_cpp(raw)
    patterns = FORTRAN_PATTERNS if fortran else CPP_PATTERNS

    for lineno, line in enumerate(code.split("\n"), 1):
        if not line.strip():
            continue
        for category, regex in patterns:
            for match in regex.finditer(line):
                if category == "float_equality" and ZERO_COMPARE.search(match.group(0)):
                    category_hit = "float_equality_zero"
                else:
                    category_hit = category
                note(category_hit, lineno)

        # literals: precision class, representability in FP32, tolerances
        if fortran:
            for match in FORTRAN_DEFAULT_LITERAL.finditer(line):
                note("literal_default", lineno)
            for match in FORTRAN_KIND_LITERAL.finditer(line):
                extras["literal_kinds"][match.group(1).lower()] += 1
            for match in FORTRAN_KIND_USE.finditer(line):
                extras["declared_kinds"][match.group(1).lower()] += 1
            for match in FORTRAN_ALIAS.finditer(line):
                note("precision_alias", lineno)
        else:
            for match in CPP_FP64_LITERAL.finditer(line):
                before = line[:match.start()].rstrip()
                after = line[match.end():].lstrip()
                wrapped = (before.endswith("(") and after.startswith(")")
                           and len(before) > 1
                           and (before[-2].isalnum() or before[-2] in "_>"))
                note("literal_converted" if wrapped else "literal_fp64", lineno)
                if differs_via_long_double(match.group(0)):
                    note("literal_long_double_route", lineno)
            for match in CPP_ALIAS.finditer(line):
                name = next((g for g in match.groups() if g), None)
                if name and not re.search(r"_(?:H|HPP|HH|HXX)_*$", name):  # include guards
                    extras["alias_candidates"][name] += 1
                    note("precision_alias", lineno)
        has_tolerance_name = bool(TOLERANCE_HINT.search(line))
        for match in ANY_FP_LITERAL.finditer(line):
            token = match.group(1)
            if not re.search(r"[.eEdD]", token):
                continue
            value = literal_value(token)
            if value is None or value == 0.0:
                continue
            magnitude = abs(value)
            if magnitude > FP32_MAX or magnitude < FP32_MIN_NORMAL:
                note("literal_out_of_fp32_range", lineno)
            elif has_tolerance_name and magnitude < 1e-3:
                note("tolerance_candidate", lineno)
            elif magnitude < FP32_EPS and re.search(r"[<>]", line):
                note("tolerance_candidate", lineno)
    return {"counts": counts, "findings": findings, "extras": extras}, []


def excluded(rel: str, patterns) -> bool:
    parts = rel.split(os.sep)
    return any(fnmatch.fnmatch(part, pat) for part in parts for pat in patterns) or \
        any(fnmatch.fnmatch(rel, pat) for pat in patterns)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("source_dir")
    parser.add_argument("--json", dest="json_path", help="write the full inventory here")
    parser.add_argument("--top", type=int, default=20, help="files to list (default 20)")
    parser.add_argument("--exclude", action="append", default=[],
                        help="extra glob to skip (directory name or relative path)")
    parser.add_argument("--no-default-excludes", action="store_true",
                        help="also scan build trees and vendored code")
    parser.add_argument("--findings-per-file", type=int, default=5,
                        help="example lines kept per file and category (default 5)")
    args = parser.parse_args()

    root = os.path.abspath(args.source_dir)
    if not os.path.isdir(root):
        print(f"error: not a directory: {root}", file=sys.stderr)
        return 2
    excludes = list(args.exclude) + ([] if args.no_default_excludes else DEFAULT_EXCLUDES)

    files, totals, languages, errors = [], Counter(), Counter(), []
    extras_total = defaultdict(Counter)
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root)
        dirnames[:] = sorted(d for d in dirnames
                             if not excluded(os.path.normpath(os.path.join(rel_dir, d)), excludes))
        for name in sorted(filenames):
            rel = os.path.normpath(os.path.join(rel_dir, name))
            lang = language_of(name)
            if lang is None or excluded(rel, excludes):
                continue
            result, errs = scan_file(os.path.join(dirpath, name), lang, args.findings_per_file)
            errors.extend(errs)
            if result is None:
                continue
            languages[lang] += 1
            for key, counter in result["extras"].items():
                extras_total[key].update(counter)
            if not result["counts"] and not result["findings"]:
                continue
            score = sum(CATEGORIES[c][0] * n for c, n in result["counts"].items())
            totals.update(result["counts"])
            files.append({"file": rel, "language": lang, "score": round(score, 1),
                          "counts": dict(result["counts"]), "findings": result["findings"]})

    files.sort(key=lambda f: (-f["score"], f["file"]))
    inventory = {
        "root": root,
        "files_scanned": dict(languages),
        "totals": dict(totals),
        "category_help": {c: CATEGORIES[c][1] for c in CATEGORIES},
        "alias_candidates": dict(extras_total["alias_candidates"].most_common(20)),
        "fortran_declared_kinds": dict(extras_total["declared_kinds"].most_common(20)),
        "fortran_literal_kinds": dict(extras_total["literal_kinds"].most_common(20)),
        "build_fp_flags": dict(extras_total["build_fp_flags"]),
        "files": files,
        "errors": errors,
    }
    if args.json_path:
        with open(args.json_path, "w", encoding="utf-8") as handle:
            json.dump(inventory, handle, indent=1)
            handle.write("\n")

    print(f"# Precision inventory: {root}")
    print(f"files scanned: " + ", ".join(f"{n} {lang}" for lang, n in sorted(languages.items())))
    print()
    print("| category | count | meaning |")
    print("|---|---:|---|")
    for category in sorted(totals, key=lambda c: (-CATEGORIES[c][0], -totals[c])):
        print(f"| {category} | {totals[category]} | {CATEGORIES[category][1]} |")
    for title, key in (("Existing alias / switch candidates", "alias_candidates"),
                       ("Fortran kinds in declarations", "fortran_declared_kinds"),
                       ("Fortran kinds on literals", "fortran_literal_kinds"),
                       ("Value-changing build flags", "build_fp_flags")):
        if inventory[key]:
            listing = ", ".join(f"{k} ({v})" for k, v in list(inventory[key].items())[:12])
            print(f"\n{title}: {listing}")
        elif key in ("alias_candidates", "build_fp_flags"):
            print(f"\n{title}: none found")
    print(f"\n## Files to look at first (top {min(args.top, len(files))} of {len(files)} by weighted score)\n")
    print("| file | score | leading categories |")
    print("|---|---:|---|")
    for entry in files[:args.top]:
        lead = sorted(entry["counts"], key=lambda c: -CATEGORIES[c][0] * entry["counts"][c])[:4]
        print(f"| {entry['file']} | {entry['score']} | "
              + ", ".join(f"{c} {entry['counts'][c]}" for c in lead) + " |")
    if errors:
        print(f"\n{len(errors)} file(s) could not be read; see 'errors' in the JSON output.")
    print("\nHeuristic regex scan: counts over-report. Confirm with compiler "
          "diagnostics before acting on a single line.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
