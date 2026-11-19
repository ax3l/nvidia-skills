// SPDX-License-Identifier: Apache-2.0

/// Self-test for stable_math.hpp, and a worked example of the before/after
/// accuracy table that every reformulation is judged by.
///
/// Each helper is evaluated in float and in double, next to the naive
/// expression it replaces, against a truth computed in extended precision.
/// Errors are printed in units of the working precision's epsilon.
///
/// Build and run with value-safe flags and a real FMA:
///     g++ -std=c++17 -O2 -mfma -I../test-harness test_stable_math.cpp -o test_stable_math
///     ./test_stable_math
///
/// Exit status 0 means every stable form stayed within its limit.

#include "stable_math.hpp"

#include "precision_compare.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <vector>

namespace {

using precision_port::error_stats;
using precision_port::ErrorStats;
using precision_port::print_accuracy_header;
using precision_port::print_accuracy_row;

int failures = 0;

// The truth is only useful where it is clearly more precise than the type
// under test: x87 extended (64 bits) and binary128 qualify for double.
const bool truth_covers_double = std::numeric_limits<long double>::digits >= 64;

template <class T>
const char* precision_name() {
  return sizeof(T) == 4 ? "fp32" : "fp64";
}

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAILED: %s\n", what);
    ++failures;
  }
}

/// Evaluate a naive and a stable unary form over `inputs` at precision T,
/// print both rows, and require the stable one to stay within `limit_eps`.
template <class T>
void unary_case(const char* name, const std::vector<long double>& inputs,
                const std::function<long double(long double)>& truth_fn,
                T (*naive)(T), T (*stable)(T), double limit_eps) {
  std::vector<T> out_naive, out_stable;
  std::vector<long double> truth;
  for (const long double x : inputs) {
    const T xt = static_cast<T>(x);
    // The truth is evaluated at the ROUNDED input, so that it measures the
    // formula and not the conversion of the input.
    truth.push_back(truth_fn(static_cast<long double>(xt)));
    out_naive.push_back(naive(xt));
    out_stable.push_back(stable(xt));
  }
  const ErrorStats e_naive =
      error_stats(out_naive.data(), truth.data(), truth.size());
  const ErrorStats e_stable =
      error_stats(out_stable.data(), truth.data(), truth.size());
  print_accuracy_row(name, "naive", precision_name<T>(), e_naive);
  print_accuracy_row(name, "stable", precision_name<T>(), e_stable);
  if (sizeof(T) == 8 && !truth_covers_double) return;
  check(e_stable.n_nonfinite == 0, name);
  check(e_stable.max_eps <= limit_eps, name);
}

std::vector<long double> log_sweep(long double lo, long double hi, int n,
                                   bool both_signs = false) {
  std::vector<long double> v;
  const long double ratio = std::pow(hi / lo, 1.0L / (n - 1));
  long double x = lo;
  for (int i = 0; i < n; ++i, x *= ratio) {
    v.push_back(x);
    if (both_signs) v.push_back(-x);
  }
  return v;
}

template <class T>
T naive_sqrt1pm1(T x) { return std::sqrt(T(1) + x) - T(1); }
template <class T>
T naive_one_minus_cos(T x) { return T(1) - std::cos(x); }
template <class T>
T naive_log1mexp(T x) { return std::log(T(1) - std::exp(x)); }
template <class T>
T naive_log1pexp(T x) { return std::log(T(1) + std::exp(x)); }

template <class T>
void run_unary_cases() {
  using namespace stable_math;
  unary_case<T>("sqrt1pm1", log_sweep(1e-12L, 1e6L, 400),
                [](long double x) { return x / (std::sqrt(1.0L + x) + 1.0L); },
                naive_sqrt1pm1<T>, sqrt1pm1<T>, 4.0);
  unary_case<T>("one_minus_cos", log_sweep(1e-9L, 3.0L, 400, true),
                [](long double x) {
                  const long double s = std::sin(x / 2.0L);
                  return 2.0L * s * s;
                },
                naive_one_minus_cos<T>, one_minus_cos<T>, 4.0);
  {
    std::vector<long double> neg;
    for (const long double x : log_sweep(1e-9L, 30.0L, 400)) neg.push_back(-x);
    unary_case<T>("log1mexp", neg,
                  [](long double x) {
                    return x > -0.69314718055994530942L
                               ? std::log(-std::expm1(x))
                               : std::log1p(-std::exp(x));
                  },
                  naive_log1mexp<T>, log1mexp<T>, 6.0);
  }
  unary_case<T>("log1pexp", log_sweep(1e-3L, 80.0L, 400, true),
                [](long double x) {
                  return x > 0.0L ? x + std::log1p(std::exp(-x))
                                  : std::log1p(std::exp(x));
                },
                naive_log1pexp<T>, log1pexp<T>, 4.0);
}

