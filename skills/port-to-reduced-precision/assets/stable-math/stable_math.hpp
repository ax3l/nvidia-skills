// SPDX-License-Identifier: Apache-2.0

/// Named, numerically stable building blocks for reduced-precision ports.
///
/// The point of this header is readability: a kernel that says
/// `sqrt1pm1(x)` tells the next reader both what is computed and that the
/// obvious `sqrt(1 + x) - 1` was avoided on purpose. Copy the header into the
/// project once (rename the namespace to the project's own if you like), keep
/// only what the port uses, and call the helpers instead of inlining the
/// tricks at every use site.
///
/// Every function is a template on the floating-point type, so one source
/// serves every precision the project builds. All of them are exact
/// algebraic identities of the textbook expression they replace, not
/// approximations, unless the comment says otherwise.
///
/// Requirements and caveats:
///   - C++17.
///   - Do NOT compile this header with value-unsafe math (-ffast-math,
///     -Ofast, -fassociative-math, nvcc --use_fast_math for the
///     error-free transformations). Reassociation deletes the compensation
///     terms and silently turns the helpers back into the naive forms.
///   - two_prod, diff_of_products and compensated_dot need a real fused
///     multiply-add. On x86-64 build with -mfma (or -march=x86-64-v3 and
///     newer); without it std::fma is a slow software fallback.
///   - Functions marked STABLE_MATH_FN are safe in device code. Define the
///     macro before including the header to add the framework's decoration:
///         #define STABLE_MATH_FN __host__ __device__ inline      // CUDA
///         #define STABLE_MATH_FN KOKKOS_INLINE_FUNCTION          // Kokkos
///     Functions declared plain `inline` are host-only.

#ifndef STABLE_MATH_HPP
#define STABLE_MATH_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>

#ifndef STABLE_MATH_FN
#define STABLE_MATH_FN inline
#endif

