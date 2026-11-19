// SPDX-License-Identifier: Apache-2.0

/// Value-range, cancellation and demotion probes for the reduced-precision
/// porting skill (Phase 1, Analyze).
///
/// The probes answer three questions about a run of the ORIGINAL-precision
/// build, using the real workload:
///
///   PP_RANGE / PP_RANGE_ARRAY  which values flow through this point
///                              (extremes, binade histogram, zeros,
///                              subnormals, integer-valuedness)
///   PP_SUB / PP_ADD            how many leading bits a subtraction cancels,
///                              and how far apart the two operands are in
///                              magnitude (bits of the smaller one absorbed)
///   PP_DEMOTE / PP_DEMOTE_ARRAY how far a lower-precision evaluation of the
///                              same quantity is from the reference one, in
///                              units of the lower precision's epsilon
///   PP_COUNT                   integer magnitudes (for 64 -> 32 bit narrowing)
///
/// Everything compiles away unless PRECISION_PROBE is defined, so the calls
/// can stay in the source during the port. PP_SUB and PP_ADD still evaluate
/// to the plain difference / sum when disabled.
///
/// Host code only. Device kernels (CUDA, OpenACC, ...) cannot call into this
/// header: probe arrays at kernel boundaries, or run the analysis on a CPU
/// build of the same source.
///
/// Output: one JSON object per probe site and line, written at exit (or by
/// precision_probe::report()) to
///     ${PRECISION_PROBE_DIR:-.}/precision_probe.<pid>.jsonl
/// Summarize with scripts/probe-summary.py.
///
/// Environment:
///   PRECISION_PROBE_DIR     output directory (default: current directory)
///   PRECISION_PROBE_STRIDE  record every N-th call of each scalar probe
///                           (default 1). Array probes always record all
///                           elements they are given.

#ifndef PRECISION_PORT_PROBE_HPP
#define PRECISION_PORT_PROBE_HPP

