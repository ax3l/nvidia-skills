! SPDX-License-Identifier: Apache-2.0

!> Self-test for stable_math.f90, and a worked example of the comparison
!> harness: each helper is evaluated in real32 next to the naive expression
!> it replaces and gated against a real64 truth with the "lo" budget.
!>
!> Build and run with value-safe flags (no -ffast-math / -Ofast / -fast):
!>     gfortran -O2 ../test-harness/precision_compare.f90 stable_math.f90 \
!>              test_stable_math.F90 -o test_stable_math && ./test_stable_math
!>
!> Add -DSTABLE_MATH_TEST_QUAD on compilers that provide real128 to also
!> check the real64 helpers against a quad-precision truth.
!>
!> The program stops with status 1 if a stable form leaves its budget.
program test_stable_math

   use, intrinsic :: iso_fortran_env, only: real32, real64, int32, int64
#ifdef STABLE_MATH_TEST_QUAD
   use, intrinsic :: iso_fortran_env, only: real128
#endif
   use stable_math
   use precision_port_compare

   implicit none

   integer, parameter :: n = 400
   integer :: failures, i
   real(real64) :: x(n), small(n)
   real(real32) :: naive(n), stable(n)
   real(real64) :: truth(n)
   type(compare_budget) :: budget

   failures = 0
   budget = default_budget(1.0_real32, 1.0_real64)

   ! logarithmic sweeps
   do i = 1, n
      x(i) = 1.0e-9_real64*(3.0e9_real64)**(real(i - 1, real64)/real(n - 1, real64))
      small(i) = 1.0e-12_real64*(1.0e18_real64)**(real(i - 1, real64)/real(n - 1, real64))
   end do

   ! Truth is evaluated at the ROUNDED real32 input, so that it measures the
   ! formula and not the conversion of the input.
   naive = sqrt(1.0_real32 + real(small, real32)) - 1.0_real32
   stable = sqrt1pm1(real(small, real32))
   truth = sqrt1pm1(real(real(small, real32), real64))
   call judge('sqrt1pm1', naive, stable, truth)

   naive = 1.0_real32 - cos(real(x, real32))
   stable = one_minus_cos(real(x, real32))
   truth = one_minus_cos(real(real(x, real32), real64))
   call judge('one_minus_cos', naive, stable, truth)

   naive = log(1.0_real32 + real(small(1:n), real32))
   stable = sm_log1p(real(small, real32))
   truth = sm_log1p(real(real(small, real32), real64))
   call judge('sm_log1p', naive, stable, truth)

   naive = exp(real(x*1.0e-1_real64, real32)) - 1.0_real32
   stable = sm_expm1(real(x*1.0e-1_real64, real32))
   truth = sm_expm1(real(real(x*1.0e-1_real64, real32), real64))
   call judge('sm_expm1', naive, stable, truth)

   naive = log(1.0_real32 - exp(-real(x*10.0_real64, real32)))
   stable = log1mexp(-real(x*10.0_real64, real32))
   truth = log1mexp(-real(real(x*10.0_real64, real32), real64))
   call judge('log1mexp', naive, stable, truth)

   naive = log(1.0_real32 + exp(real(x*30.0_real64, real32)))
   stable = log1pexp(real(x*30.0_real64, real32))
   truth = log1pexp(real(real(x*30.0_real64, real32), real64))
   call judge('log1pexp', naive, stable, truth)

   block
      real(real32) :: a, b(n), c, d(n)
      a = 1.2345678_real32
      c = 0.9876543_real32
      do i = 1, n
         b(i) = a*(1.0_real32 - real(1.0e-6_real64*(1.0e5_real64)**(real(i - 1, real64)/real(n - 1, real64)), real32))
         d(i) = a*b(i)/c*(1.0_real32 - 1.0e-4_real32)
      end do
      naive = a*a - b*b
      stable = diff_of_squares(a, b)
      truth = diff_of_squares(real(a, real64), real(b, real64))
      call judge('diff_of_squares', naive, stable, truth)
      naive = a*b - c*d
      stable = diff_of_products(a, b, c, d)
      truth = diff_of_products(real(a, real64), real(b, real64), real(c, real64), real(d, real64))
      call judge('diff_of_products', naive, stable, truth)
   end block

   ! summation with large cancelling terms
   block
      integer, parameter :: m = 4*16384
      real(real32), allocatable :: v(:), w(:)
      real(real32) :: s_naive, d_naive
      real(real64) :: s_truth, d_truth, r(3)
      allocate (v(m), w(m))
      do i = 1, m, 4
         call random_number(r)
         v(i) = real(4096.0_real64*(1.0_real64 + r(1)), real32)
         v(i + 1) = real(r(2), real32)
         v(i + 2) = -v(i)
         v(i + 3) = real(r(3), real32)
      end do
      call random_number(w)
      w = 1.0_real32 + 0.5_real32*w
      s_truth = compensated_sum(real(v, real64))
      d_truth = compensated_dot(real(v, real64), real(w, real64))
      s_naive = 0.0_real32
      d_naive = 0.0_real32
      do i = 1, m
         s_naive = s_naive + v(i)
         d_naive = d_naive + v(i)*w(i)
      end do
      call judge('compensated_sum', [s_naive], [compensated_sum(v)], [s_truth])
      call judge('compensated_dot', [d_naive], [compensated_dot(v, w)], [d_truth])
   end block

   ! discrete helpers
   call check(cell_index_clamped(nearest(1.0_real32, -1.0_real32), 0.0_real32, 1000.0_real32, 1000) == 1000, &
              'cell index top')
   call check(cell_index_clamped(0.0_real32, 0.0_real32, 1000.0_real32, 1000) == 1, 'cell index bottom')
   call check(cell_index_clamped(-1.0_real32, 0.0_real32, 1000.0_real32, 1000) == 1, 'cell index below')
   call check(cell_index_clamped(2.0_real32, 0.0_real32, 1000.0_real32, 1000) == 1000, 'cell index above')
   block
      real(real32) :: wrapped
      wrapped = wrap_periodic(-epsilon(1.0_real32)/8.0_real32, 0.0_real32, 1.0_real32)
      call check(wrapped >= 0.0_real32 .and. wrapped < 1.0_real32, 'wrap_periodic range')
      call check(abs(wrap_periodic(2.25_real64, 0.0_real64, 1.0_real64) - 0.25_real64) < 1.0e-15_real64, &
                 'wrap_periodic value')
   end block
   block
      integer :: cell, k
      real(real32) :: offset, value, carry, plain, step
      real(real64) :: exact, carried_error, plain_error
      cell = 10; offset = 2.25_real32
      call normalize_cell_offset(cell, offset)
      call check(cell == 12 .and. offset == 0.25_real32, 'normalize_cell_offset up')
      cell = 10; offset = -0.25_real32
      call normalize_cell_offset(cell, offset)
      call check(cell == 9 .and. offset == 0.75_real32, 'normalize_cell_offset down')
      cell = 10; offset = -epsilon(1.0_real32)/8.0_real32
      call normalize_cell_offset(cell, offset)
      call check(cell == 10 .and. offset == 0.0_real32, 'normalize_cell_offset edge')

      ! 10^5 increments, each below half a unit in the last place of the value
      step = 0.3_real32*epsilon(1.0_real32)
      value = 1.0_real32; carry = 0.0_real32; plain = 1.0_real32
      do k = 1, 100000
         call add_with_carry(value, carry, step)
         plain = plain + step
      end do
      exact = 1.0_real64 + 100000.0_real64*real(step, real64)
      carried_error = abs(real(value, real64) + real(carry, real64) - exact)/epsilon(1.0_real32)
      plain_error = abs(real(plain, real64) - exact)/epsilon(1.0_real32)
      print '(a,es9.2,a,es9.2,a)', ' add_with_carry, 1e5 steps of 0.3 eps: plain ', plain_error, &
         ' eps, carried ', carried_error, ' eps'
      call check(carried_error <= 2.0_real64, 'add_with_carry')
      call check(plain_error > 1000.0_real64, 'add_with_carry: plain sum should stall')
   end block
   call check(abs(norm3(3.0_real32, 4.0_real32, 12.0_real32) - 13.0_real32) <= 4.0_real32*epsilon(1.0_real32)*13.0_real32, &
              'norm3 value')
   call check(norm3(1.0e30_real32, 1.0e30_real32, 1.0e30_real32) < huge(1.0_real32), 'norm3 overflow')
   call check(norm3(1.0e-30_real32, 1.0e-30_real32, 1.0e-30_real32) > 0.0_real32, 'norm3 underflow')
   call check(lerp(0.1_real32, 0.7_real32, 0.0_real32) == 0.1_real32 .and. &
              lerp(0.1_real32, 0.7_real32, 1.0_real32) == 0.7_real32, 'lerp endpoints')
   call check(fits_int32(2147483647_int64) .and. .not. fits_int32(2147483648_int64), 'fits_int32')
   call check(checked_narrow(123456_int64, 'n') == 123456_int32, 'checked_narrow')
   call check(widen_mul(100000_int32, 100000_int32) == 10000000000_int64, 'widen_mul')

