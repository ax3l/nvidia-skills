// SPDX-License-Identifier: Apache-2.0

/// Comparison harness for reduced-precision porting tests.
///
/// One test source is built twice: at the original precision ("hi") and at
/// the target precision ("lo"). It calls the candidate at the build's working
/// precision and compares against a reference held at the ORIGINAL precision.
/// This header picks the right gate from the two types involved:
///
///   candidate as precise as the reference  ->  "same precision" gate: results
///       must not have changed beyond roundoff
///   candidate narrower than the reference  ->  "narrower" gate: the error must
///       stay within a small multiple of the candidate type's epsilon
/// The reference may be the frozen original (double) or a truth evaluated at
/// higher precision (long double); against a truth, both builds get the
/// "narrower" gate, in units of their own epsilon.
///
/// Three calls cover the common case:
///
///   auto budget = precision_port::default_budget<Real, double>();
///   auto result = precision_port::compare_field(cand, ref, n, budget);
///   if (!precision_port::report_and_gate(result, budget, "kernel")) return 1;
///
/// default_budget() is the FUNCTION-LEVEL gate: every element is judged on
/// its own relative error, so a sweep over many decades of inputs cannot
/// hide a wrong small result behind a large one. For whole fields of an
/// application run, where cells far below the peak are legitimately noisy,
/// build the gate from the user's accuracy budget with field_budget().
/// For quantities whose zero is arbitrary (positions, times, phases, angles),
/// a relative error means nothing: gate them with absolute_budget(), in a
/// natural unit of the quantity.
///
/// Pass the values in the type whose accuracy is being judged. A double
/// result converted to long double before the call is judged, and counted in
/// eps, as a long double.
///
/// A second group (error_stats / print_accuracy_row / no_worse_than) measures
/// accuracy against a higher-precision truth, for the before/after tables a
/// reformulation is judged by.
///
/// Environment overrides (absolute values replace the type-derived defaults):
///   PP_WMAD_TOL       gate on the normalized weighted mean absolute difference
///   PP_REL_TOL        element-wise relative tolerance
///   PP_ABS_TOL        element-wise absolute tolerance
///   PP_REL_FLOOR      magnitude floor, as a fraction of the reference peak
///   PP_OUTLIER_CHECK  on (default) / off: gate on the outlier count
///   PP_LO_WMAD_EPS    "narrower" gate only: wmad limit in units of eps (default 4)
///   PP_LO_AMP_MAX     "narrower" gate only: element limit in units of eps (default 16)
///
/// See references/verification-guide.md for how to read and review results.

#ifndef PRECISION_PORT_COMPARE_HPP
#define PRECISION_PORT_COMPARE_HPP

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace precision_port {

/// Gate thresholds for one comparison.
struct Budget {
  double wmad_tol = 0.0;      ///< limit on wmad; this is the gate
  double rel_tol = 0.0;       ///< element passes if rel <= rel_tol ...
  double abs_tol = 0.0;       ///< ... OR abs <= abs_tol
  double rel_floor = 0.0;     ///< floor on |ref|, as a fraction of the peak
  bool outlier_check = true;  ///< gate on n_exceed > 0 (reviewable)
  bool narrower = false;       ///< candidate is narrower than the reference
  bool absolute = false;      ///< absolute gate: only abs_tol decides
  double cand_eps = 0.0;      ///< epsilon of the candidate type
};

/// Outcome of one field comparison. `wmad` gates; the rest is diagnosis.
struct CompareResult {
  double wmad = 0.0;
  double max_rel = 0.0;
  double max_abs = 0.0;      ///< largest |candidate - reference|
  long long n_exceed = 0;
  long long n_total = 0;
  std::size_t worst = 0;     ///< flat index, 0-based
  double peak = 0.0;         ///< max |reference|
  bool nonfinite = false;
  bool empty_field = false;
};

namespace detail {

inline double env_real(const char* name, double fallback) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') return fallback;
  char* end = nullptr;
  const double parsed = std::strtod(raw, &end);
  if (end == raw) return fallback;
  // Require the whole value: "1e-3oops" must not silently become 1e-3.
  while (std::isspace(static_cast<unsigned char>(*end))) ++end;
  return (*end == '\0') ? parsed : fallback;
}

inline bool env_flag(const char* name, bool fallback) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') return fallback;
  std::string v(raw);
  for (char& ch : v) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (v == "0" || v == "off" || v == "false" || v == "no") return false;
  if (v == "1" || v == "on" || v == "true" || v == "yes") return true;
  return fallback;
}

}  // namespace detail