#if defined(PRECISION_PROBE)

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace precision_probe {

namespace detail {

inline long process_id() {
#if defined(_WIN32)
  return static_cast<long>(_getpid());
#else
  return static_cast<long>(getpid());
#endif
}

inline std::uint64_t stride() {
  static const std::uint64_t value = [] {
    const char* raw = std::getenv("PRECISION_PROBE_STRIDE");
    if (raw == nullptr || raw[0] == '\0') return std::uint64_t{1};
    const long long parsed = std::atoll(raw);
    return parsed > 0 ? static_cast<std::uint64_t>(parsed) : std::uint64_t{1};
  }();
  return value;
}

/// JSON has no NaN/Inf literal: print non-finite numbers as null.
inline void print_number(std::FILE* f, const char* key, double v) {
  if (std::isfinite(v)) {
    std::fprintf(f, ",\"%s\":%.17g", key, v);
  } else {
    std::fprintf(f, ",\"%s\":null", key);
  }
}

inline void print_string(std::FILE* f, const char* key, const char* v,
                         bool first = false) {
  std::fprintf(f, "%s\"%s\":\"", first ? "" : ",", key);
  for (const char* c = v; *c != '\0'; ++c) {
    if (*c == '"' || *c == '\\') std::fputc('\\', f);
    std::fputc(static_cast<unsigned char>(*c) < 0x20 ? ' ' : *c, f);
  }
  std::fputc('"', f);
}

inline void print_histogram(std::FILE* f, const char* key,
                            const std::map<int, std::uint64_t>& bins) {
  std::fprintf(f, ",\"%s\":{", key);
  bool first = true;
  for (const auto& kv : bins) {
    std::fprintf(f, "%s\"%d\":%llu", first ? "" : ",", kv.first,
                 static_cast<unsigned long long>(kv.second));
    first = false;
  }
  std::fputc('}', f);
}

template <class T>
constexpr const char* type_label() {
  using U = std::remove_cv_t<std::remove_reference_t<T>>;
  if (std::is_floating_point<U>::value) {
    return sizeof(U) == 4 ? "float32" : sizeof(U) == 8 ? "float64" : "float_ext";
  }
  if (std::is_integral<U>::value) {
    return sizeof(U) == 8 ? (std::is_signed<U>::value ? "int64" : "uint64")
         : sizeof(U) == 4 ? (std::is_signed<U>::value ? "int32" : "uint32")
                          : "int_small";
  }
  return "other";
}

template <class T>
constexpr double min_normal_of() {
  using U = std::remove_cv_t<std::remove_reference_t<T>>;
  return std::is_floating_point<U>::value
             ? static_cast<double>(std::numeric_limits<U>::min())
             : 0.0;
}

template <class T>
constexpr double epsilon_of() {
  using U = std::remove_cv_t<std::remove_reference_t<T>>;
  return static_cast<double>(std::numeric_limits<U>::epsilon());
}

}  // namespace detail

/// Common part of every probe site: identity plus a lock.
class Site {
 public:
  Site(const char* name, const char* file, int line, const char* kind,
       const char* type);
  virtual ~Site() = default;
  Site(const Site&) = delete;
  Site& operator=(const Site&) = delete;

  void write(std::FILE* f) {
    std::lock_guard<std::mutex> lock(mutex_);
    detail::print_string(f, "site", name_.c_str(), true);
    detail::print_string(f, "kind", kind_);
    detail::print_string(f, "type", type_);
    detail::print_string(f, "file", file_);
    std::fprintf(f, ",\"line\":%d", line_);
    write_body(f);
  }

 protected:
  virtual void write_body(std::FILE* f) = 0;

  /// True for the calls that should be recorded under PRECISION_PROBE_STRIDE.
  bool sampled() {
    const std::uint64_t n = calls_.fetch_add(1, std::memory_order_relaxed);
    return n % detail::stride() == 0;
  }

  std::mutex mutex_;

 private:
  std::string name_;
  const char* file_;
  int line_;
  const char* kind_;
  const char* type_;
  std::atomic<std::uint64_t> calls_{0};
};

/// Process-wide list of sites; writes the report once at exit.
class Registry {
 public:
  static Registry& instance() {
    // Intentionally leaked: the exit-time report must not outlive it.
    static Registry* const registry = new Registry;
    return *registry;
  }

  void add(Site* site) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sites_.empty()) std::atexit([] { Registry::instance().report(); });
    sites_.push_back(site);
  }

  /// Write all sites. Safe to call repeatedly: each call rewrites the file
  /// with the statistics gathered so far.
  void report() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sites_.empty()) return;
    const char* dir = std::getenv("PRECISION_PROBE_DIR");
    std::string path = (dir != nullptr && dir[0] != '\0') ? dir : ".";
    path += "/precision_probe." + std::to_string(detail::process_id()) + ".jsonl";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
      std::fprintf(stderr, "precision_probe: cannot write %s\n", path.c_str());
      return;
    }
    for (Site* site : sites_) {
      std::fputc('{', f);
      site->write(f);
      std::fputs("}\n", f);
    }
    std::fclose(f);
  }

 private:
  Registry() = default;
  std::mutex mutex_;
  std::vector<Site*> sites_;
};

inline Site::Site(const char* name, const char* file, int line,
                  const char* kind, const char* type)
    : name_(name), file_(file), line_(line), kind_(kind), type_(type) {
  Registry::instance().add(this);
}

/// Explicit flush, for programs that do not terminate through exit().
inline void report() { Registry::instance().report(); }

/// Value statistics of one program point.
class RangeSite final : public Site {
 public:
  RangeSite(const char* name, const char* file, int line, const char* type,
            double min_normal)
      : Site(name, file, line, "range", type), min_normal_(min_normal) {}

