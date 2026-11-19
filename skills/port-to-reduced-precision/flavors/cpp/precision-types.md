# Precision in C, C++ and CUDA C++

Language mechanics for every phase when `config.json:language` is `cpp`.
The rules behind them are in the
[maintainability guide](../../references/maintainability-guide.md).

- [Role Aliases](#role-aliases)
- [Literals and Constants](#literals-and-constants)
- [Math Functions](#math-functions)
- [Limits and Tolerances](#limits-and-tolerances)
- [Boundaries](#boundaries)
- [Compiler Diagnostics](#compiler-diagnostics)
- [Build Wiring](#build-wiring)
- [Probes](#probes)
- [Standalone Test](#standalone-test)
- [Standalone Test for a Kernel with State](#standalone-test-for-a-kernel-with-state)
- [Format Emulation Through the Alias](#format-emulation-through-the-alias)
- [Device Code: CUDA and Kokkos](#device-code-cuda-and-kokkos)

## Role Aliases

One header defines every role. It is the only file that names a concrete
floating-point type for role data:

```cpp
// precision.hpp
#pragma once
#include <limits>

namespace app {

#if defined(APP_PRECISION_SINGLE)
using Real = float;          // role: field and particle data
#else
using Real = double;
#endif
using WideReal = double;     // role: time and global diagnostics, wide in every profile

// 0.5_r is a Real literal: no silent promotion to double
constexpr Real operator""_r(long double x) { return static_cast<Real>(x); }
constexpr Real operator""_r(unsigned long long x) { return static_cast<Real>(x); }

// k machine epsilons of T, for tolerances that follow the type
template <class T> constexpr T eps_multiple(double k) {
  return static_cast<T>(k) * std::numeric_limits<T>::epsilon();
}

}  // namespace app
```

- If the project or its framework already has such aliases and literal
  suffixes, use those. Do not add a second mechanism.
- One macro per role (`APP_PRECISION_SINGLE`, `APP_PARTICLE_PRECISION_SINGLE`),
  set by the build system from `PRECISION_PROFILE`. Kernels never test these
  macros.
- **Alias or template?** A global alias keeps signatures simple and is what
  most codes do. Function templates on the scalar type
  (`template <class T> T kernel(T x)`) let one executable evaluate a kernel
  at two precisions, which is convenient for tests and required for helper
  libraries. Template the helpers; keep the application on aliases unless it
  is templated already.
- A role that differs between profiles by more than its type (a
  representation change) is a different struct behind the same interface,
  not an alias.

## Literals and Constants

An unsuffixed literal is a `double`. In `Real y = x * 0.1;` with
`Real = float`, `x` is converted to double, multiplied in double, and the
result converted back: an FP64 multiply and two conversions in a "single
precision" kernel, and a different rounding from `x * 0.1f`.

| Write | Not |
|-------|-----|
| `0.5_r * x` (user-defined literal) or `Real(0.5) * x` | `0.5 * x` |
| `x * x` | `std::pow(x, 2)` or `pow(x, 2.0)` |
| `Real(1) / 3` | `1.0 / 3.0` (double), `1 / 3` (integer zero) |
| `std::numbers::pi_v<Real>` (C++20), or a `constexpr Real` constant | `M_PI` (a double macro, not standard) |

**A `_r` literal is not always the same double.** A user-defined literal
receives its value as `long double`. On x86-64 that is the 80-bit format, so
`0.562722_r` is rounded twice (decimal → 64-bit significand → `double`), and
for about one decimal literal in 4000 the result differs from the plain
`0.562722` in the last bit. This is one of the reasons the `hi` build agrees
with the original to roundoff and not bit for bit, and by default nothing
needs to be done about it. Only where bit-identical `hi` results were
requested: `scripts/precision-scan.py` lists exactly these literals
(`literal_long_double_route`); write them as `Real(0.562722)`, which rounds
the way the original did. Literals that are exact in binary (`0.5`, `2.0`,
`1024.0`) are never affected, and neither are platforms where `long double`
is the same as `double` (MSVC).

Constants whose *products* leave the range of the target type are combined
once at higher precision and stored in the working type:

```cpp
// Stefan-Boltzmann constant 2 pi^5 k^4 / (15 h^3 c^2): k^4 is about 4e-92
// and h^3 about 3e-100, far outside the range of float, while the result,
// 5.67e-8, is an ordinary Real. Combined in double at compile time.
inline constexpr double pi = 3.14159265358979323846;
inline constexpr double k_B = 1.380649e-23, h_P = 6.62607015e-34, c_0 = 299792458.0;
inline constexpr Real stefan_boltzmann = static_cast<Real>(
    2.0 * pi * pi * pi * pi * pi * k_B * k_B * k_B * k_B /
    (15.0 * h_P * h_P * h_P * c_0 * c_0));
```

A constant needed in two roles gets two names, defined from one literal, so
that host set-up code does not silently pick up the rounded one:

```cpp
namespace constants {
namespace wide { inline constexpr WideReal c = 299792458.0; }   // set-up, time step, diagnostics
inline constexpr Real c = static_cast<Real>(wide::c);            // kernels
}
```

Tables of constants declared `double` (coefficients, lookup tables) are a
common leak: give them the role's type, or convert once when they are built.

## Math Functions

| Write | Not | Why |
|-------|-----|-----|
| `std::sqrt(x)`, `std::abs(x)`, `std::exp(x)` | `sqrtf(x)`, `fabs(x)`, `exp(x)` unqualified in C | `std::` overloads follow the argument type. Suffixed names fix one precision. In C, unqualified names take `double` (use `<tgmath.h>`). |
| `std::abs(x)` | `abs(x)` | Without the right header, `abs` is the integer function and truncates. |
| `std::fma(a, b, c)` | — | Needs hardware FMA to be fast (`-mfma` or a matching `-march` on x86-64). |
| `std::hypot`, `std::expm1`, `std::log1p`, `std::copysign` | hand-written equivalents | Accurate and type-generic. |
| `std::floor(x)` then a checked conversion | `(int)x` on unchecked values | Out-of-range float-to-int conversion is undefined behavior. |

Mixed arguments promote: `std::pow(x, 2.0)` and `std::max(x, 1e-30)` drag a
`float` expression to `double` or fail to compile. Give both arguments the
same type.

## Limits and Tolerances

```cpp
std::numeric_limits<Real>::epsilon()   // 2^-23 (float), 2^-52 (double)
std::numeric_limits<Real>::min()       // smallest NORMAL positive value
std::numeric_limits<Real>::max()
std::numeric_limits<Real>::digits      // significand bits: 24, 53
```

Replace `DBL_EPSILON`, `1e-12`-style numerical tolerances and `1e300`-style
sentinels with expressions of these (`eps_multiple<Real>(64) * scale`).
`numeric_limits<T>::min()` is the smallest *normal* number, not the most
negative one; `lowest()` is that.

Such a replacement changes the `hi` build slightly too (`64 eps` is not
`1e-12`). That is intended: one expression that means the same thing at
every precision, with the `hi` gate confirming that the results stay within
roundoff and the counts unchanged. Only where bit-identical `hi` results
were requested, keep the original value at `hi` by taking the larger of the
original literal and the type's limit:

```cpp
const Real tol = std::max(Real(1e-12), eps_multiple<Real>(8));     // double: 1e-12; float: 9.5e-7
const Real floor_v = std::max(Real(1e-300), std::numeric_limits<Real>::min());  // double: 1e-300
```

**Selecting a form by precision.** Most repairs run the same statements at
every precision, even when that costs the `hi` build a little time or moves
its last digits. Where the precisions really need different statements (a
repair that would slow the `hi` build more than it is worth, a rescaling
that only the narrower type needs), put the branch in a small, readable,
named helper and select it from the type, never with the build macro in a
kernel. Such helpers are fine where they are needed. They are not the first
tool to reach for, and keeping the last digits of the `hi` build is not a
reason for one (unless bit-identical results were requested at setup):

```cpp
// A carry word is needed where the working type cannot hold the running
// value plus its increments. Not at double: 53 bits suffice, and the carry
// would cost 5 % there.
template <class T>
inline constexpr bool carries_rounding = std::numeric_limits<T>::digits < 53;

template <class T>
void advance(T& value, T& carry, T increment) {
  if constexpr (carries_rounding<T>) {
    stable_math::add_with_carry(value, carry, increment);
  } else {
    value += increment;
  }
}
```

Both branches compile in every build. The branch a build does not use still
has to be tested in that build, or it rots unseen: instantiate the helper
with `float` and with `double` in its test, whatever `Real` is.

## Boundaries

Where a type is fixed from outside, convert once in a named place:

- **MPI**: map the alias to a datatype with a small trait
  (`mpi_type<Real>()` returning `MPI_FLOAT` or `MPI_DOUBLE`) instead of
  writing `MPI_DOUBLE` at call sites. Reductions that need accuracy use the
  wide role and its datatype.
- **I/O**: follow `io_policy`. To keep the on-disk precision, convert in the
  writer/reader (`std::vector<double>` buffer or a streaming conversion), and
  never write `sizeof(double)` or raw `fwrite` of role data elsewhere.
  `printf("%g", x)` is safe (variadic promotion); `scanf("%lf", &x)` is not
  when `x` became `float`.
- **Libraries**: BLAS/LAPACK/FFT entry points encode the precision in their
  names (`dgemm`/`sgemm`, `fftw_`/`fftwf_`, `cublasD`/`cublasS`). Dispatch on
  the alias in one overloaded wrapper per routine.
- **C and Fortran interop**: the interface declares concrete types
  (`c_double`). Either the interface follows the alias on both sides, or it
  stays fixed and the caller converts.
- **Bit-level code** (`memcpy` of floats, unions, hashing of bit patterns,
  serialization): the layout changes with the type. Each such site needs a
  deliberate rewrite, not an alias.

## Compiler Diagnostics

Turn these on for the `lo` build of the files in scope; they give the exact
locations the inventory only estimates.

| Compiler | Flags | Finds |
|----------|-------|-------|
| GCC | `-Wdouble-promotion` | `float` silently promoted to `double` (literals, math calls) |
| GCC | `-Wfloat-conversion` (part of `-Wconversion`) | `double` silently narrowed to `float` |
| GCC | `-Wconversion` | integer narrowing, int↔float conversions |
| GCC, Clang | `-Wfloat-equal` | `==` / `!=` on floating point |
| Clang | `-Wdouble-promotion -Wimplicit-float-conversion -Wimplicit-int-float-conversion` | as above |
| Clang | `-Wshorten-64-to-32` | 64-bit integer silently narrowed to 32-bit |
| clang-tidy | `bugprone-narrowing-conversions` | narrowing the warnings miss |
| clang-tidy | `performance-type-promotion-in-math-fn` | C math call that promotes a `float` argument |
| clang-tidy | `bugprone-implicit-widening-of-multiplication-result` | `int * int` assigned to a 64-bit value |
| clang-tidy | `bugprone-integer-division`, `bugprone-incorrect-roundings`, `bugprone-too-small-loop-variable`, `cert-flp30-c` | integer division in a float context, `(int)(x + 0.5)`, narrow loop counters, float loop counters |
| nvcc | `-Xptxas --warn-on-double-precision-use` | FP64 instructions in device code (reports PTX lines) |

`-Wdouble-promotion` on a `lo` build is the fastest way to find FP64 leaks
at their source; `scripts/fp64-audit.py` confirms on the binary that none
are left. Check a flag against the installed compiler's documentation before
adding it to a build file.

## Build Wiring

The build system turns the profile into the role macros. CMake:

```cmake
set(APP_PRECISION "DOUBLE" CACHE STRING "Working precision: DOUBLE or SINGLE")
set_property(CACHE APP_PRECISION PROPERTY STRINGS DOUBLE SINGLE)
if(APP_PRECISION STREQUAL "SINGLE")
  target_compile_definitions(app PUBLIC APP_PRECISION_SINGLE)
endif()
```

Plain Make has no build directories and does not notice a changed flag:
after `make`, a later `make PRECISION=SINGLE` finds the binary up to date and
the "single" run silently executes the double one. Give each precision its
own object directory and binary name:

```make
PRECISION ?= DOUBLE
ifeq ($(PRECISION),SINGLE)
  CPPFLAGS += -DAPP_PRECISION_SINGLE
endif
BUILD := build/$(PRECISION)
$(BUILD)/app: $(patsubst %.cpp,$(BUILD)/%.o,$(SRCS))
	$(CXX) $(LDFLAGS) $^ -o $@
$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
$(BUILD):
	mkdir -p $@
```

The scripts in `.precision-port/` map `PRECISION_PROFILE=hi|lo` to
`-DAPP_PRECISION=DOUBLE|SINGLE` and use one build directory per profile
([setup guide](../../references/setup-guide.md#scripts)). An option of the
build system does not become a preprocessor symbol by itself; check that
the definition reaches the compile line (`make VERBOSE=1`, or
`compile_commands.json`). The quickest end-to-end check that a `lo` build is
really `lo`: print `sizeof(app::Real)` once at start-up in a debug line, and
run the FP64 audit.

## Probes

Copy `assets/probe/precision_probe.hpp` into the project and compile the
analysis build with `-DPRECISION_PROBE`:

```cpp
#include "precision_probe.hpp"

void accumulate_moments(const double* x, std::size_t n, Moments& m) {
  PP_RANGE_ARRAY("moments:x:in", x, n);
  for (std::size_t i = 0; i < n; ++i) {
    m.sum = PP_ADD("moments:sum+=x", m.sum, x[i]);                 // operand gap: absorption
    m.sum_sq = PP_ADD("moments:sum_sq+=x^2", m.sum_sq, x[i] * x[i]);
  }
  const double mean = m.sum / static_cast<double>(n);
  m.variance = PP_SUB("moments:<x^2>-mean^2", m.sum_sq / static_cast<double>(n),
                      mean * mean);                                // cancelled bits
  PP_COUNT("moments:n", n);
}
```

| Macro | Records |
|-------|---------|
| `PP_RANGE(name, x)`, `PP_RANGE_ARRAY(name, ptr, n)` | extremes, binade histogram, zeros, subnormals, integer-valuedness |
| `PP_SUB(name, a, b)`, `PP_ADD(name, a, b)` | evaluates to `a − b` / `a + b`; cancelled bits and operand gap |
| `PP_DEMOTE(name, ref, lo)`, `PP_DEMOTE_ARRAY(name, ref, lo, n)` | amplification of a lower-precision evaluation |
| `PP_COUNT(name, n)` | integer extremes |

Without `PRECISION_PROBE` the macros compile to nothing (`PP_SUB`/`PP_ADD`
to the plain operation). Scalar probes in hot loops take a lock per call:
probe arrays at function boundaries where possible and set
`PRECISION_PROBE_STRIDE` to sample. Output goes to `PRECISION_PROBE_DIR`;
summarize with `scripts/probe-summary.py`. Host code only.

Probes belong to the analysis, on the original source. `PP_RANGE*`,
`PP_DEMOTE*` and `PP_COUNT` are statements and can stay through a round.
`PP_SUB` and `PP_ADD` wrap an expression: they make the mechanical diff of
Phase 3 hard to compare with the original and have to be rewritten whenever
Phase 4 changes that expression. Commit the probe calls with the analysis
(so that it can be repeated), then **remove the wrapping probes in a commit
of their own before Phase 3**; re-insert them from that commit when a later
round analyzes the same code again.

## Standalone Test

A complete test for one repaired function, built once per profile. The
function under test (`kinetic.hpp`):

```cpp
// (gamma - 1) for u = gamma * v
inline Real gamma_minus_one(Real u, Real inv_c2) {
  // Written as sqrt1pm1(u^2/c^2): the direct difference sqrt(1 + x) - 1
  // loses all digits in single precision for u/c below about 3e-4.
  return stable_math::sqrt1pm1(u * u * inv_c2);
}
```

The test:

```cpp
#include "kinetic.hpp"              // current function, at the build's working precision
#include "precision_compare.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

// Frozen copy of the original at the original precision. Test-only: never
// linked into the application, never regenerated from the modified function.
static double gamma_minus_one_reference(double u, double inv_c2) {
  return std::sqrt(1.0 + u * u * inv_c2) - 1.0;
}

int main() {
  using app::Real;
  namespace pp = precision_port;
  const double c = 299792458.0, inv_c2 = 1.0 / (c * c);

  std::vector<double> reference;
  std::vector<Real> candidate;
  std::vector<long double> truth;
  for (double u = 1.0e2; u <= 1.0e12; u *= 1.05) {   // assumed range of u, log-spaced, both ends
    reference.push_back(gamma_minus_one_reference(u, inv_c2));
    const Real ur = static_cast<Real>(u);             // the function really receives rounded inputs
    const Real ic = static_cast<Real>(inv_c2);
    candidate.push_back(app::gamma_minus_one(ur, ic));
    const long double x = static_cast<long double>(ur) * ur * ic;
    truth.push_back(x / (std::sqrt(1.0L + x) + 1.0L));   // stable form, higher precision
  }
  const std::size_t n = truth.size();
  const char* precision = sizeof(Real) == 4 ? "fp32" : "fp64";

  // Before/after table, in eps of each row's own precision
  pp::print_accuracy_header();
  pp::print_accuracy_row("gamma_minus_one", "original", "fp64",
                         pp::error_stats(reference.data(), truth.data(), n));
  pp::print_accuracy_row("gamma_minus_one", "reformulated", precision,
                         pp::error_stats(candidate.data(), truth.data(), n));

  // Gate: as accurate as the working precision allows, against the truth
  const auto budget = pp::default_budget<Real, long double>();
  const bool ok = pp::report_and_gate(
      pp::compare_field(candidate.data(), truth.data(), n, budget), budget,
      "gamma_minus_one vs truth");

  // Did results move relative to the original? Not a gate here, because the
  // truth has adjudicated; at hi it is what the developer must be told.
  const auto drift = pp::default_budget<Real, double>();
  if (!pp::report_and_gate(pp::compare_field(candidate.data(), reference.data(), n, drift),
                           drift, "gamma_minus_one vs original")) {
    std::printf("NOTE: results differ from the original beyond its budget "
                "(adjudicated by the truth above)\n");
  }
  return ok ? 0 : 1;
}
```

Its output in the `lo` build shows what the repair bought, and that the
*original* was only good to three digits in double precision at the low end
of the range:

```text
| function | variant | precision | max err [eps] | p99 [eps] | mean [eps] | worst digits | non-finite |
| gamma_minus_one | original     | fp64 | 9.82e+12 | 2.08e+12 | 9.71e+10 | 2.7 | 0 |
| gamma_minus_one | reformulated | fp32 | 1.23     | 1.08     | 0.335    | 6.8 | 0 |
VERIFICATION PASSED (narrower): gamma_minus_one vs truth
VERIFICATION FAILED (outliers, narrower) - review before waiving: gamma_minus_one vs original
NOTE: results differ from the original beyond its budget (adjudicated by the truth above)
```

Notes:

- Without a truth, gate against the frozen reference instead
  (`default_budget<Real, double>()`), and a difference at `hi` beyond that
  budget cannot be accepted.
- `long double` is wider than `double` on x86-64 and on AArch64 Linux, not
  everywhere. Check `std::numeric_limits<long double>::digits`; otherwise use
  `__float128` or a multiprecision library for the truth.
- Build the test with the same value-affecting flags as the application.
- Captured inputs: where a sweep cannot represent the workload (correlated
  arguments, a state array), dump a sample of real inputs at the reference
  precision from the probe build and read it in the test.

## Standalone Test for a Kernel with State

A kernel that advances state every step needs a different test than a
formula ([verification guide](../../references/verification-guide.md#kernels-with-state)):
a budget in the state's own unit at the intended run length, a truth that
starts from the candidate's state and parameters, and, for a periodic
quantity, a comparison modulo the period. A complete example, built once per
profile like the one above:

```cpp
// precision_tests/test_advance_phase/test_advance_phase.cpp
// A kernel with state: n oscillators, phase in cycles in [0, 1), advanced by
// freq * dt every step. Assumed: freq >= 0, freq * dt < 1.
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "precision.hpp"
#include "precision_compare.hpp"
#include "stable_math.hpp"

// The ORIGINAL update, as a template on the type. One body gives the frozen
// reference (double) and the "naive at this precision" row (Real).
template <class T>
void advance_phase_original(T* phi, const T* freq, std::size_t n, T dt) {
  for (std::size_t i = 0; i < n; ++i) {
    phi[i] += freq[i] * dt;
    phi[i] -= std::floor(phi[i]);
  }
}

// The CANDIDATE (in a real test: #include it from the source tree). Templated
// as well, so that the truth is the same stable form at a wider precision.
template <class T>
void advance_phase(T* phi, T* carry, const T* freq, std::size_t n, T dt) {
  for (std::size_t i = 0; i < n; ++i) {
    stable_math::add_with_carry(phi[i], carry[i], freq[i] * dt);
    phi[i] -= std::floor(phi[i]);
  }
}

// Largest distance between two sets of phases, modulo one cycle: a wrap that
// happens one step apart in the two runs is not an error of 1.
template <class A, class B>
std::vector<double> phase_miss(const std::vector<A>& a, const std::vector<B>& b) {
  std::vector<double> miss(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    const long double d = static_cast<long double>(a[i]) - static_cast<long double>(b[i]);
    miss[i] = static_cast<double>(d - std::round(d));
  }
  return miss;
}

int main() {
  using app::Real;
  namespace pp = precision_port;
  const std::size_t n = 1000;
  const int steps = 4000;                   // the run length the budget is for
  const double dt_ref = 1.0e-4;

  // 1. Initial state and parameters at the reference precision.
  std::vector<double> phi_ref(n), freq_ref(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double u = static_cast<double>(i) / static_cast<double>(n - 1);
    phi_ref[i] = std::fmod(618.033988749895 * u, 1.0);
    freq_ref[i] = 0.1 * std::pow(100.0, u);               // 0.1 .. 10, log-spaced
  }

  // 2. The candidate starts from that state, rounded once to its own type.
  std::vector<Real> phi(n), carry(n, Real(0)), freq(n), phi_naive(n);
  for (std::size_t i = 0; i < n; ++i) {
    phi[i] = phi_naive[i] = static_cast<Real>(phi_ref[i]);
    freq[i] = static_cast<Real>(freq_ref[i]);
  }
  const Real dt = static_cast<Real>(dt_ref);

  // 3. The truth starts from the state and parameters THE CANDIDATE HAS, not
  //    from the double ones: this test judges the update, not the rounding
  //    of freq and dt (that shows in the application's QoIs instead).
  std::vector<long double> phi_true(phi.begin(), phi.end()), carry_true(n, 0.0L);
  const std::vector<long double> freq_true(freq.begin(), freq.end());

  for (int s = 0; s < steps; ++s) {
    advance_phase_original(phi_ref.data(), freq_ref.data(), n, dt_ref);
    advance_phase_original(phi_naive.data(), freq.data(), n, dt);
    advance_phase(phi.data(), carry.data(), freq.data(), n, dt);
    advance_phase(phi_true.data(), carry_true.data(), freq_true.data(), n,
                  static_cast<long double>(dt));
  }

  // 4. Gate: an absolute budget in the quantity's own unit, taken from what
  //    the application needs at this run length (here: 1e-5 of a cycle). A
  //    relative error is meaningless for a phase, and "16 eps" is not a
  //    budget for 4000 accumulated steps: what the carry leaves is the
  //    rounding of freq * dt itself, which repeats every step and grows with
  //    the phase advanced (here up to 4 cycles: about 4 eps).
  const std::vector<double> zero(n, 0.0);
  const std::vector<double> miss_naive = phase_miss(phi_naive, phi_true);
  const std::vector<double> miss = phase_miss(phi, phi_true);
  const std::vector<double> miss_ref = phase_miss(phi, phi_ref);
  const pp::Budget budget = pp::absolute_budget<Real, long double>(1.0e-5);

  const pp::CompareResult before = pp::compare_field(miss_naive.data(), zero.data(), n, budget);
  const pp::CompareResult after = pp::compare_field(miss.data(), zero.data(), n, budget);
  const pp::CompareResult to_ref = pp::compare_field(miss_ref.data(), zero.data(), n, budget);
  std::printf("after %d steps, largest phase error against the truth [cycles]:\n", steps);
  std::printf("  original update at this precision  %.3e\n", before.max_abs);
  std::printf("  candidate                          %.3e\n", after.max_abs);
  std::printf("information: candidate against the frozen double reference %.3e "
              "(includes the rounding of freq and dt)\n", to_ref.max_abs);
  return pp::report_and_gate(after, budget, "advance_phase vs truth [cycles]") ? 0 : 1;
}
```

Its output in the `lo` build, and in the `hi` build:

```text
after 4000 steps, largest phase error against the truth [cycles]:
  original update at this precision  1.189e-04
  candidate                          2.363e-07
information: candidate against the frozen double reference 3.598e-07 (includes the rounding of freq and dt)
compare advance_phase vs truth [cycles] [absolute]: max_abs 2.36293e-07  (abs_tol 1.000e-05, 0.0236 of it)
  n_exceed 0 of 1000  worst_idx 995  (wmad 2.296e-05, not gated)
VERIFICATION PASSED (absolute): advance_phase vs truth [cycles]

  original update at this precision  2.210e-13
  candidate                          3.871e-16
```

Three things to take from it:

- The two templates give four runs: the frozen reference
  (`advance_phase_original<double>`), the lowered original for the "before"
  number, the candidate, and the truth (`advance_phase<long double>`). No
  hand-made lowered copy is needed. If the application's function is not a
  template, a test-only template copy of the *original* body serves the
  same purpose; the candidate is always the application's own code.
- The original double code is itself 2e-13 cycles (about 2000 eps) from the
  truth after 4000 steps. An eps-count gate would reject the reference.
- With the carry, what is left in single precision (2e-7 cycles) is the
  rounding of `freq * dt`, repeated every step. It scales with the phase
  advanced, not with the step count. A budget tighter than that cannot be
  met by any summation scheme in this type.

## Format Emulation Through the Alias

With the CCCL FP component available (`scripts/env-check.sh` reports the
include path), the analysis can lower a role without changing its storage:

```cpp
// precision.hpp, analysis build only (-DCCCL_ENABLE_FPTOOL -DAPP_EMULATE_FP32)
#include <cuda/fptool>
using Real = cuda::fp64_custom<8, 23>;     // FP32 range and precision
// using Real = cuda::fp64_custom<11, 23>; // FP32 precision, FP64 range
```

```cpp
template <class T> T gamma_minus_one_naive(T u, T c) {
  const T g = sqrt(T(1.0) + u * u / (c * c));   // unqualified: found for both types
  return g - T(1.0);
}

using F32 = cuda::fp64_custom<8, 23>;
double ref = gamma_minus_one_naive<double>(3.0e4, 299792458.0);          // 5.0e-9
double emu = static_cast<double>(
    gamma_minus_one_naive<F32>(F32{3.0e4}, F32{299792458.0}));           // 0: all digits lost
```

- Construction from `double` is explicit by default
  (`CCCL_FP_CUSTOM_EXPLICIT_CASTS=0` lifts that for an alias swap).
- Math functions are found unqualified (argument-dependent lookup); only
  `sqrt` and `fma` round to the emulated format, every other function
  computes in full FP64.
- The same source runs in host and device code. Sizes can be set at run
  time with `cuda::fp_custom_dynamic_size` and the
  `cuda::fp_custom_set_host_mantissa_size` /
  `cuda::fp_custom_set_device_mantissa_size` setters for a significand
  sweep.

Interpretation and caveats are in the
[analysis guide](../../references/analysis-guide.md#format-emulation-with-the-cccl-fptool).
This is an analysis device: it never ships in the ported code.

## Device Code: CUDA and Kokkos

- **Decorations.** Helpers called from kernels need the framework's
  host/device decoration. `stable_math.hpp` takes it from `STABLE_MATH_FN`
  (define it as `__host__ __device__ inline`, `KOKKOS_INLINE_FUNCTION`, or
  the project's own macro before including the header).
- **Probes do not run on the device.** Probe arrays before and after kernel
  launches on the host, or run Phase 1 on a CPU build.
- **Fast math.** Check whether device code is compiled with a fast-math
  option (for `nvcc`, `--use_fast_math`; some frameworks enable it by
  default). It allows reassociation and replaces division, square root and
  some transcendentals by approximate versions. Both matter more at lower
  precision: record it at setup, and treat changing it as a decision.
- **Conversions are FP64-rate instructions.** On a device with scarce FP64,
  a `float` kernel that touches a `double` (a literal, a `WideReal` member, a
  promoted argument) pays for the conversion on the FP64 units. Keep role
  boundaries outside kernels; verify with `scripts/fp64-audit.py` and the
  executed conversion count from Nsight Compute.
- **Atomics and reductions.** Atomic adds and framework reductions operate
  on the element type. An accurate reduction over single-precision data
  needs the compensated or wide accumulator inside the reduction's value
  type, with a matching combine step. An atomic add cannot carry a
  compensation: one atomic operation updates one word. For scatter-adds
  that must stay accurate over many steps, accumulate per step in the
  working type and fold into the long-lived array in a separate pass
  ([catalog, section C](../../references/reformulation-catalog.md#c-accumulation-rung-l4)).
- **Wider arithmetic on the device.** `cuda::fp32mp2` (float-float) and
  `cuda::fp64emu` from the CCCL FP component work in kernels; see the
  [performance guide](../../references/performance-guide.md#choosing-wider-arithmetic)
  for when they pay off.
- **Half precision.** CUDA provides `__half` with conversion and arithmetic
  intrinsics in `cuda_fp16.h`; C++23 has `std::float16_t` in `<stdfloat>`,
  GCC and Clang `_Float16`; Kokkos has a half type. See
  [fp32-to-fp16](../../transitions/fp32-to-fp16.md).