#ifdef STABLE_MATH_TEST_QUAD
   call quad_checks()
#endif

   if (failures > 0) then
      print '(a,i0,a)', 'STABLE MATH SELF-TEST FAILED: ', failures, ' check(s)'
      stop 1
   end if
   print '(a)', 'STABLE MATH SELF-TEST PASSED'

contains

   !> The naive form must show the problem, the stable form must pass the gate.
   subroutine judge(name, naive_values, stable_values, truth_values)
      character(len=*), intent(in) :: name
      real(real32), intent(in) :: naive_values(:), stable_values(:)
      real(real64), intent(in) :: truth_values(:)
      type(compare_result) :: before, after

      before = compare_field(naive_values, truth_values, budget)
      after = compare_field(stable_values, truth_values, budget)
      if (before%nonfinite) then
         print '(a,t20,a,g0.3,a)', name, 'naive: non-finite values   stable max ', &
            after%max_rel/budget%cand_eps, ' eps'
      else
         print '(a,t20,a,g0.3,a,g0.3,a)', name, 'naive max ', before%max_rel/budget%cand_eps, &
            ' eps   stable max ', after%max_rel/budget%cand_eps, ' eps'
      end if
      if (.not. report_and_gate(after, budget, name)) failures = failures + 1
   end subroutine judge

   subroutine check(ok, what)
      logical, intent(in) :: ok
      character(len=*), intent(in) :: what
      if (.not. ok) then
         print '(2a)', 'FAILED: ', what
         failures = failures + 1
      end if
   end subroutine check