  template <class T>
  void record(T x) {
    if (!sampled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    add(static_cast<double>(x));
  }

  template <class T>
  void record_array(const T* p, std::size_t n) {
    if (p == nullptr || n == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < n; ++i) add(static_cast<double>(p[i]));
  }

 private:
  void add(double x) {
    ++count_;
    if (!std::isfinite(x)) {
      ++n_nonfinite_;
      return;
    }
    if (x < min_) min_ = x;
    if (x > max_) max_ = x;
    if (x == 0.0) {
      ++n_zero_;
      return;
    }
    if (x < 0.0) ++n_negative_;
    const double ax = std::fabs(x);
    if (ax < min_abs_nonzero_) min_abs_nonzero_ = ax;
    if (ax > max_abs_) max_abs_ = ax;
    if (ax < min_normal_) ++n_subnormal_;
    if (x != std::nearbyint(x)) ++n_nonintegral_;
    ++binades_[std::ilogb(ax)];
  }

  void write_body(std::FILE* f) override {
    std::fprintf(f, ",\"count\":%llu,\"n_zero\":%llu,\"n_negative\":%llu"
                    ",\"n_nonfinite\":%llu,\"n_subnormal\":%llu"
                    ",\"n_nonintegral\":%llu",
                 static_cast<unsigned long long>(count_),
                 static_cast<unsigned long long>(n_zero_),
                 static_cast<unsigned long long>(n_negative_),
                 static_cast<unsigned long long>(n_nonfinite_),
                 static_cast<unsigned long long>(n_subnormal_),
                 static_cast<unsigned long long>(n_nonintegral_));
    detail::print_number(f, "min", min_);
    detail::print_number(f, "max", max_);
    detail::print_number(f, "min_abs_nonzero", min_abs_nonzero_);
    detail::print_number(f, "max_abs", max_abs_ > 0.0 ? max_abs_
                             : std::numeric_limits<double>::quiet_NaN());
    detail::print_histogram(f, "binades", binades_);
  }

  double min_normal_;
  std::uint64_t count_ = 0, n_zero_ = 0, n_negative_ = 0, n_nonfinite_ = 0;
  std::uint64_t n_subnormal_ = 0, n_nonintegral_ = 0;
  double min_ = std::numeric_limits<double>::infinity();
  double max_ = -std::numeric_limits<double>::infinity();
  double min_abs_nonzero_ = std::numeric_limits<double>::infinity();
  double max_abs_ = 0.0;
  std::map<int, std::uint64_t> binades_;
};

/// Two hazards of one addition or subtraction, recorded per operation:
///
///   lost bits  leading bits cancelled when the operands nearly cancel. The
///              cancelling operation is itself exact: the count says how far
///              rounding error ALREADY PRESENT in the operands is promoted
///              towards the leading bits of the result.
///   gap bits   exponent distance between the operands. A small term added
///              to a large one keeps only (precision - gap) of its own bits
///              and vanishes entirely once the gap reaches the precision.
///              This is how `t += dt` and `x += v * dt` stall.
///
/// Both only matter relative to the precision of the TARGET type.
class CancelSite final : public Site {
 public:
  CancelSite(const char* name, const char* file, int line, const char* type)
      : Site(name, file, line, "addsub", type) {}

  template <class A, class B>
  auto sub(A a, B b) -> decltype(a - b) {
    const auto d = a - b;
    if (sampled()) add(static_cast<double>(a), static_cast<double>(b),
                       static_cast<double>(d));
    return d;
  }

  template <class A, class B>
  auto add_op(A a, B b) -> decltype(a + b) {
    const auto s = a + b;
    if (sampled()) add(static_cast<double>(a), static_cast<double>(b),
                       static_cast<double>(s));
    return s;
  }