/// Binary helpers on pairs that nearly cancel.
template <class T>
void run_binary_cases() {
  using namespace stable_math;
  std::vector<T> naive_sq, stable_sq, naive_dop, stable_dop;
  std::vector<long double> truth_sq, truth_dop;
  for (const long double rel : log_sweep(1e-6L, 1e-1L, 200)) {
    const T a = static_cast<T>(1.2345678L);
    const T b = static_cast<T>(1.2345678L * (1.0L - rel));
    const long double al = static_cast<long double>(a);
    const long double bl = static_cast<long double>(b);
    truth_sq.push_back((al - bl) * (al + bl));
    naive_sq.push_back(a * a - b * b);
    stable_sq.push_back(diff_of_squares(a, b));

    // a*b - c*d with a*b ~ c*d
    const T c = static_cast<T>(0.9876543L);
    const T d = static_cast<T>(al * bl / static_cast<long double>(c) *
                               (1.0L - rel * 1e-2L));
    const long double cl = static_cast<long double>(c);
    const long double dl = static_cast<long double>(d);
    // The truth cancels as well, so it needs the compensated form too.
    truth_dop.push_back(diff_of_products<long double>(al, bl, cl, dl));
    // volatile keeps the compiler from contracting the naive form into an
    // fma, which would hide the cancellation this row is meant to show
    volatile T ab = a * b;
    volatile T cd = c * d;
    naive_dop.push_back(ab - cd);
    stable_dop.push_back(diff_of_products(a, b, c, d));
  }
  const auto e1 = error_stats(naive_sq.data(), truth_sq.data(), truth_sq.size());
  const auto e2 = error_stats(stable_sq.data(), truth_sq.data(), truth_sq.size());
  print_accuracy_row("diff_of_squares", "naive", precision_name<T>(), e1);
  print_accuracy_row("diff_of_squares", "stable", precision_name<T>(), e2);
  const auto e3 = error_stats(naive_dop.data(), truth_dop.data(), truth_dop.size());
  const auto e4 = error_stats(stable_dop.data(), truth_dop.data(), truth_dop.size());
  print_accuracy_row("diff_of_products", "naive", precision_name<T>(), e3);
  print_accuracy_row("diff_of_products", "stable", precision_name<T>(), e4);
  if (sizeof(T) == 8 && !truth_covers_double) return;
  check(e2.max_eps <= 4.0, "diff_of_squares");
  check(e4.max_eps <= 4.0, "diff_of_products");
}

