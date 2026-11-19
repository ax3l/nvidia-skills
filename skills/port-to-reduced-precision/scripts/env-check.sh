#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

# Environment check for the reduced-precision porting skill.
# Usage: env-check.sh [cpp|fortran] [--gpu]
#   cpp | fortran   source language of the project (default: cpp)
#   --gpu           the target is a CUDA GPU: also check the NVIDIA tools
#
# Required tools fail the check (exit 1). Everything else is reported as
# available or missing so the analysis plan can be chosen accordingly;
# missing optional tools are never an error.
# Environment: CCCL_ROOT may point at a CCCL checkout or install prefix.

set -euo pipefail

LANGUAGE="cpp"
WANT_GPU=0
for arg in "$@"; do
    case "$arg" in
        cpp|fortran) LANGUAGE="$arg" ;;
        --gpu) WANT_GPU=1 ;;
        -h|--help) sed -n '4,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

PASS=0
FAIL=0
MISSING=0

have() { command -v "$1" > /dev/null 2>&1; }

required() {  # label, command...
    local label="$1"; shift
    if "$@" > /dev/null 2>&1; then
        echo "  [OK]      $label"; PASS=$((PASS + 1))
    else
        echo "  [FAIL]    $label"; FAIL=$((FAIL + 1))
    fi
}

optional() {  # label, purpose, command...
    local label="$1" purpose="$2"; shift 2
    if "$@" > /dev/null 2>&1; then
        echo "  [OK]      $label"; PASS=$((PASS + 1))
    else
        echo "  [missing] $label — $purpose"; MISSING=$((MISSING + 1))
    fi
}

first_of() {  # print the first command that exists
    local c
    for c in "$@"; do
        if have "$c"; then printf '%s' "$c"; return 0; fi
    done
    return 1
}

echo "=== Reduced-precision port: environment check ($LANGUAGE) ==="
echo ""
echo "--- Required ---"
required "git" have git
required "python3 (scripts/*.py use only the standard library)" have python3

if [ "$LANGUAGE" = "fortran" ]; then
    FC_FOUND="$(first_of "${FC:-}" gfortran nvfortran ifx flang flang-new ftn || true)"
    if [ -n "$FC_FOUND" ]; then
        echo "  [OK]      Fortran compiler: $FC_FOUND"; PASS=$((PASS + 1))
        "$FC_FOUND" --version 2>/dev/null | head -1 | sed 's/^/            /' || true
    else
        echo "  [FAIL]    Fortran compiler (gfortran, nvfortran, ifx, flang, ftn)"; FAIL=$((FAIL + 1))
    fi
else
    CXX_FOUND="$(first_of "${CXX:-}" g++ clang++ nvc++ icpx CC || true)"
    if [ -n "$CXX_FOUND" ]; then
        echo "  [OK]      C++ compiler: $CXX_FOUND"; PASS=$((PASS + 1))
        "$CXX_FOUND" --version 2>/dev/null | head -1 | sed 's/^/            /' || true
    else
        echo "  [FAIL]    C++ compiler (g++, clang++, nvc++, icpx)"; FAIL=$((FAIL + 1))
    fi
fi

echo ""
echo "--- Static analysis (Phase 1 inventory) ---"
if [ "$LANGUAGE" = "cpp" ]; then
    optional "clang++ (-Wdouble-promotion, -Wimplicit-float-conversion, -Wshorten-64-to-32)" \
             "GCC covers most of these with -Wdouble-promotion -Wfloat-conversion -Wconversion" have clang++
    optional "clang-tidy (bugprone-narrowing-conversions, performance-type-promotion-in-math-fn)" \
             "exact locations for narrowing and math promotions" have clang-tidy
else
    optional "gfortran (-Wconversion -Wconversion-extra; -freal-8-real-4 for a quick demotion experiment)" \
             "use the project compiler's own conversion warnings instead" have gfortran
fi

echo ""
echo "--- Dynamic analysis (Phase 1 sensitivity) ---"
if have valgrind && valgrind --tool=verrou --version > /dev/null 2>&1; then
    echo "  [OK]      Verrou (valgrind --tool=verrou): single-precision emulation and delta-debugging without source changes"
    PASS=$((PASS + 1))
else
    echo "  [missing] Verrou (valgrind --tool=verrou) — emulate FP32 on an unmodified CPU binary; fall back to probes and precision builds"
    MISSING=$((MISSING + 1))
fi
if have herbie || { have racket && racket -l herbie -e '' > /dev/null 2>&1; }; then
    echo "  [OK]      Herbie: suggests more accurate rewrites of an expression"; PASS=$((PASS + 1))
else
    echo "  [missing] Herbie (racket -l herbie) — reformulate by hand with references/reformulation-catalog.md"
    MISSING=$((MISSING + 1))
fi