 private:
  void add(double a, double b, double result) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++count_;
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(result)) {
      ++n_nonfinite_;
      return;
    }
    const double big = std::fmax(std::fabs(a), std::fabs(b));
    if (big == 0.0) return;  // 0 - 0: nothing to cancel
    if (a != 0.0 && b != 0.0) {
      const int gap = std::abs(std::ilogb(std::fabs(a)) -
                               std::ilogb(std::fabs(b)));
      ++gap_bits_[gap];
      if (gap > max_gap_) max_gap_ = gap;
    }
    if (result == 0.0) {
      ++n_total_;  // operands equal: every bit cancelled
      return;
    }
    const int lost = std::ilogb(big) - std::ilogb(std::fabs(result));
    const int bin = lost > 0 ? lost : 0;
    ++lost_bits_[bin];
    if (bin > max_lost_) {
      max_lost_ = bin;
      worst_a_ = a;
      worst_b_ = b;
    }
  }

  void write_body(std::FILE* f) override {
    std::fprintf(f, ",\"count\":%llu,\"n_total_cancel\":%llu"
                    ",\"n_nonfinite\":%llu,\"max_lost_bits\":%d"
                    ",\"max_gap_bits\":%d",
                 static_cast<unsigned long long>(count_),
                 static_cast<unsigned long long>(n_total_),
                 static_cast<unsigned long long>(n_nonfinite_), max_lost_,
                 max_gap_);
    detail::print_number(f, "worst_a", worst_a_);
    detail::print_number(f, "worst_b", worst_b_);
    detail::print_histogram(f, "lost_bits", lost_bits_);
    detail::print_histogram(f, "gap_bits", gap_bits_);
  }

  std::uint64_t count_ = 0, n_total_ = 0, n_nonfinite_ = 0;
  int max_lost_ = 0, max_gap_ = 0;
  double worst_a_ = 0.0, worst_b_ = 0.0;
  std::map<int, std::uint64_t> lost_bits_;
  std::map<int, std::uint64_t> gap_bits_;
};

/// Discrepancy between a reference-precision and a lower-precision evaluation
/// of the same quantity, as an amplification factor: relative error divided
/// by the lower precision's epsilon. About 1 means "as good as the format
/// allows"; 2^k means k bits were lost on top of that.
class DemoteSite final : public Site {
 public:
  DemoteSite(const char* name, const char* file, int line, const char* type,
             double eps_lo)
      : Site(name, file, line, "demote", type), eps_lo_(eps_lo) {}

  template <class R, class L>
  void record(R ref, L lo) {
    if (!sampled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    add(static_cast<double>(ref), static_cast<double>(lo));
  }

  template <class R, class L>
  void record_array(const R* ref, const L* lo, std::size_t n) {
    if (ref == nullptr || lo == nullptr || n == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < n; ++i) {
      add(static_cast<double>(ref[i]), static_cast<double>(lo[i]));
    }
  }

 private:
  void add(double ref, double lo) {
    ++count_;
    if (!std::isfinite(ref) || !std::isfinite(lo)) {
      // A reference that is finite while the low-precision value is not is
      // an overflow (or invalid operation) introduced by the demotion.
      if (std::isfinite(ref)) ++n_lo_nonfinite_;
      else ++n_ref_nonfinite_;
      return;
    }
    if (ref == 0.0) {
      if (lo != 0.0) ++n_ref_zero_mismatch_;
      else ++amplification_[0];
      return;
    }
    const double amp = std::fabs(lo - ref) / std::fabs(ref) / eps_lo_;
    sum_amp_ += amp;
    ++n_amp_;
    // bin 0: below 1 eps; bin k: [2^(k-1), 2^k) eps
    const int bin = amp < 1.0 ? 0 : std::ilogb(amp) + 1;
    ++amplification_[bin];
    if (amp > max_amp_) {
      max_amp_ = amp;
      worst_ref_ = ref;
      worst_lo_ = lo;
    }
  }

  void write_body(std::FILE* f) override {
    std::fprintf(f, ",\"count\":%llu,\"n_lo_nonfinite\":%llu"
                    ",\"n_ref_nonfinite\":%llu,\"n_ref_zero_mismatch\":%llu",
                 static_cast<unsigned long long>(count_),
                 static_cast<unsigned long long>(n_lo_nonfinite_),
                 static_cast<unsigned long long>(n_ref_nonfinite_),
                 static_cast<unsigned long long>(n_ref_zero_mismatch_));
    detail::print_number(f, "eps_lo", eps_lo_);
    detail::print_number(f, "max_amplification", max_amp_);
    detail::print_number(f, "mean_amplification",
                         n_amp_ > 0 ? sum_amp_ / static_cast<double>(n_amp_)
                                    : 0.0);
    detail::print_number(f, "worst_ref", worst_ref_);
    detail::print_number(f, "worst_lo", worst_lo_);
    detail::print_histogram(f, "log2_amplification", amplification_);
  }

  double eps_lo_;
  std::uint64_t count_ = 0, n_amp_ = 0;
  std::uint64_t n_lo_nonfinite_ = 0, n_ref_nonfinite_ = 0;
  std::uint64_t n_ref_zero_mismatch_ = 0;
  double sum_amp_ = 0.0, max_amp_ = 0.0, worst_ref_ = 0.0, worst_lo_ = 0.0;
  std::map<int, std::uint64_t> amplification_;
};

/// Integer magnitudes, for deciding whether a 64-bit quantity fits in 32 bits.
class CountSite final : public Site {
 public:
  CountSite(const char* name, const char* file, int line, const char* type)
      : Site(name, file, line, "count", type) {}