namespace stable_math {

// ---------------------------------------------------------------------------
// Tolerances
// ---------------------------------------------------------------------------

/// `k` times the machine epsilon of T. Use this instead of a literal such as
/// 1e-12, which is unreachable in single precision and meaningless in half.
template <class T>
constexpr T eps_multiple(double k) noexcept {
  return static_cast<T>(k) * std::numeric_limits<T>::epsilon();
}

// ---------------------------------------------------------------------------
// Error-free transformations
// ---------------------------------------------------------------------------

/// An unevaluated sum hi + lo. For the transformations below, hi is the
/// rounded result and lo its exact rounding error.
template <class T>
struct Pair {
  T hi;
  T lo;
};

/// a + b = hi + lo exactly, for any magnitudes (Knuth).
template <class T>
STABLE_MATH_FN Pair<T> two_sum(T a, T b) noexcept {
  const T s = a + b;
  const T bb = s - a;
  const T err = (a - (s - bb)) + (b - bb);
  return {s, err};
}

/// a + b = hi + lo exactly, REQUIRES |a| >= |b| or a == 0 (Dekker).
/// Three operations instead of six; wrong when the precondition fails.
template <class T>
STABLE_MATH_FN Pair<T> fast_two_sum(T a, T b) noexcept {
  const T s = a + b;
  const T err = b - (s - a);
  return {s, err};
}

/// a * b = hi + lo exactly (barring over/underflow). Needs a real FMA.
template <class T>
STABLE_MATH_FN Pair<T> two_prod(T a, T b) noexcept {
  const T p = a * b;
  return {p, std::fma(a, b, -p)};
}

// ---------------------------------------------------------------------------
// Cancellation-free forms of common expressions
// ---------------------------------------------------------------------------

/// a*b - c*d, accurate when the two products nearly cancel (Kahan).
/// Replaces determinants, discriminants and cross-product components.
template <class T>
STABLE_MATH_FN T diff_of_products(T a, T b, T c, T d) noexcept {
  const T cd = c * d;
  const T err = std::fma(-c, d, cd);  // exact rounding error of cd
  const T dop = std::fma(a, b, -cd);
  return dop + err;
}

/// a*b + c*d, accurate when the two products nearly cancel.
template <class T>
STABLE_MATH_FN T sum_of_products(T a, T b, T c, T d) noexcept {
  return diff_of_products(a, b, -c, d);
}

/// a^2 - b^2 as (a - b)(a + b): no cancellation of the squares.
template <class T>
STABLE_MATH_FN T diff_of_squares(T a, T b) noexcept {
  return (a - b) * (a + b);
}

/// sqrt(1 + x) - 1 for x >= -1, accurate for small |x|.
/// Example: the relativistic gamma - 1 is sqrt1pm1(u^2 / c^2).
template <class T>
STABLE_MATH_FN T sqrt1pm1(T x) noexcept {
  return x / (std::sqrt(T(1) + x) + T(1));
}

/// 1 - cos(x) as 2 sin^2(x / 2): accurate for small |x|.
template <class T>
STABLE_MATH_FN T one_minus_cos(T x) noexcept {
  const T s = std::sin(x / T(2));
  return T(2) * s * s;
}

/// log(1 - exp(x)) for x < 0, accurate over the whole range.
template <class T>
STABLE_MATH_FN T log1mexp(T x) noexcept {
  return (x > T(-0.69314718055994530942))  // -ln 2
             ? std::log(-std::expm1(x))
             : std::log1p(-std::exp(x));
}

/// log(1 + exp(x)) without overflow for large x.
template <class T>
STABLE_MATH_FN T log1pexp(T x) noexcept {
  return (x > T(0)) ? x + std::log1p(std::exp(-x)) : std::log1p(std::exp(x));
}

/// Real roots of a x^2 + b x + c = 0.
/// n is the number of distinct real roots (0, 1 or 2); a double root is
/// returned in both r1 and r2. The larger-magnitude root is computed first
/// and the other from Vieta's formula, so neither suffers cancellation, and
/// the discriminant uses diff_of_products.
/// Not protected against overflow of b*b: scale the coefficients if they can
/// approach sqrt(max(T)).
template <class T>
struct QuadraticRoots {
  int n;
  T r1;
  T r2;
};

template <class T>
STABLE_MATH_FN QuadraticRoots<T> quadratic_roots(T a, T b, T c) noexcept {
  if (a == T(0)) {
    if (b == T(0)) return {0, T(0), T(0)};
    return {1, -c / b, -c / b};
  }
  const T disc = diff_of_products(b, b, T(4) * a, c);
  if (disc < T(0)) return {0, T(0), T(0)};
  const T q = -(b + std::copysign(std::sqrt(disc), b)) / T(2);
  if (q == T(0)) return {1, T(0), T(0)};  // b == 0 and c == 0
  return {disc == T(0) ? 1 : 2, q / a, c / q};
}

/// sqrt(x^2 + y^2 + z^2) without overflow or underflow of the squares.
/// For two components use std::hypot.
template <class T>
STABLE_MATH_FN T norm3(T x, T y, T z) noexcept {
  const T ax = std::fabs(x);
  const T ay = std::fabs(y);
  const T az = std::fabs(z);
  const T m = std::fmax(ax, std::fmax(ay, az));
  if (m == T(0) || std::isinf(m)) return m;
  const T sx = ax / m;
  const T sy = ay / m;
  const T sz = az / m;
  return m * std::sqrt(sx * sx + sy * sy + sz * sz);
}

/// Linear interpolation that returns exactly a at t = 0 and exactly b at
/// t = 1 (the one-line `a + t * (b - a)` misses b by rounding).
template <class T>
STABLE_MATH_FN T lerp(T a, T b, T t) noexcept {
  return (t < T(0.5)) ? a + t * (b - a) : b - (T(1) - t) * (b - a);
}

// ---------------------------------------------------------------------------
// Accumulation
// ---------------------------------------------------------------------------

/// Running sum with a compensation term (two_sum based: the
/// Kahan-Babuska-Neumaier / Ogita-Rump-Oishi "Sum2" scheme).
///
/// The result is as accurate as if the sum had been accumulated in twice the
/// working precision and then rounded. Unlike classic Kahan summation it
/// stays accurate when a term is larger than the running sum.
///
/// Cost: 7 additions per term instead of 1, and two words of state.
template <class T>
struct CompensatedSum {
  T sum = T(0);
  T comp = T(0);