/// Function-level gate for comparing `Cand` results against a `Ref`
/// reference, element by element.
///
/// same precision (Cand at least as precise as Ref):
///     double reference: wmad 1e-12, rel 1e-10
///     float  reference: wmad 1e-6,  rel 1e-4
/// narrower (Cand narrower than Ref), with eps = epsilon of Cand:
///     wmad 4 eps, rel 16 eps
/// both: no magnitude floor; abs tolerance = smallest normal number of Cand,
///     so only differences below the type's own resolution near zero are
///     forgiven.
///
/// The narrower-gate defaults are deliberately tight: they describe a well-conditioned,
/// numerically stable kernel. A kernel whose problem is ill-conditioned needs
/// a looser, documented budget (see the verification guide), not a silent one.
template <class Cand, class Ref>
inline Budget default_budget() {
  Budget b;
  b.cand_eps = static_cast<double>(std::numeric_limits<Cand>::epsilon());
  b.narrower = std::numeric_limits<Cand>::digits < std::numeric_limits<Ref>::digits;
  b.abs_tol = static_cast<double>(std::numeric_limits<Cand>::min());
  b.rel_floor = 0.0;
  if (b.narrower) {
    b.wmad_tol = detail::env_real("PP_LO_WMAD_EPS", 4.0) * b.cand_eps;
    b.rel_tol = detail::env_real("PP_LO_AMP_MAX", 16.0) * b.cand_eps;
  } else if (std::numeric_limits<Ref>::digits >= 53) {
    b.wmad_tol = 1e-12;
    b.rel_tol = 1e-10;
  } else {
    b.wmad_tol = 1e-6;
    b.rel_tol = 1e-4;
  }
  b.wmad_tol = detail::env_real("PP_WMAD_TOL", b.wmad_tol);
  b.rel_tol = detail::env_real("PP_REL_TOL", b.rel_tol);
  b.abs_tol = detail::env_real("PP_ABS_TOL", b.abs_tol);
  b.rel_floor = detail::env_real("PP_REL_FLOOR", b.rel_floor);
  b.outlier_check = detail::env_flag("PP_OUTLIER_CHECK", b.outlier_check);
  return b;
}

/// Field-level gate built from an explicit accuracy budget, for comparing a
/// whole output field of an application run (a quantity of interest) against
/// its reference. `rel_floor` is the fraction of the field's peak below which
/// cells are compared against the floor instead of their own magnitude.
/// The three numbers come from the user's budget for that quantity; they are
/// arguments on purpose, so that no hidden default decides what is accurate
/// enough. The PP_* environment overrides still apply.
template <class Cand, class Ref>
inline Budget field_budget(double wmad_tol, double rel_tol, double rel_floor) {
  Budget b;
  b.cand_eps = static_cast<double>(std::numeric_limits<Cand>::epsilon());
  b.narrower = std::numeric_limits<Cand>::digits < std::numeric_limits<Ref>::digits;
  b.wmad_tol = detail::env_real("PP_WMAD_TOL", wmad_tol);
  b.rel_tol = detail::env_real("PP_REL_TOL", rel_tol);
  b.abs_tol = detail::env_real("PP_ABS_TOL", 0.0);
  b.rel_floor = detail::env_real("PP_REL_FLOOR", rel_floor);
  b.outlier_check = detail::env_flag("PP_OUTLIER_CHECK", true);
  return b;
}

/// Absolute gate, for quantities whose zero is arbitrary: positions, times,
/// phases, angles, offsets from a base value. Every element must lie within
/// `abs_tol` of its reference, in the quantity's own unit; wmad is reported
/// and not gated. Choose the tolerance in a natural unit (a fraction of a
/// cell, of the displacement over the test, of a period) and name that unit
/// where the test sets it. Of the environment overrides only PP_ABS_TOL
/// applies: this gate has one criterion and it cannot be switched off.
template <class Cand, class Ref>
inline Budget absolute_budget(double abs_tol) {
  Budget b;
  b.cand_eps = static_cast<double>(std::numeric_limits<Cand>::epsilon());
  b.narrower = std::numeric_limits<Cand>::digits < std::numeric_limits<Ref>::digits;
  b.absolute = true;
  b.wmad_tol = std::numeric_limits<double>::infinity();
  b.rel_tol = 0.0;
  b.abs_tol = detail::env_real("PP_ABS_TOL", abs_tol);
  b.rel_floor = 0.0;
  b.outlier_check = true;
  return b;
}

