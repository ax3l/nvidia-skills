# Precision in Fortran

Language mechanics for every phase when `config.json:language` is
`fortran`. The rules behind them are in the
[maintainability guide](../../references/maintainability-guide.md).

- [Kind Parameters as Roles](#kind-parameters-as-roles)
- [Literals and Constants](#literals-and-constants)
- [Intrinsics](#intrinsics)
- [Limits and Tolerances](#limits-and-tolerances)
- [Boundaries](#boundaries)
- [Compiler Diagnostics and Flags](#compiler-diagnostics-and-flags)
- [Build Wiring](#build-wiring)
- [Probes](#probes)
- [Standalone Test](#standalone-test)
- [Offloaded Code: OpenACC and OpenMP](#offloaded-code-openacc-and-openmp)

## Kind Parameters as Roles

One module defines every role as a kind parameter. It is the only place
that names a concrete kind for role data:

```fortran
!> The only place that names concrete real kinds.
module precision_kinds
   use, intrinsic :: iso_fortran_env, only: real32, real64, int32, int64
   implicit none
   private
   public :: wp, hp, ip, lp
#ifdef APP_PRECISION_SINGLE
   integer, parameter :: wp = real32   !< working precision: field and particle data
#else
   integer, parameter :: wp = real64
#endif
   integer, parameter :: hp = real64   !< time and global diagnostics, wide in every profile
   integer, parameter :: ip = int32    !< local indices and counts
   integer, parameter :: lp = int64    !< global sizes and offsets
end module precision_kinds
```

- If the project already has such a module (`wp`, `rk`, `dp` used as the
  working kind), use it. A kind named `dp` that is switched to single is
  confusing but legal; renaming it is a separate, mechanical commit the user
  may or may not want.
- The module is the one file that needs the preprocessor (`.F90`, or the
  compiler's preprocessing flag). Kernels never test the macro.
- Declarations become `real(wp)`, `complex(wp)`, `integer(ip)`.
  `double precision`, `real*8`, `real(8)` and `implicit real*8 (a-h,o-z)`
  fix the precision and are replaced. Kind *numbers* (`real(8)`) are not
  portable across compilers in any case.

## Literals and Constants

Fortran's default real literal is **single** precision, the opposite of C:

| Literal | Kind | Consequence |
|---------|------|-------------|
| `0.1` | default real (single) | In a double-precision expression it carries only single accuracy: `x = 0.1` stores 0.100000001490116 in a `real(real64)` variable. An existing accuracy bug in many codes, found by this port. |
| `0.1d0` | double | Fixes the precision: in a `wp = real32` build it promotes the expression to double. |
| `0.1_wp` | working | What the port uses. |

| Write | Not |
|-------|-----|
| `0.5_wp*x`, `1.0_wp`, `1.0e-3_wp` | `0.5*x`, `0.5d0*x`, `1.0d-3` |
| `real(i, wp)` | `dble(i)`, `float(i)`, `real(i)` |
| `real(x, hp)` at a boundary | `dble(x)` |
| `x*x`, `x**2` (integer exponent) | `x**2.0` or `x**2.0d0` |
| `acos(-1.0_wp)` or a `real(wp), parameter` constant | `3.14159` (single) or `3.14159265358979d0` (fixed double) |

Exactly representable literals (`1.0`, `0.5`, `2.0`) are harmless as default
reals by value, and still worth suffixing so that the reader and the
conversion warnings do not have to tell them apart from `0.1`.

Constants whose *products* leave the range of the target kind are combined
once at the wide kind and stored at the working kind:

```fortran
! q_e**2/(m_e*eps0) in SI units: m_e*eps0 is about 8e-42, below the normal
! range of real32. Combined at the wide kind; the result, about 3.2e3, is an
! ordinary real(wp).
real(wp), parameter :: plasma_coefficient = &
   real(1.602176634e-19_hp**2/(9.1093837015e-31_hp*8.8541878128e-12_hp), wp)
```

`DATA` statements, array constructors and namelist defaults follow the same
rule: give every element the kind of its target.

## Intrinsics

Use the generic names; they follow the argument's kind.

| Write | Not |
|-------|-----|
| `sqrt`, `exp`, `log`, `sin`, `abs`, `max`, `min`, `mod`, `sign`, `atan2` | `dsqrt`, `dexp`, `dlog`, `dsin`, `dabs`, `dmax1`, `dmin1`, `dmod`, `dsign`, `datan2`, `alog`, `amax1` |
| `real(x, wp)`, `cmplx(a, b, wp)` | `dble(x)`, `sngl(x)`, `dcmplx(a, b)`, and `cmplx(a, b)` without a kind (default complex: single) |
| `int(x)`, `nint(x)` with a range check where it matters | `idint`, `idnint` |
| `hypot(x, y)`, `norm2(v)` | `sqrt(x**2 + y**2)` when overflow is possible |

`max(x, 1.0d-30)` and `sign(1.0d0, x)` with `x` of kind `wp = real32` are
kind mismatches: an error with most compilers, a silent promotion with
extensions enabled. Give every argument the same kind.

Fortran has no `log1p` / `expm1` intrinsic; the helper module provides
`sm_log1p` and `sm_expm1`.

## Limits and Tolerances

```fortran
epsilon(1.0_wp)    ! 2**-23 (real32), 2**-52 (real64)
tiny(1.0_wp)       ! smallest NORMAL positive value
huge(1.0_wp)
digits(1.0_wp)     ! significand bits: 24, 53
precision(1.0_wp), range(1.0_wp)   ! decimal digits and exponent range
```

Replace numerical tolerances written as literals (`1.0d-12`) and sentinels
(`1.0d300`, `1.0d-300`) by expressions of these. Note the argument: the
inventory's `epsilon_query` hits such as `epsilon(1.0d0)` and `tiny(1.d0)`
name the double kind explicitly and do not follow `wp`.

`selected_real_kind(p, r)` requests a kind by decimal precision and exponent
range; a result of `-1` or below means the compiler has none. It is the
portable way to ask for half or quad precision where they exist.

## Boundaries

- **MPI**: pair each kind with its datatype in one place
  (`mpi_wp = MPI_REAL` or `MPI_DOUBLE_PRECISION`, selected next to `wp`), and
  use that name at every call. `MPI_REAL8` and friends are fixed sizes.
- **Unformatted and direct-access I/O**: record lengths (`recl=`), and the
  bytes on disk, follow the kind of the I/O list. To keep the on-disk
  precision (`io_policy: "keep"`), write `real(x, hp)` and read into an `hp`
  temporary. Formatted I/O with fixed widths (`ES23.15`) keeps working but
  prints digits the working kind does not have.
- **NetCDF, HDF5 and similar**: the external type is a separate argument
  (`nf90_double`); decide per `io_policy`.
- **Libraries**: BLAS/LAPACK names encode the precision (`dgemm`/`sgemm`).
  Wrap each used routine in a generic interface that dispatches on the kind,
  in one module.
- **C interoperability**: `bind(C)` interfaces declare `c_double` or
  `c_float`. Either both sides follow the working kind (`c_wp` selected next
  to `wp`), or the interface stays fixed and the caller converts.
- **`transfer`, `equivalence`, storage association through `common` blocks
  and argument passing by sequence association** depend on the size of the
  type. Each is a deliberate rewrite, not a kind change.
- **Implicit interfaces** (external procedures without a module or interface
  block) are not checked by the compiler: a caller passing `real(wp)` to a
  routine expecting `double precision` compiles and computes garbage. Put
  procedures in modules before changing kinds, or the port cannot be
  verified by building.

## Compiler Diagnostics and Flags

| Compiler | Flags | Finds / does |
|----------|-------|--------------|
| gfortran | `-Wconversion -Wconversion-extra` | implicit conversions between kinds and types |
| gfortran | `-Wcompare-reals` (in `-Wextra`) | `==` / `/=` on reals |
| gfortran | `-Wreal-q-constant`, `-std=f2018` | non-standard literals and extensions that hide kind problems |
| gfortran | `-fimplicit-none` | undeclared variables whose kind comes from implicit typing |
| gfortran | `-freal-8-real-4` | **experiment only**: demotes every `real(8)` entity, including `double precision` and `d0` literals, to `real(4)` |
| any | the project compiler's own conversion and interface-checking warnings | consult its documentation; names differ |

The kind-demotion flag gives a quick look at an all-single build of a
self-contained code (Phase 1, method S4). It is not the port: library
interfaces, MPI datatypes and I/O records do not follow it, and the source
still says `double`. Other compilers have similar options under different
names; check the compiler's documentation instead of assuming one.

Flags that promote kinds in the other direction (`-fdefault-real-8`, `-r8`
and equivalents) are sometimes how a code became double precision in the
first place. Then the source is written in default reals, the "port" to
single is the removal of the flag, and every literal and library call needs
the same review as above. The inventory lists such flags under build flags.

Value-unsafe optimization (`-ffast-math`, `-Ofast`, and vendor options that
relax floating-point semantics) removes the compensation in the helper
module. `assets/stable-math/test_stable_math.F90` fails when built with such
flags: run it with the project's flags.

## Build Wiring

```makefile
PRECISION_PROFILE ?= hi
ifeq ($(PRECISION_PROFILE),lo)
  FPPFLAGS += -DAPP_PRECISION_SINGLE
endif
BUILD := $(PRECISION_PORT_BUILD_DIR)/$(PRECISION_PROFILE)
# one object AND module directory per profile: a stale .mod from the other
# precision is the classic way to link a half-single, half-double program
```

Module files encode kinds. Never share a module or object directory between
profiles, and rebuild everything that uses `precision_kinds` when the
profile changes (separate trees make that automatic).

## Probes

Compile `assets/probe/precision_probe.f90` into the analysis build and guard
the calls, so that production builds neither call nor need the module:

```fortran
#ifdef PRECISION_PROBE
   use precision_probe
#endif
   ...
#ifdef PRECISION_PROBE
   call pp_range('push:ux:in', ux)                      ! whole array
   call pp_count('push:np', size(x, kind=int64))
#endif
   do i = 1, n
#ifdef PRECISION_PROBE
      x(i) = pp_add('push:x+=v*dt', x(i), ux(i)*dt)     ! operand gap: absorption
      k = pp_sub('push:gamma-1', g, 1.0_wp)             ! cancelled bits
#else
      x(i) = x(i) + ux(i)*dt
      k = g - 1.0_wp
#endif
   end do
   ...
#ifdef PRECISION_PROBE
   call pp_report()        ! once, before the program ends
#endif
```

| Call | Records |
|------|---------|
| `call pp_range(name, x)` | scalar or rank 1–3 array, real32 or real64: extremes, binades, zeros, subnormals, integer-valuedness |
| `pp_sub(name, a, b)`, `pp_add(name, a, b)` | functions returning `a − b` / `a + b`; cancelled bits and operand gap |
| `call pp_demote(name, ref, lo)` | real64 reference next to its real32 evaluation |
| `call pp_count(name, n)` | integer extremes |

`pp_sub` and `pp_add` have side effects: not inside `pure` / `elemental`
procedures or `do concurrent`. Calls are serialized by an OpenMP critical
section; probe whole arrays outside threaded loops. Output and summary are
as for C++ (`PRECISION_PROBE_DIR`, `scripts/probe-summary.py`).

## Standalone Test

The function under test:

```fortran
module kinetic
   use precision_kinds, only: wp
   use stable_math, only: sqrt1pm1
   implicit none
contains
   !> (gamma - 1) for u = gamma * v
   elemental function gamma_minus_one(u, inv_c2) result(g1)
      real(wp), intent(in) :: u, inv_c2
      real(wp) :: g1
      ! Written as sqrt1pm1(u**2/c**2): the direct difference sqrt(1 + x) - 1
      ! loses all digits in single precision for u/c below about 3e-4.
      g1 = sqrt1pm1(u*u*inv_c2)
   end function gamma_minus_one
end module kinetic
```

The test, built once per profile:

```fortran
!> Frozen copy of the original at the original precision. Test-only: never
!> linked into the application, never regenerated from the modified function.
module kinetic_reference
   use, intrinsic :: iso_fortran_env, only: real64
   implicit none
contains
   elemental function gamma_minus_one_reference(u, inv_c2) result(g1)
      real(real64), intent(in) :: u, inv_c2
      real(real64) :: g1
      g1 = sqrt(1.0_real64 + u*u*inv_c2) - 1.0_real64
   end function gamma_minus_one_reference
end module kinetic_reference

program test_gamma_minus_one
   use, intrinsic :: iso_fortran_env, only: real64
   use precision_kinds, only: wp
   use kinetic, only: gamma_minus_one
   use kinetic_reference, only: gamma_minus_one_reference
   use stable_math, only: sqrt1pm1
   use precision_port_compare
   implicit none

   integer, parameter :: n = 480
   real(real64), parameter :: c = 299792458.0_real64, inv_c2 = 1.0_real64/(c*c)
   real(real64) :: u(n), reference(n), truth(n)
   real(wp) :: candidate(n)
   type(compare_budget) :: budget
   type(compare_result) :: res
   logical :: ok
   integer :: i

   do i = 1, n                     ! assumed range of u, log-spaced, both ends
      u(i) = 1.0e2_real64*(1.0e10_real64)**(real(i - 1, real64)/real(n - 1, real64))
   end do
   reference = gamma_minus_one_reference(u, inv_c2)
   candidate = gamma_minus_one(real(u, wp), real(inv_c2, wp))   ! rounded inputs
   ! stable form at real64, evaluated at the inputs the candidate received
   truth = sqrt1pm1(real(real(u, wp), real64)**2*real(real(inv_c2, wp), real64))

   ! Gate against the truth
   budget = default_budget(candidate(1), truth(1))
   res = compare_field(candidate, truth, budget)
   ok = report_and_gate(res, budget, 'gamma_minus_one vs truth')

   ! Did results move relative to the original? Informational.
   res = compare_field(candidate, reference, budget)
   if (.not. report_and_gate(res, budget, 'gamma_minus_one vs original')) then
      print '(a)', 'NOTE: results differ from the original beyond its budget'
   end if
   if (.not. ok) stop 1
end program test_gamma_minus_one
```

Notes:

- A real64 truth can judge only the real32 build. For the real64 build use
  a `real128` truth where the compiler provides that kind
  (`selected_real_kind(30) > 0`), or run that half of the adjudication with a
  compiler that does. Without a higher-precision truth, a `hi` difference
  from the frozen reference cannot be accepted.
- The harness supports real32 and real64 candidates against a real64
  reference, and real32 against real32, at ranks 1 to 3.
- A kernel that advances state every step is tested differently from a
  formula ([verification guide](../../references/verification-guide.md#kernels-with-state)):
  a budget in the state's own unit at the intended run length
  (`absolute_budget(candidate(1), reference(1), abs_tol)` for positions,
  times and phases), a truth that starts from the candidate's own rounded
  state, and a comparison modulo the period for periodic quantities. The
  helpers for the usual repairs are `add_with_carry(value, carry, increment)`
  and `normalize_cell_offset(cell, offset)` in `stable_math`. The
  [C++ flavor guide](../cpp/precision-types.md#standalone-test-for-a-kernel-with-state)
  has a complete example; the structure carries over line by line.
- Build order: `precision_kinds`, the helper module (with
  `stable_math_impl.inc` on the include path), the harness, the code under
  test, the test. One module directory per profile.

## Offloaded Code: OpenACC and OpenMP

- **Probes and the harness are host code.** Probe arrays outside offloaded
  regions; run Phase 1 on a CPU build where possible.
- **Device routines.** Helpers called inside offloaded loops must be
  available on the device. The helper functions are `elemental` and small:
  mark them for the device the way the project does for its own routines
  (`!$acc routine seq`, `!$omp declare target`), or let the compiler inline
  them from the module.
- **FP64 left in device code.** A `d0` literal or `dble()` in an offloaded
  loop of a `wp = real32` build is an FP64 instruction plus conversions on
  the device. Read the compiler's own offload report, and try
  `scripts/fp64-audit.py --mode cuda` on the executable: it works when
  `cuobjdump` can extract the device code the compiler embedded, which
  depends on the compiler (not verified for every offload tool chain).
  `-Wconversion-extra`-style diagnostics on the `lo` build find the same
  sites at the source.
- **Reductions.** `reduction(+:s)` over single-precision data sums in the
  kind of `s`. Give the reduction variable the wide kind where the target
  hardware makes that cheap, or restructure into a compensated reduction
  (sum and compensation reduced together) where it does not; see the
  [reformulation catalog](../../references/reformulation-catalog.md#c-accumulation-rung-l4).
- **Compiler math flags** for offloaded code may relax floating-point
  semantics by default. Record them at setup.