  STABLE_MATH_FN void add(T x) noexcept {
    const Pair<T> s = two_sum(sum, x);
    sum = s.hi;
    comp += s.lo;
  }

  /// Fold another partial sum in (parallel reductions: combine per-thread or
  /// per-rank accumulators with this, not with plain +).
  STABLE_MATH_FN void merge(const CompensatedSum& other) noexcept {
    const Pair<T> s = two_sum(sum, other.sum);
    sum = s.hi;
    comp += s.lo + other.comp;
  }

  STABLE_MATH_FN T result() const noexcept { return sum + comp; }
};

/// One step of a value that is advanced by many small increments (a
/// position, a momentum, a time, a phase): `value += increment`, with `carry`
/// keeping what the addition rounded away so that the next step adds it back.
///
/// A plain `value += increment` drops the low bits of every increment, in
/// the same direction for as long as `value` stays in one binade, so the
/// error grows with the number of steps; an increment below half a unit in
/// the last place of `value` is lost completely. With the carry,
/// value + carry stays within about one rounding of the exact running total
/// however many steps are taken.
///
/// Cost: 7 additions instead of 1, and one more word of state per value.
/// Initialize the carry to 0; move, copy, sort and checkpoint it together
/// with its value.
template <class T>
STABLE_MATH_FN void add_with_carry(T& value, T& carry, T increment) noexcept {
  const Pair<T> s = two_sum(value, increment + carry);
  value = s.hi;
  carry = s.lo;
}

/// Compensated sum of x[0..n).
template <class T>
STABLE_MATH_FN T compensated_sum(const T* x, std::size_t n) noexcept {
  CompensatedSum<T> acc;
  for (std::size_t i = 0; i < n; ++i) acc.add(x[i]);
  return acc.result();
}

/// Compensated dot product (Ogita-Rump-Oishi "Dot2"): as accurate as a dot
/// product evaluated in twice the working precision. Needs a real FMA.
template <class T>
STABLE_MATH_FN T compensated_dot(const T* x, const T* y,
                                 std::size_t n) noexcept {
  T p = T(0);
  T s = T(0);
  for (std::size_t i = 0; i < n; ++i) {
    const Pair<T> h = two_prod(x[i], y[i]);
    const Pair<T> q = two_sum(p, h.hi);
    p = q.hi;
    s += q.lo + h.lo;
  }
  return p + s;
}

/// Pairwise (cascade) summation: error grows like log2(n) instead of n, at
/// the cost of a plain sum. Host-only (recursive).
template <class T>
inline T pairwise_sum(const T* x, std::size_t n) noexcept {
  if (n <= 128) {
    T s = T(0);
    for (std::size_t i = 0; i < n; ++i) s += x[i];
    return s;
  }
  const std::size_t half = n / 2;
  return pairwise_sum(x, half) + pairwise_sum(x + half, n - half);
}

// ---------------------------------------------------------------------------
// Grid indices and periodic wrapping
// ---------------------------------------------------------------------------

/// Index of the cell containing x on a uniform grid of n cells that starts
/// at x_lo, clamped to [0, n - 1].
///
/// (x - x_lo) * inv_dx can round up to exactly n for a position that is
/// still inside the domain; in single precision that happens routinely.
/// A NaN position maps to 0: validate positions separately.
template <class T>
STABLE_MATH_FN int cell_index_clamped(T x, T x_lo, T inv_dx, int n) noexcept {
  const T s = (x - x_lo) * inv_dx;
  if (!(s > T(0))) return 0;
  if (s >= static_cast<T>(n)) return n - 1;
  const int i = static_cast<int>(s);
  return i < n ? i : n - 1;
}

/// Map x into the half-open periodic interval [lo, hi).
///
/// The textbook wrap can return exactly hi (for x a rounding error below lo,
/// x + (hi - lo) rounds to hi). hi and lo are the same point, so that case
/// is mapped to lo. NaN propagates.
template <class T>
STABLE_MATH_FN T wrap_periodic(T x, T lo, T hi) noexcept {
  const T len = hi - lo;
  T y = x - len * std::floor((x - lo) / len);
  if (y < lo) y = lo;
  if (y >= hi) y = lo;
  return y;
}

/// A position stored as a cell index plus an offset in [0, 1) inside that
/// cell (in units of the cell size): bring the pair back into this form
/// after the offset has been moved. Stored this way, the sub-cell part keeps
/// the full significand whatever the size of the domain, and the integer
/// part is exact. Periodic wrapping and domain bounds become integer
/// operations on `cell`, which the caller applies afterwards.
///
/// An offset a rounding error below 0 belongs to the top of the cell below,
/// where it rounds to exactly 1: that is offset 0 of the original cell, and
/// is returned as such. The move must stay within the range of Int. A NaN
/// offset is left unchanged.
template <class T, class Int>
STABLE_MATH_FN void normalize_cell_offset(Int& cell, T& offset) noexcept {
  if (!(offset < T(0) || offset >= T(1))) return;  // in range, or NaN
  const T whole = std::floor(offset);
  cell += static_cast<Int>(whole);
  offset -= whole;
  if (offset >= T(1)) {
    offset = T(0);
    cell += 1;
  }
}

// ---------------------------------------------------------------------------
// Uniform random numbers from random bits
// ---------------------------------------------------------------------------

// The precision is part of the name on purpose: overloading on the width of
// the integer argument is ambiguous for `unsigned long long` on LP64 systems.

/// Uniform in [0, 1) with 24 random bits; never returns 1.
/// (Dividing a 32-bit integer by 2^32 in float can round up to 1.0f.)
STABLE_MATH_FN float uniform01_f32(std::uint32_t bits) noexcept {
  return static_cast<float>(bits >> 8) * 0x1.0p-24f;
}

/// Uniform in [0, 1) with 53 random bits; never returns 1.
STABLE_MATH_FN double uniform01_f64(std::uint64_t bits) noexcept {
  return static_cast<double>(bits >> 11) * 0x1.0p-53;
}

/// Uniform in (0, 1]: safe as the argument of log().
STABLE_MATH_FN float uniform01_open_low_f32(std::uint32_t bits) noexcept {
  return (static_cast<float>(bits >> 8) + 1.0f) * 0x1.0p-24f;
}

/// Uniform in (0, 1]: safe as the argument of log().
STABLE_MATH_FN double uniform01_open_low_f64(std::uint64_t bits) noexcept {
  return (static_cast<double>(bits >> 11) + 1.0) * 0x1.0p-53;
}

// ---------------------------------------------------------------------------
// Integer narrowing
// ---------------------------------------------------------------------------

/// True if `v` is representable in `To` without changing its value.
template <class To, class From>
constexpr bool fits_in(From v) noexcept {
  static_assert(std::is_integral<To>::value && std::is_integral<From>::value,
                "fits_in is for integer types");
  const To narrowed = static_cast<To>(v);
  if (static_cast<From>(narrowed) != v) return false;
  // Same value but different sign happens across signed/unsigned.
  const bool from_negative = std::is_signed<From>::value && v < From{};
  const bool to_negative = std::is_signed<To>::value && narrowed < To{};
  return from_negative == to_negative;
}

/// Narrow `v` to `To`, terminating with a message if the value does not fit.
/// Host-only. Use it once at the boundary where a quantity enters its narrow
/// representation (setup, I/O, redistribution), not inside hot loops.
template <class To, class From>
inline To checked_narrow(From v, const char* what = "value") {
  if (!fits_in<To>(v)) {
    std::fprintf(stderr,
                 "checked_narrow: %s = %lld does not fit in %zu-bit %s "
                 "integer\n",
                 what, static_cast<long long>(v), 8 * sizeof(To),
                 std::is_signed<To>::value ? "signed" : "unsigned");
    std::abort();
  }
  return static_cast<To>(v);
}

/// Product of two 32-bit values without overflow (nx * ny, n * stride, ...).
STABLE_MATH_FN std::int64_t widen_mul(std::int32_t a, std::int32_t b) noexcept {
  return static_cast<std::int64_t>(a) * static_cast<std::int64_t>(b);
}

}  // namespace stable_math

#endif  // STABLE_MATH_HPP