/// Summation of an ill-conditioned series: large terms that cancel.
template <class T>
void run_sum_cases() {
  using namespace stable_math;
  std::vector<T> x, y;
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  auto next = [&state]() {  // xorshift64*, deterministic test data
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1Dull;
  };
  const int n = 1 << 16;
  for (int i = 0; i < n; ++i) {
    const T big = static_cast<T>(std::ldexp(1.0 + uniform01_f64(next()), 12));
    const T small_a = static_cast<T>(uniform01_f64(next()));
    const T small_b = static_cast<T>(uniform01_f64(next()));
    x.insert(x.end(), {big, small_a, -big, small_b});
  }
  for (std::size_t i = 0; i < x.size(); ++i) {
    y.push_back(static_cast<T>(1.0 + 0.5 * uniform01_f64(next())));
  }

  // Truth: compensated accumulation in extended precision.
  CompensatedSum<long double> sum_truth, dot_truth;
  for (std::size_t i = 0; i < x.size(); ++i) {
    sum_truth.add(static_cast<long double>(x[i]));
    dot_truth.add(static_cast<long double>(x[i]) *
                  static_cast<long double>(y[i]));
  }
  T naive = T(0), naive_dot = T(0);
  for (std::size_t i = 0; i < x.size(); ++i) {
    naive += x[i];
    naive_dot += x[i] * y[i];
  }
  const long double st = sum_truth.result(), dt = dot_truth.result();
  auto row = [&](const char* fn, const char* variant, T value,
                 long double truth) {
    const ErrorStats e = error_stats(&value, &truth, 1);
    print_accuracy_row(fn, variant, precision_name<T>(), e);
    return e;
  };
  row("sum (cancelling)", "naive", naive, st);
  const auto ep = row("sum (cancelling)", "pairwise",
                      pairwise_sum(x.data(), x.size()), st);
  const auto ec = row("sum (cancelling)", "compensated",
                      compensated_sum(x.data(), x.size()), st);
  row("dot (cancelling)", "naive", naive_dot, dt);
  const auto ed = row("dot (cancelling)", "compensated",
                      compensated_dot(x.data(), y.data(), x.size()), dt);
  (void)ep;
  if (sizeof(T) == 8 && !truth_covers_double) return;
  check(ec.max_eps <= 2.0, "compensated_sum");
  check(ed.max_eps <= 2.0, "compensated_dot");

  // merge() must agree with one long accumulation
  CompensatedSum<T> left, right;
  for (std::size_t i = 0; i < x.size() / 2; ++i) left.add(x[i]);
  for (std::size_t i = x.size() / 2; i < x.size(); ++i) right.add(x[i]);
  left.merge(right);
  T merged = left.result();
  check(error_stats(&merged, &st, 1).max_eps <= 2.0, "CompensatedSum::merge");
}

template <class T>
void run_discrete_cases() {
  using namespace stable_math;
  // quadratic with widely separated roots: x^2 - (1e4 + 1e-4) x + 1 = 0
  const T big = T(1e4), tiny = T(1e-4);
  const QuadraticRoots<T> q = quadratic_roots(T(1), -(big + tiny), T(1));
  check(q.n == 2, "quadratic_roots count");
  const T lo_root = std::fmin(q.r1, q.r2), hi_root = std::fmax(q.r1, q.r2);
  check(std::fabs(lo_root - tiny) <= eps_multiple<T>(8) * tiny,
        "quadratic_roots small root");
  check(std::fabs(hi_root - big) <= eps_multiple<T>(8) * big,
        "quadratic_roots large root");
  check(quadratic_roots(T(1), T(0), T(1)).n == 0, "quadratic_roots no root");
  check(quadratic_roots(T(1), T(-2), T(1)).n == 1, "quadratic_roots double");

  // norm3 must survive squares that overflow or underflow
  const T huge = std::sqrt(std::numeric_limits<T>::max()) * T(4);
  const T small = std::sqrt(std::numeric_limits<T>::min()) / T(4);
  check(std::isfinite(norm3(huge, huge, huge)), "norm3 overflow");
  check(norm3(small, small, small) > T(0), "norm3 underflow");
  check(std::fabs(norm3(T(3), T(4), T(12)) - T(13)) <= eps_multiple<T>(4) * 13,
        "norm3 value");

  // lerp hits both endpoints exactly
  const T a = T(0.1), b = T(0.7);
  check(lerp(a, b, T(0)) == a && lerp(a, b, T(1)) == b, "lerp endpoints");

  // cell index: the largest position below the upper edge stays in range
  const int n = 1000;
  const T x_lo = T(0), dx = T(0.001), x_hi = T(1);
  const T inv_dx = T(1) / dx;
  const T last = std::nextafter(x_hi, x_lo);
  check(cell_index_clamped(last, x_lo, inv_dx, n) == n - 1, "cell index top");
  check(cell_index_clamped(x_lo, x_lo, inv_dx, n) == 0, "cell index bottom");
  check(cell_index_clamped(T(-1), x_lo, inv_dx, n) == 0, "cell index below");
  check(cell_index_clamped(T(2), x_lo, inv_dx, n) == n - 1, "cell index above");

  // periodic wrap never returns the upper bound
  const T tiny_neg = -std::numeric_limits<T>::epsilon() / T(8);
  const T wrapped = wrap_periodic(tiny_neg, T(0), T(1));
  check(wrapped >= T(0) && wrapped < T(1), "wrap_periodic range");
  check(wrap_periodic(T(2.25), T(0), T(1)) == T(0.25), "wrap_periodic value");

  // cell index + offset: whole cells move to the index, the offset stays in [0, 1)
  int cell = 10;
  T offset = T(2.25);
  normalize_cell_offset(cell, offset);
  check(cell == 12 && offset == T(0.25), "normalize_cell_offset up");
  cell = 10;
  offset = T(-0.25);
  normalize_cell_offset(cell, offset);
  check(cell == 9 && offset == T(0.75), "normalize_cell_offset down");
  cell = 10;
  offset = tiny_neg;  // rounds to the cell edge: must not come back as 1
  normalize_cell_offset(cell, offset);
  check(cell == 10 && offset == T(0), "normalize_cell_offset edge");

  // carried running value: 10^5 increments, each below half a unit in the
  // last place of the value, which a plain += loses completely
  const T step = T(0.3) * std::numeric_limits<T>::epsilon();
  T value = T(1), carry = T(0), plain = T(1);
  for (int i = 0; i < 100000; ++i) {
    add_with_carry(value, carry, step);
    plain += step;
  }
  const long double exact = 1.0L + 100000.0L * static_cast<long double>(step);
  const long double eps = static_cast<long double>(std::numeric_limits<T>::epsilon());
  const long double carried_error =
      std::fabs(static_cast<long double>(value) + static_cast<long double>(carry) - exact);
  const long double plain_error = std::fabs(static_cast<long double>(plain) - exact);
  std::printf("add_with_carry (%s), 1e5 steps of 0.3 eps onto 1: plain sum off by "
              "%.3g eps, carried sum by %.3g eps\n",
              precision_name<T>(), static_cast<double>(plain_error / eps),
              static_cast<double>(carried_error / eps));
  check(carried_error <= 2.0L * eps, "add_with_carry");
  check(plain_error > 1000.0L * eps, "add_with_carry: plain sum should stall");
}