/// Compare `a` against reference `ref` over `n` elements. `w` carries the
/// physical weight of each element (cell volume or area on a non-uniform
/// mesh) and may be nullptr, meaning 1. All accumulation is in double.
template <class Cand, class Ref, class W = double>
inline CompareResult compare_field(const Cand* a, const Ref* ref,
                                   std::size_t n, const Budget& budget,
                                   const W* w = nullptr) {
  CompareResult c;
  c.n_total = static_cast<long long>(n);
  if (n == 0) {
    c.empty_field = true;
    return c;
  }

  // Non-finite anywhere is an unconditional failure — never waivable.
  for (std::size_t i = 0; i < n; ++i) {
    const double ai = static_cast<double>(a[i]);
    const double ri = static_cast<double>(ref[i]);
    if (!std::isfinite(ai) || !std::isfinite(ri)) {
      c.nonfinite = true;
      c.worst = i;
      c.wmad = std::numeric_limits<double>::infinity();
      c.max_rel = std::numeric_limits<double>::infinity();
      c.n_exceed = -1;
      return c;
    }
    c.peak = std::fmax(c.peak, std::fabs(ri));
  }

  // Optional magnitude floor (field-level budgets): cells far below the
  // field peak cannot manufacture a huge relative error out of numerical dust.
  const double floor_v = budget.rel_floor * c.peak;
  double num = 0.0;
  double den = 0.0;
  std::size_t worst_abs = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double wi = (w != nullptr) ? static_cast<double>(w[i]) : 1.0;
    const double ai = static_cast<double>(a[i]);
    const double ri = static_cast<double>(ref[i]);
    const double ad = std::fabs(ai - ri);
    num += wi * ad;
    den += wi * std::fabs(ri);
    const double scale = std::fabs(ri) + floor_v;
    // A zero reference with no floor: only an absolute criterion can apply.
    const double rd =
        scale > 0.0 ? ad / scale : (ad > budget.abs_tol ? HUGE_VAL : 0.0);
    if (rd > c.max_rel) {
      c.max_rel = rd;
      c.worst = i;
    }
    if (ad > c.max_abs) {
      c.max_abs = ad;
      worst_abs = i;
    }
    // An element passes if EITHER tolerance is met.
    if (ad > budget.abs_tol && rd > budget.rel_tol) ++c.n_exceed;
  }
  c.wmad = (den > 0.0) ? num / den : num;
  if (budget.absolute) c.worst = worst_abs;
  return c;
}

/// Print the required numbers and apply the gate. Returns true on pass.
inline bool report_and_gate(const CompareResult& c, const Budget& budget,
                            const char* label) {
  const char* mode = budget.absolute ? "absolute"
                     : (budget.narrower ? "narrower" : "same precision");
  if (c.empty_field) {
    std::printf("EMPTY FIELD: zero elements compared, so the result would "
                "prove nothing\n");
    std::printf("VERIFICATION FAILED (empty field, %s): %s\n", mode, label);
    return false;
  }
  if (c.nonfinite) {
    std::printf("NON-FINITE VALUES in %s at flat index %zu\n", label, c.worst);
    std::printf("VERIFICATION FAILED (non-finite, %s): %s\n", mode, label);
    return false;
  }

  if (budget.absolute) {
    std::printf("compare %s [absolute]: max_abs %.5e  (abs_tol %.3e, %.3g of it)\n",
                label, c.max_abs, budget.abs_tol,
                budget.abs_tol > 0.0 ? c.max_abs / budget.abs_tol : HUGE_VAL);
    std::printf("  n_exceed %lld of %lld  worst_idx %zu  (wmad %.3e, not gated)\n",
                c.n_exceed, c.n_total, c.worst, c.wmad);
    if (c.n_exceed > 0) {
      std::printf("VERIFICATION FAILED (absolute): %s\n", label);
      return false;
    }
    std::printf("VERIFICATION PASSED (absolute): %s\n", label);
    return true;
  }
  std::printf("compare %s [%s]: wmad %.5e  max_rel %.5e\n", label, mode,
              c.wmad, c.max_rel);
  if (budget.cand_eps > 0.0) {
    std::printf("  in eps of the candidate type: wmad %.3g  max_rel %.3g\n",
                c.wmad / budget.cand_eps, c.max_rel / budget.cand_eps);
  }
  std::printf("  n_exceed %lld of %lld  worst_idx %zu  (wmad_tol %.3e, "
              "rel_tol %.3e, abs_tol %.3e, rel_floor %.3e)\n",
              c.n_exceed, c.n_total, c.worst, budget.wmad_tol, budget.rel_tol,
              budget.abs_tol, budget.rel_floor);

  if (c.wmad > budget.wmad_tol) {
    std::printf("VERIFICATION FAILED (wmad, %s): %s\n", mode, label);
    return false;
  }
  // The outlier bound IS reviewable — see verification-guide.md,
  // "Reviewing an outlier failure".
  if (budget.outlier_check && c.n_exceed > 0) {
    std::printf("VERIFICATION FAILED (outliers, %s) - review before waiving: "
                "%s\n", mode, label);
    return false;
  }
  std::printf("VERIFICATION PASSED (%s): %s\n", mode, label);
  return true;
}