  template <class I>
  void record(I v) {
    if (!sampled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ++count_;
    // Unsigned values above INT64_MAX are tracked separately so that the
    // signed min/max stay meaningful.
    if (!std::is_signed<I>::value &&
        static_cast<unsigned long long>(v) >
            static_cast<unsigned long long>(
                std::numeric_limits<long long>::max())) {
      ++n_above_int64_;
      return;
    }
    const long long x = static_cast<long long>(v);
    if (x < min_) min_ = x;
    if (x > max_) max_ = x;
    if (x < 0) ++n_negative_;
  }

 private:
  void write_body(std::FILE* f) override {
    std::fprintf(f, ",\"count\":%llu,\"n_negative\":%llu"
                    ",\"n_above_int64\":%llu",
                 static_cast<unsigned long long>(count_),
                 static_cast<unsigned long long>(n_negative_),
                 static_cast<unsigned long long>(n_above_int64_));
    if (min_ <= max_) {
      std::fprintf(f, ",\"min\":%lld,\"max\":%lld", min_, max_);
    } else {
      std::fprintf(f, ",\"min\":null,\"max\":null");
    }
  }

  std::uint64_t count_ = 0, n_negative_ = 0, n_above_int64_ = 0;
  long long min_ = std::numeric_limits<long long>::max();
  long long max_ = std::numeric_limits<long long>::min();
};

}  // namespace precision_probe

#define PP_DETAIL_CONCAT2(a, b) a##b
#define PP_DETAIL_CONCAT(a, b) PP_DETAIL_CONCAT2(a, b)
#define PP_DETAIL_SITE PP_DETAIL_CONCAT(pp_site_, __LINE__)

/// Record one scalar value.
#define PP_RANGE(name, x)                                                     \
  do {                                                                        \
    static ::precision_probe::RangeSite& PP_DETAIL_SITE =                     \
        *new ::precision_probe::RangeSite(                                    \
        name, __FILE__, __LINE__,                                             \
        ::precision_probe::detail::type_label<decltype(x)>(),                 \
        ::precision_probe::detail::min_normal_of<decltype(x)>());             \
    PP_DETAIL_SITE.record(x);                                                 \
  } while (0)

/// Record n contiguous values starting at ptr (one lock for the whole array).
#define PP_RANGE_ARRAY(name, ptr, n)                                          \
  do {                                                                        \
    static ::precision_probe::RangeSite& PP_DETAIL_SITE =                     \
        *new ::precision_probe::RangeSite(                                    \
        name, __FILE__, __LINE__,                                             \
        ::precision_probe::detail::type_label<decltype(*(ptr))>(),            \
        ::precision_probe::detail::min_normal_of<decltype(*(ptr))>());        \
    PP_DETAIL_SITE.record_array((ptr), static_cast<std::size_t>(n));          \
  } while (0)

/// Evaluates to (a) - (b) and records cancellation and operand gap.
#define PP_SUB(name, a, b)                                                    \
  ([&]() {                                                                    \
    static ::precision_probe::CancelSite& pp_site =                           \
        *new ::precision_probe::CancelSite(                                   \
        name, __FILE__, __LINE__,                                             \
        ::precision_probe::detail::type_label<decltype((a) - (b))>());        \
    return pp_site.sub((a), (b));                                             \
  }())

/// Evaluates to (a) + (b) and records cancellation and operand gap.
#define PP_ADD(name, a, b)                                                    \
  ([&]() {                                                                    \
    static ::precision_probe::CancelSite& pp_site =                           \
        *new ::precision_probe::CancelSite(                                   \
        name, __FILE__, __LINE__,                                             \
        ::precision_probe::detail::type_label<decltype((a) + (b))>());        \
    return pp_site.add_op((a), (b));                                          \
  }())

/// Record a reference-precision value next to its lower-precision evaluation.
/// The amplification is measured against the epsilon of the type of `lo`.
#define PP_DEMOTE(name, ref, lo)                                              \
  do {                                                                        \
    static ::precision_probe::DemoteSite& PP_DETAIL_SITE =                    \
        *new ::precision_probe::DemoteSite(                                   \
        name, __FILE__, __LINE__,                                             \
        ::precision_probe::detail::type_label<decltype(lo)>(),                \
        ::precision_probe::detail::epsilon_of<decltype(lo)>());               \
    PP_DETAIL_SITE.record((ref), (lo));                                       \
  } while (0)

/// Array form of PP_DEMOTE over n elements.
#define PP_DEMOTE_ARRAY(name, ref_ptr, lo_ptr, n)                             \
  do {                                                                        \
    static ::precision_probe::DemoteSite& PP_DETAIL_SITE =                    \
        *new ::precision_probe::DemoteSite(                                   \
        name, __FILE__, __LINE__,                                             \
        ::precision_probe::detail::type_label<decltype(*(lo_ptr))>(),         \
        ::precision_probe::detail::epsilon_of<decltype(*(lo_ptr))>());        \
    PP_DETAIL_SITE.record_array((ref_ptr), (lo_ptr),                          \
                                static_cast<std::size_t>(n));                 \
  } while (0)

/// Record an integer magnitude (sizes, indices, ids, counters).
#define PP_COUNT(name, n)                                                     \
  do {                                                                        \
    static ::precision_probe::CountSite& PP_DETAIL_SITE =                     \
        *new ::precision_probe::CountSite(                                    \
        name, __FILE__, __LINE__,                                             \
        ::precision_probe::detail::type_label<decltype(n)>());                \
    PP_DETAIL_SITE.record(n);                                                 \
  } while (0)

#else  // !PRECISION_PROBE: every probe compiles away

// The arguments are referenced inside sizeof, which does not evaluate them,
// so variables that only feed a probe do not become "unused".
#define PP_RANGE(name, x) ((void)sizeof(x))
#define PP_RANGE_ARRAY(name, ptr, n) ((void)sizeof(ptr), (void)sizeof(n))
#define PP_SUB(name, a, b) ((a) - (b))
#define PP_ADD(name, a, b) ((a) + (b))
#define PP_DEMOTE(name, ref, lo) ((void)sizeof(ref), (void)sizeof(lo))
#define PP_DEMOTE_ARRAY(name, ref_ptr, lo_ptr, n)                             \
  ((void)sizeof(ref_ptr), (void)sizeof(lo_ptr), (void)sizeof(n))
#define PP_COUNT(name, n) ((void)sizeof(n))

#endif  // PRECISION_PROBE

#endif  // PRECISION_PORT_PROBE_HPP