# CCCL FP component: <cuda/fptool> (fp64_custom), <cuda/fpmp>, <cuda/fpemu>
CCCL_HIT=""
CANDIDATES=()
[ -n "${CCCL_ROOT:-}" ] && CANDIDATES+=("$CCCL_ROOT/libcudacxx/include" "$CCCL_ROOT/include" "$CCCL_ROOT/include/cccl")
[ -n "${CUDA_HOME:-}" ] && CANDIDATES+=("$CUDA_HOME/include/cccl" "$CUDA_HOME/include")
if have nvcc; then
    NVCC_DIR="$(dirname "$(dirname "$(command -v nvcc)")")"
    CANDIDATES+=("$NVCC_DIR/include/cccl" "$NVCC_DIR/include")
fi
for dir in "${CANDIDATES[@]+"${CANDIDATES[@]}"}"; do
    if [ -f "$dir/cuda/fptool" ] && [ -f "$dir/cuda/fpmp" ]; then CCCL_HIT="$dir"; break; fi
done
if [ -n "$CCCL_HIT" ]; then
    echo "  [OK]      CCCL FP component (<cuda/fptool>, <cuda/fpmp>, <cuda/fpemu>): -I$CCCL_HIT"
    PASS=$((PASS + 1))
else
    echo "  [missing] CCCL FP component (<cuda/fptool> fp64_custom, <cuda/fpmp>) — set CCCL_ROOT to a CCCL that has it; without it use precision builds and probes"
    MISSING=$((MISSING + 1))
fi

echo ""
echo "--- Timing (Phase 1 hot spots, Phase 5 assessment) ---"
optional "nsys (Nsight Systems)" "profile with the project's own timers or another profiler" have nsys
PARANOID_FILE=/proc/sys/kernel/perf_event_paranoid
if [ -r "$PARANOID_FILE" ]; then
    PARANOID="$(tr -d '[:space:]' < "$PARANOID_FILE")"
    if [[ "$PARANOID" =~ ^-?[0-9]+$ ]] && [ "$PARANOID" -le 2 ]; then
        echo "  [OK]      perf_event_paranoid = $PARANOID (CPU sampling allowed)"; PASS=$((PASS + 1))
    else
        echo "  [missing] perf_event_paranoid = $PARANOID — nsys CPU sampling needs <= 2; NVTX ranges and GPU traces still work"
        MISSING=$((MISSING + 1))
    fi
fi

if [ "$WANT_GPU" -eq 1 ]; then
    echo ""
    echo "--- CUDA GPU target ---"
    optional "nvidia-smi" "needed to identify the GPU" have nvidia-smi
    optional "nvcc" "needed to build CUDA sources and the FP rate microbenchmark" have nvcc
    optional "ncu (Nsight Compute)" "per-kernel FP64/FP32 instruction counts and pipe utilization" have ncu
    optional "cuobjdump (scripts/fp64-audit.py --mode cuda)" "static audit of FP64 instructions in device code" have cuobjdump
    optional "nvdisasm" "source lines of FP64 instructions (with -lineinfo)" have nvdisasm
    if have nvidia-smi; then
        GPU_INFO="$(nvidia-smi --query-gpu=name,compute_cap,driver_version --format=csv,noheader 2>&1 || true)"
        if [ -n "$GPU_INFO" ] && ! echo "$GPU_INFO" | grep -qi 'fail\|NVML\|blocked'; then
            echo "  GPU: $GPU_INFO"
            echo "  The FP32:FP64 throughput ratio decides which remedies pay off:"
            echo "  measure it with assets/microbench/fp_rate.cu (see references/performance-guide.md)."
        else
            echo "  [missing] GPU not accessible through nvidia-smi"; MISSING=$((MISSING + 1))
        fi
    fi
fi

echo ""
echo "--- CPU floating-point features ---"
if [ -r /proc/cpuinfo ]; then
    FLAGS="$(grep -m1 -E '^(flags|Features)' /proc/cpuinfo || true)"
    for feature in fma f16c avx512_fp16 avx512fp16 fphp asimdhp; do
        if echo "$FLAGS" | grep -qw "$feature"; then echo "  $feature: yes"; fi
    done
    echo "$FLAGS" | grep -qwE 'fma|asimd' || echo "  no hardware FMA reported: std::fma falls back to software (slow)"
    echo "$FLAGS" | grep -qwE 'avx512_fp16|avx512fp16|fphp' || echo "  no half-precision arithmetic reported: FP16 on this CPU is storage/conversion only"
else
    echo "  /proc/cpuinfo not readable: check FMA and FP16 support by other means"
fi

echo ""
echo "Result: $PASS available, $FAIL required missing, $MISSING optional missing"
if [ "$FAIL" -gt 0 ]; then
    echo "Install the required tools before starting the port."
    exit 1
fi
exit 0