#ifdef STABLE_MATH_TEST_QUAD
   !> real64 helpers against a real128 truth, in units of epsilon(1.0_real64).
   subroutine quad_checks()
      real(real128) :: q, t
      real(real64) :: worst(4), e
      integer :: k
      worst = 0.0_real64
      do k = 1, n
         q = real(small(k), real128)
         t = q/(sqrt(1.0_real128 + q) + 1.0_real128)
         e = real(abs(real(sqrt1pm1(small(k)), real128) - t)/abs(t), real64)
         worst(1) = max(worst(1), e)
         t = log(1.0_real128 + q)
         e = real(abs(real(sm_log1p(small(k)), real128) - t)/abs(t), real64)
         worst(2) = max(worst(2), e)
         t = exp(q) - 1.0_real128
         e = real(abs(real(sm_expm1(small(k)), real128) - t)/abs(t), real64)
         worst(3) = max(worst(3), e)
         q = real(x(k), real128)
         t = 2.0_real128*sin(0.5_real128*q)**2
         e = real(abs(real(one_minus_cos(x(k)), real128) - t)/abs(t), real64)
         worst(4) = max(worst(4), e)
      end do
      worst = worst/epsilon(1.0_real64)
      print '(a,4(g0.3,1x))', 'fp64 max err [eps]: sqrt1pm1, sm_log1p, sm_expm1, one_minus_cos = ', worst
      call check(all(worst <= 8.0_real64), 'real64 helpers against real128')
   end subroutine quad_checks
#endif

end program test_stable_math