void run_integer_cases() {
  using namespace stable_math;
  check(fits_in<std::int32_t>(std::int64_t{2147483647}), "fits_in max");
  check(!fits_in<std::int32_t>(std::int64_t{2147483648}), "fits_in overflow");
  check(!fits_in<std::uint32_t>(std::int64_t{-1}), "fits_in negative");
  check(!fits_in<std::int32_t>(std::uint64_t{4294967295u}), "fits_in sign");
  check(widen_mul(100000, 100000) == std::int64_t{10000000000},
        "widen_mul");
  check(uniform01_f32(std::uint32_t{0xFFFFFFFFu}) < 1.0f, "uniform01_f32 < 1");
  check(uniform01_f64(std::uint64_t{0xFFFFFFFFFFFFFFFFull}) < 1.0,
        "uniform01_f64 < 1");
  check(uniform01_open_low_f32(std::uint32_t{0}) > 0.0f,
        "uniform01_open_low_f32 > 0");
  check(uniform01_open_low_f32(std::uint32_t{0xFFFFFFFFu}) == 1.0f,
        "uniform01_open_low_f32 top");
  check(uniform01_open_low_f64(std::uint64_t{0}) > 0.0,
        "uniform01_open_low_f64 > 0");
}

}  // namespace

int main() {
  if (!truth_covers_double) {
    std::printf("note: long double is not wider than double on this platform; "
                "fp64 rows are printed but not checked\n");
  }
  print_accuracy_header();
  run_unary_cases<float>();
  run_unary_cases<double>();
  run_binary_cases<float>();
  run_binary_cases<double>();
  run_sum_cases<float>();
  run_sum_cases<double>();
  run_discrete_cases<float>();
  run_discrete_cases<double>();
  run_integer_cases();
  if (failures > 0) {
    std::printf("\nSTABLE MATH SELF-TEST FAILED: %d check(s)\n", failures);
    return 1;
  }
  std::printf("\nSTABLE MATH SELF-TEST PASSED\n");
  return 0;
}