/// Accuracy of a candidate against a higher-precision truth, expressed in
/// units of the candidate type's epsilon so that the "hi" and "lo" builds of
/// one function can be read side by side.
struct ErrorStats {
  double max_eps = 0.0;    ///< worst relative error / eps
  double p99_eps = 0.0;    ///< 99th percentile
  double mean_eps = 0.0;
  double max_rel = 0.0;    ///< worst relative error (absolute scale)
  double eps = 0.0;        ///< epsilon of the candidate type
  std::size_t worst = 0;   ///< flat index of the worst element
  std::size_t n = 0;
  std::size_t n_nonfinite = 0;  ///< candidate not finite where truth is
};

/// Relative error is |a - t| / max(|t|, floor), with the floor at the
/// candidate type's smallest normal number: an exact zero in the truth does
/// not turn a subnormal-sized error into an infinite one.
template <class Cand, class Truth>
inline ErrorStats error_stats(const Cand* a, const Truth* truth,
                              std::size_t n) {
  ErrorStats s;
  s.n = n;
  s.eps = static_cast<double>(std::numeric_limits<Cand>::epsilon());
  if (n == 0) return s;
  const long double floor_v =
      static_cast<long double>(std::numeric_limits<Cand>::min());
  std::vector<double> rel;
  rel.reserve(n);
  long double sum = 0.0L;
  for (std::size_t i = 0; i < n; ++i) {
    const long double ti = static_cast<long double>(truth[i]);
    const long double ai = static_cast<long double>(a[i]);
    if (!std::isfinite(static_cast<double>(a[i]))) {
      if (std::isfinite(ti)) ++s.n_nonfinite;
      continue;
    }
    if (!std::isfinite(ti)) continue;
    const long double scale = std::fmax(std::fabs(ti), floor_v);
    const double r = static_cast<double>(std::fabs(ai - ti) / scale);
    if (r > s.max_rel) {
      s.max_rel = r;
      s.worst = i;
    }
    sum += static_cast<long double>(r);
    rel.push_back(r);
  }
  if (rel.empty()) return s;
  const double count = static_cast<double>(rel.size());
  const std::size_t k =
      std::min(rel.size() - 1, static_cast<std::size_t>(0.99 * count));
  std::nth_element(rel.begin(), rel.begin() + static_cast<std::ptrdiff_t>(k),
                   rel.end());
  s.max_eps = s.max_rel / s.eps;
  s.p99_eps = rel[k] / s.eps;
  s.mean_eps = static_cast<double>(sum) / count / s.eps;
  return s;
}

/// Header of the before/after accuracy table (Markdown, pasteable).
inline void print_accuracy_header() {
  std::printf("| function | variant | precision | max err [eps] | "
              "p99 [eps] | mean [eps] | worst digits | non-finite |\n");
  std::printf("|---|---|---|---:|---:|---:|---:|---:|\n");
}

/// One row: `variant` is e.g. "original" or "reformulated", `precision` the
/// build's working precision ("fp64", "fp32", ...).
inline void print_accuracy_row(const char* function, const char* variant,
                               const char* precision, const ErrorStats& s) {
  const double digits =
      s.max_rel > 0.0 ? -std::log10(s.max_rel)
                      : static_cast<double>(std::numeric_limits<double>::digits10);
  std::printf("| %s | %s | %s | %.3g | %.3g | %.3g | %.1f | %zu |\n", function,
              variant, precision, s.max_eps, s.p99_eps, s.mean_eps,
              digits > 0.0 ? digits : 0.0, s.n_nonfinite);
}

/// Adjudication when a reformulation changes results at the ORIGINAL
/// precision: it is acceptable only if it is no less accurate than the
/// original against the truth. `slack_eps` allows that many eps of ordinary
/// rounding noise.
inline bool no_worse_than(const ErrorStats& candidate,
                          const ErrorStats& original, double slack_eps = 2.0) {
  return candidate.n_nonfinite <= original.n_nonfinite &&
         candidate.max_eps <= original.max_eps + slack_eps;
}

}  // namespace precision_port

#endif  // PRECISION_PORT_COMPARE_HPP
