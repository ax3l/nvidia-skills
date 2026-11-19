! SPDX-License-Identifier: Apache-2.0

!> Comparison harness for reduced-precision porting tests. Fortran
!> counterpart of precision_compare.hpp, with the same gates and the same
!> environment variables.
!>
!> One test program is built twice: at the original precision ("hi") and at
!> the target precision ("lo"). It calls the candidate at the build's working
!> kind and compares against a reference held at the ORIGINAL kind:
!>
!>   budget = default_budget(candidate(1), reference(1))
!>   result = compare_field(candidate, reference, budget)
!>   if (.not. report_and_gate(result, budget, 'kernel')) stop 1
!>
!> The gate follows from the two kinds: a candidate as precise as the
!> reference gets the "same precision" gate (results must not have changed
!> beyond roundoff), a narrower candidate the "narrower" gate (error within a
!> small multiple of the candidate kind's epsilon).
!>
!> default_budget() is the FUNCTION-LEVEL gate: every element is judged on
!> its own relative error, so a sweep over many decades of inputs cannot hide
!> a wrong small result behind a large one. For whole fields of an
!> application run, where cells far below the peak are legitimately noisy,
!> build the gate from the user's accuracy budget with
!> field_budget(candidate(1), reference(1), wmad_tol, rel_tol, rel_floor).
!> For quantities whose zero is arbitrary (positions, times, phases, angles)
!> a relative error means nothing: gate them with
!> absolute_budget(candidate(1), reference(1), abs_tol), in a natural unit.
!>
!> Supported: candidate real32 or real64 against a real64 reference, and
!> real32 against real32, at ranks 1 to 3. `weight` is the physical weight of
!> each element (cell volume or area on a non-uniform mesh); omit it on a
!> uniform one.
!>
!> Environment overrides: PP_WMAD_TOL, PP_REL_TOL, PP_ABS_TOL, PP_REL_FLOOR,
!> PP_OUTLIER_CHECK (on/off), and for the "narrower" gate PP_LO_WMAD_EPS
!> (default 4) and PP_LO_AMP_MAX (default 16), in units of the candidate epsilon.
!> See references/verification-guide.md.
module precision_port_compare

   use, intrinsic :: iso_fortran_env, only: real32, real64, int64
   use, intrinsic :: ieee_arithmetic, only: ieee_is_finite

   implicit none
   private

   public :: compare_budget, compare_result
   public :: default_budget, field_budget, absolute_budget, compare_field, report_and_gate

   integer, parameter :: dp = real64

   !> Gate thresholds for one comparison.
   type :: compare_budget
      real(dp) :: wmad_tol = 0.0_dp       !< limit on wmad; this is the gate
      real(dp) :: rel_tol = 0.0_dp        !< element passes if rel <= rel_tol ...
      real(dp) :: abs_tol = 0.0_dp        !< ... OR abs <= abs_tol
      real(dp) :: rel_floor = 0.0_dp      !< floor on |ref|, as a fraction of the peak
      real(dp) :: cand_eps = 0.0_dp       !< epsilon of the candidate kind
      logical  :: outlier_check = .true.  !< gate on n_exceed > 0 (reviewable)
      logical  :: narrower = .false.       !< candidate is narrower than the reference
      logical  :: absolute = .false.      !< absolute gate: only abs_tol decides
   end type compare_budget

   !> Outcome of one field comparison. `wmad` gates; the rest is diagnosis.
   type :: compare_result
      real(dp)       :: wmad = 0.0_dp
      real(dp)       :: max_rel = 0.0_dp
      real(dp)       :: max_abs = 0.0_dp       !< largest |candidate - reference|
      real(dp)       :: peak = 0.0_dp          !< max |reference|
      integer(int64) :: n_exceed = 0_int64
      integer(int64) :: n_total = 0_int64
      integer(int64) :: worst = 1_int64        !< flat index, 1-based, column-major
      logical        :: nonfinite = .false.
      logical        :: empty_field = .false.
      logical        :: shape_error = .false.  !< candidate, reference, weight disagree
   end type compare_result

   interface default_budget
      module procedure budget_r4_r8, budget_r8_r8, budget_r4_r4
   end interface default_budget

   interface field_budget
      module procedure field_budget_r4_r8, field_budget_r8_r8, field_budget_r4_r4
   end interface field_budget

   !> Absolute gate, for quantities whose zero is arbitrary (positions,
   !> times, phases, angles): absolute_budget(candidate(1), reference(1), abs_tol)
   interface absolute_budget
      module procedure absolute_budget_r4_r8, absolute_budget_r8_r8, absolute_budget_r4_r4
   end interface absolute_budget

   interface compare_field
      module procedure cmp_r4_r8_1d, cmp_r4_r8_2d, cmp_r4_r8_3d
      module procedure cmp_r8_r8_1d, cmp_r8_r8_2d, cmp_r8_r8_3d
      module procedure cmp_r4_r4_1d, cmp_r4_r4_2d, cmp_r4_r4_3d
   end interface compare_field

contains

   !> Function-level default rules. cand_digits / ref_digits are significand
   !> bits; cand_tiny is the smallest normal number of the candidate kind.
   !>
   !> same precision: wmad 1e-12, rel 1e-10 (real64 reference); 1e-6, 1e-4 (real32)
   !> narrower:       wmad 4 eps, rel 16 eps, eps = epsilon of the candidate kind
   !> both:           no magnitude floor; abs tolerance = cand_tiny
   function make_budget(cand_eps, cand_tiny, cand_digits, ref_digits) result(b)
      real(dp), intent(in) :: cand_eps, cand_tiny
      integer, intent(in) :: cand_digits, ref_digits
      type(compare_budget) :: b
      real(dp) :: scale

      b%cand_eps = cand_eps
      b%narrower = cand_digits < ref_digits
      b%abs_tol = cand_tiny
      b%rel_floor = 0.0_dp
      if (b%narrower) then
         scale = 4.0_dp
         call get_real_env('PP_LO_WMAD_EPS', scale)
         b%wmad_tol = scale*cand_eps
         scale = 16.0_dp
         call get_real_env('PP_LO_AMP_MAX', scale)
         b%rel_tol = scale*cand_eps
      else if (ref_digits >= 53) then
         b%wmad_tol = 1.0e-12_dp
         b%rel_tol = 1.0e-10_dp
      else
         b%wmad_tol = 1.0e-6_dp
         b%rel_tol = 1.0e-4_dp
      end if
      call get_real_env('PP_WMAD_TOL', b%wmad_tol)
      call get_real_env('PP_REL_TOL', b%rel_tol)
      call get_real_env('PP_ABS_TOL', b%abs_tol)
      call get_real_env('PP_REL_FLOOR', b%rel_floor)
      call get_flag_env('PP_OUTLIER_CHECK', b%outlier_check)
   end function make_budget

   function budget_r4_r8(candidate, reference) result(b)
      real(real32), intent(in) :: candidate
      real(real64), intent(in) :: reference
      type(compare_budget) :: b
      b = make_budget(real(epsilon(candidate), dp), real(tiny(candidate), dp), &
                      digits(candidate), digits(reference))
   end function budget_r4_r8

   function budget_r8_r8(candidate, reference) result(b)
      real(real64), intent(in) :: candidate, reference
      type(compare_budget) :: b
      b = make_budget(real(epsilon(candidate), dp), real(tiny(candidate), dp), &
                      digits(candidate), digits(reference))
   end function budget_r8_r8

   function budget_r4_r4(candidate, reference) result(b)
      real(real32), intent(in) :: candidate, reference
      type(compare_budget) :: b
      b = make_budget(real(epsilon(candidate), dp), real(tiny(candidate), dp), &
                      digits(candidate), digits(reference))
   end function budget_r4_r4

   !> Field-level gate built from an explicit accuracy budget, for a whole
   !> output field of an application run. `rel_floor` is the fraction of the
   !> field's peak below which cells are compared against the floor instead
   !> of their own magnitude. The numbers come from the user's budget for
   !> that quantity; the PP_* environment overrides still apply.
   function make_field_budget(cand_eps, narrower, wmad_tol, rel_tol, rel_floor) result(b)
      real(dp), intent(in) :: cand_eps, wmad_tol, rel_tol, rel_floor
      logical, intent(in) :: narrower
      type(compare_budget) :: b
      b%cand_eps = cand_eps
      b%narrower = narrower
      b%wmad_tol = wmad_tol
      b%rel_tol = rel_tol
      b%abs_tol = 0.0_dp
      b%rel_floor = rel_floor
      call get_real_env('PP_WMAD_TOL', b%wmad_tol)
      call get_real_env('PP_REL_TOL', b%rel_tol)
      call get_real_env('PP_ABS_TOL', b%abs_tol)
      call get_real_env('PP_REL_FLOOR', b%rel_floor)
      call get_flag_env('PP_OUTLIER_CHECK', b%outlier_check)
   end function make_field_budget

   function field_budget_r4_r8(candidate, reference, wmad_tol, rel_tol, rel_floor) result(b)
      real(real32), intent(in) :: candidate
      real(real64), intent(in) :: reference
      real(dp), intent(in) :: wmad_tol, rel_tol, rel_floor
      type(compare_budget) :: b
      b = make_field_budget(real(epsilon(candidate), dp), digits(candidate) < digits(reference), &
                            wmad_tol, rel_tol, rel_floor)
   end function field_budget_r4_r8

   function field_budget_r8_r8(candidate, reference, wmad_tol, rel_tol, rel_floor) result(b)
      real(real64), intent(in) :: candidate, reference
      real(dp), intent(in) :: wmad_tol, rel_tol, rel_floor
      type(compare_budget) :: b
      b = make_field_budget(real(epsilon(candidate), dp), digits(candidate) < digits(reference), &
                            wmad_tol, rel_tol, rel_floor)
   end function field_budget_r8_r8

   function field_budget_r4_r4(candidate, reference, wmad_tol, rel_tol, rel_floor) result(b)
      real(real32), intent(in) :: candidate, reference
      real(dp), intent(in) :: wmad_tol, rel_tol, rel_floor
      type(compare_budget) :: b
      b = make_field_budget(real(epsilon(candidate), dp), digits(candidate) < digits(reference), &
                            wmad_tol, rel_tol, rel_floor)
   end function field_budget_r4_r4

   !> Comparison on flattened double-precision copies.
   !> Absolute gate: every element must lie within `abs_tol` of its reference,
   !> in the quantity's own unit; wmad is reported and not gated. Choose the
   !> tolerance in a natural unit (a fraction of a cell, of the displacement
   !> over the test, of a period). Of the environment overrides only
   !> PP_ABS_TOL applies.
   function make_absolute_budget(cand_eps, narrower, abs_tol) result(b)
      real(dp), intent(in) :: cand_eps, abs_tol
      logical, intent(in) :: narrower
      type(compare_budget) :: b
      b%cand_eps = cand_eps
      b%narrower = narrower
      b%absolute = .true.
      b%wmad_tol = huge(1.0_dp)
      b%rel_tol = 0.0_dp
      b%abs_tol = abs_tol
      b%rel_floor = 0.0_dp
      b%outlier_check = .true.
      call get_real_env('PP_ABS_TOL', b%abs_tol)
   end function make_absolute_budget

   function absolute_budget_r4_r8(candidate, reference, abs_tol) result(b)
      real(real32), intent(in) :: candidate
      real(real64), intent(in) :: reference
      real(dp), intent(in) :: abs_tol
      type(compare_budget) :: b
      b = make_absolute_budget(real(epsilon(candidate), dp), &
                               digits(candidate) < digits(reference), abs_tol)
   end function absolute_budget_r4_r8

   function absolute_budget_r8_r8(candidate, reference, abs_tol) result(b)
      real(real64), intent(in) :: candidate, reference
      real(dp), intent(in) :: abs_tol
      type(compare_budget) :: b
      b = make_absolute_budget(real(epsilon(candidate), dp), &
                               digits(candidate) < digits(reference), abs_tol)
   end function absolute_budget_r8_r8

   function absolute_budget_r4_r4(candidate, reference, abs_tol) result(b)
      real(real32), intent(in) :: candidate, reference
      real(dp), intent(in) :: abs_tol
      type(compare_budget) :: b
      b = make_absolute_budget(real(epsilon(candidate), dp), &
                               digits(candidate) < digits(reference), abs_tol)
   end function absolute_budget_r4_r4

   function compare_core(a, ref, budget, w) result(c)
      real(dp), intent(in) :: a(:), ref(:)
      type(compare_budget), intent(in) :: budget
      real(dp), intent(in), optional :: w(:)
      type(compare_result) :: c
      real(dp) :: num, den, ad, rd, wi, floor_v, scale
      integer(int64) :: i, worst_abs

      c%n_total = size(ref, kind=int64)
      if (size(a, kind=int64) /= c%n_total) then
         c%shape_error = .true.
         return
      end if
      if (present(w)) then
         if (size(w, kind=int64) /= c%n_total) then
            c%shape_error = .true.
            return
         end if
      end if
      if (c%n_total == 0) then
         c%empty_field = .true.
         return
      end if

      ! Non-finite anywhere is an unconditional failure, never waivable.
      do i = 1, c%n_total
         if (.not. (ieee_is_finite(a(i)) .and. ieee_is_finite(ref(i)))) then
            c%nonfinite = .true.
            c%worst = i
            c%n_exceed = -1
            return
         end if
         c%peak = max(c%peak, abs(ref(i)))
      end do

      floor_v = budget%rel_floor*c%peak
      num = 0.0_dp
      den = 0.0_dp
      worst_abs = 1_int64
      do i = 1, c%n_total
         wi = 1.0_dp
         if (present(w)) wi = w(i)
         ad = abs(a(i) - ref(i))
         num = num + wi*ad
         den = den + wi*abs(ref(i))
         scale = abs(ref(i)) + floor_v
         if (scale > 0.0_dp) then
            rd = ad/scale
         else if (ad > budget%abs_tol) then
            ! zero reference and no floor: only an absolute criterion applies
            rd = huge(1.0_dp)
         else
            rd = 0.0_dp
         end if
         if (rd > c%max_rel) then
            c%max_rel = rd
            c%worst = i
         end if
         if (ad > c%max_abs) then
            c%max_abs = ad
            worst_abs = i
         end if
         ! An element passes if EITHER tolerance is met.
         if (ad > budget%abs_tol .and. rd > budget%rel_tol) c%n_exceed = c%n_exceed + 1
      end do
      if (den > 0.0_dp) then
         c%wmad = num/den
      else
         c%wmad = num
      end if
      if (budget%absolute) c%worst = worst_abs
   end function compare_core

   ! --- candidate real32, reference real64 ------------------------------------

   function cmp_r4_r8_1d(a, ref, budget, weight) result(c)
      real(real32), intent(in) :: a(:)
      real(real64), intent(in) :: ref(:)
      type(compare_budget), intent(in) :: budget
      real(real64), intent(in), optional :: weight(:)
      type(compare_result) :: c
      c = compare_core(real(a, dp), ref, budget, weight)
   end function cmp_r4_r8_1d

   function cmp_r4_r8_2d(a, ref, budget, weight) result(c)
      real(real32), intent(in) :: a(:, :)
      real(real64), intent(in) :: ref(:, :)
      type(compare_budget), intent(in) :: budget
      real(real64), intent(in), optional :: weight(:, :)
      type(compare_result) :: c
      if (present(weight)) then
         c = compare_core(reshape(real(a, dp), [size(a)]), reshape(ref, [size(ref)]), budget, &
                          reshape(weight, [size(weight)]))
      else
         c = compare_core(reshape(real(a, dp), [size(a)]), reshape(ref, [size(ref)]), budget)
      end if
   end function cmp_r4_r8_2d

   function cmp_r4_r8_3d(a, ref, budget, weight) result(c)
      real(real32), intent(in) :: a(:, :, :)
      real(real64), intent(in) :: ref(:, :, :)
      type(compare_budget), intent(in) :: budget
      real(real64), intent(in), optional :: weight(:, :, :)
      type(compare_result) :: c
      if (present(weight)) then
         c = compare_core(reshape(real(a, dp), [size(a)]), reshape(ref, [size(ref)]), budget, &
                          reshape(weight, [size(weight)]))
      else
         c = compare_core(reshape(real(a, dp), [size(a)]), reshape(ref, [size(ref)]), budget)
      end if
   end function cmp_r4_r8_3d

   ! --- candidate real64, reference real64 ------------------------------------

   function cmp_r8_r8_1d(a, ref, budget, weight) result(c)
      real(real64), intent(in) :: a(:), ref(:)
      type(compare_budget), intent(in) :: budget
      real(real64), intent(in), optional :: weight(:)
      type(compare_result) :: c
      c = compare_core(a, ref, budget, weight)
   end function cmp_r8_r8_1d

   function cmp_r8_r8_2d(a, ref, budget, weight) result(c)
      real(real64), intent(in) :: a(:, :), ref(:, :)
      type(compare_budget), intent(in) :: budget
      real(real64), intent(in), optional :: weight(:, :)
      type(compare_result) :: c
      if (present(weight)) then
         c = compare_core(reshape(a, [size(a)]), reshape(ref, [size(ref)]), budget, &
                          reshape(weight, [size(weight)]))
      else
         c = compare_core(reshape(a, [size(a)]), reshape(ref, [size(ref)]), budget)
      end if
   end function cmp_r8_r8_2d

   function cmp_r8_r8_3d(a, ref, budget, weight) result(c)
      real(real64), intent(in) :: a(:, :, :), ref(:, :, :)
      type(compare_budget), intent(in) :: budget
      real(real64), intent(in), optional :: weight(:, :, :)
      type(compare_result) :: c
      if (present(weight)) then
         c = compare_core(reshape(a, [size(a)]), reshape(ref, [size(ref)]), budget, &
                          reshape(weight, [size(weight)]))
      else
         c = compare_core(reshape(a, [size(a)]), reshape(ref, [size(ref)]), budget)
      end if
   end function cmp_r8_r8_3d

   ! --- candidate real32, reference real32 ------------------------------------

   function cmp_r4_r4_1d(a, ref, budget, weight) result(c)
      real(real32), intent(in) :: a(:), ref(:)
      type(compare_budget), intent(in) :: budget
      real(real64), intent(in), optional :: weight(:)
      type(compare_result) :: c
      c = compare_core(real(a, dp), real(ref, dp), budget, weight)
   end function cmp_r4_r4_1d

   function cmp_r4_r4_2d(a, ref, budget, weight) result(c)
      real(real32), intent(in) :: a(:, :), ref(:, :)
      type(compare_budget), intent(in) :: budget
      real(real64), intent(in), optional :: weight(:, :)
      type(compare_result) :: c
      if (present(weight)) then
         c = compare_core(reshape(real(a, dp), [size(a)]), reshape(real(ref, dp), [size(ref)]), &
                          budget, reshape(weight, [size(weight)]))
      else
         c = compare_core(reshape(real(a, dp), [size(a)]), reshape(real(ref, dp), [size(ref)]), &
                          budget)
      end if
   end function cmp_r4_r4_2d

   function cmp_r4_r4_3d(a, ref, budget, weight) result(c)
      real(real32), intent(in) :: a(:, :, :), ref(:, :, :)
      type(compare_budget), intent(in) :: budget
      real(real64), intent(in), optional :: weight(:, :, :)
      type(compare_result) :: c
      if (present(weight)) then
         c = compare_core(reshape(real(a, dp), [size(a)]), reshape(real(ref, dp), [size(ref)]), &
                          budget, reshape(weight, [size(weight)]))
      else
         c = compare_core(reshape(real(a, dp), [size(a)]), reshape(real(ref, dp), [size(ref)]), &
                          budget)
      end if
   end function cmp_r4_r4_3d

   !> Print the required numbers and apply the gate. Returns .true. on pass.
   function report_and_gate(c, budget, label) result(ok)
      type(compare_result), intent(in) :: c
      type(compare_budget), intent(in) :: budget
      character(len=*), intent(in) :: label
      logical :: ok
      character(len=:), allocatable :: mode

      if (budget%absolute) then
         mode = 'absolute'
      else if (budget%narrower) then
         mode = 'narrower'
      else
         mode = 'same precision'
      end if
      ok = .false.
      if (c%shape_error) then
         print '(a)', 'SHAPE MISMATCH between candidate, reference or weight: this is a bug in the test'
         print '(5a)', 'VERIFICATION FAILED (shape, ', mode, '): ', label, ''
         return
      end if
      if (c%empty_field) then
         print '(a)', 'EMPTY FIELD: zero elements compared, so the result would prove nothing'
         print '(5a)', 'VERIFICATION FAILED (empty field, ', mode, '): ', label, ''
         return
      end if
      if (c%nonfinite) then
         print '(3a,i0)', 'NON-FINITE VALUES in ', label, ' at flat index ', c%worst
         print '(5a)', 'VERIFICATION FAILED (non-finite, ', mode, '): ', label, ''
         return
      end if

      if (budget%absolute) then
         print '(3a,es12.5,a,es10.3,a)', 'compare ', label, ' [absolute]: max_abs ', c%max_abs, &
            '  (abs_tol ', budget%abs_tol, ')'
         print '(a,i0,a,i0,a,i0,a,es10.3,a)', '  n_exceed ', c%n_exceed, ' of ', c%n_total, &
            '  worst_idx ', c%worst, '  (wmad ', c%wmad, ', not gated)'
         if (c%n_exceed > 0) then
            print '(2a)', 'VERIFICATION FAILED (absolute): ', label
            return
         end if
         print '(2a)', 'VERIFICATION PASSED (absolute): ', label
         ok = .true.
         return
      end if
      print '(5a,es12.5,a,es12.5)', 'compare ', label, ' [', mode, ']: wmad ', c%wmad, &
         '  max_rel ', c%max_rel
      if (budget%cand_eps > 0.0_dp) then
         print '(a,g0.3,a,g0.3)', '  in eps of the candidate kind: wmad ', c%wmad/budget%cand_eps, &
            '  max_rel ', c%max_rel/budget%cand_eps
      end if
      print '(a,i0,a,i0,a,i0)', '  n_exceed ', c%n_exceed, ' of ', c%n_total, '  worst_idx ', c%worst
      print '(a,es10.3,a,es10.3,a,es10.3,a,es10.3,a)', '  (wmad_tol ', budget%wmad_tol, &
         ', rel_tol ', budget%rel_tol, ', abs_tol ', budget%abs_tol, ', rel_floor ', &
         budget%rel_floor, ')'

      if (c%wmad > budget%wmad_tol) then
         print '(5a)', 'VERIFICATION FAILED (wmad, ', mode, '): ', label, ''
         return
      end if
      ! The outlier bound IS reviewable: see verification-guide.md,
      ! "Reviewing an outlier failure".
      if (budget%outlier_check .and. c%n_exceed > 0) then
         print '(5a)', 'VERIFICATION FAILED (outliers, ', mode, ') - review before waiving: ', label, ''
         return
      end if
      print '(5a)', 'VERIFICATION PASSED (', mode, '): ', label, ''
      ok = .true.
   end function report_and_gate

   !> Overwrite `value` from the environment if the variable holds a number.
   subroutine get_real_env(name, value)
      character(len=*), intent(in) :: name
      real(dp), intent(inout) :: value
      character(len=64) :: text
      integer :: length, status, ios
      real(dp) :: parsed

      call get_environment_variable(name, text, length, status)
      if (status /= 0 .or. length == 0) return
      read (text(1:length), *, iostat=ios) parsed
      if (ios == 0) value = parsed
   end subroutine get_real_env

   !> Overwrite `value` from the environment: on/off, true/false, yes/no, 1/0.
   subroutine get_flag_env(name, value)
      character(len=*), intent(in) :: name
      logical, intent(inout) :: value
      character(len=16) :: text
      integer :: length, status, i, code

      call get_environment_variable(name, text, length, status)
      if (status /= 0 .or. length == 0) return
      do i = 1, length
         code = iachar(text(i:i))
         if (code >= iachar('A') .and. code <= iachar('Z')) text(i:i) = achar(code + 32)
      end do
      select case (trim(text(1:length)))
      case ('0', 'off', 'false', 'no')
         value = .false.
      case ('1', 'on', 'true', 'yes')
         value = .true.
      end select
   end subroutine get_flag_env

end module precision_port_compare
