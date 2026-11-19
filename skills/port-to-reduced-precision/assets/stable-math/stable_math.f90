! SPDX-License-Identifier: Apache-2.0

!> Named, numerically stable building blocks for reduced-precision ports
!> (Fortran counterpart of stable_math.hpp).
!>
!> `use stable_math` and call the helpers by name instead of inlining the
!> tricks: `sqrt1pm1(x)` tells the next reader both what is computed and that
!> the obvious `sqrt(1 + x) - 1` was avoided on purpose. Every helper is
!> generic over real32 and real64, so one source serves every working kind,
!> and is an exact algebraic identity of the expression it replaces.
!>
!> The bodies live once in stable_math_impl.inc, which is included for each
!> kind. Copy both files into the project and keep only what the port uses.
!>
!> Do NOT compile this module with value-unsafe math (-ffast-math, -Ofast,
!> -fast, -Mfprelaxed, -fp-model fast): reassociation deletes the
!> compensation terms and silently turns the helpers back into the naive
!> forms. Run test_stable_math.f90 with the project's own flags to check.
module stable_math_r4
   use, intrinsic :: iso_fortran_env, only: real32
   implicit none
   private
   public :: two_sum, two_prod, diff_of_products, diff_of_squares, sqrt1pm1
   public :: one_minus_cos, sm_log1p, sm_expm1, log1mexp, log1pexp, norm3, lerp
   public :: compensated_sum, compensated_dot, cell_index_clamped, wrap_periodic
   public :: normalize_cell_offset, add_with_carry
   integer, parameter :: wp = real32
   real(wp), parameter :: split = 4097.0_wp        ! 2**12 + 1
contains
   include 'stable_math_impl.inc'
end module stable_math_r4

module stable_math_r8
   use, intrinsic :: iso_fortran_env, only: real64
   implicit none
   private
   public :: two_sum, two_prod, diff_of_products, diff_of_squares, sqrt1pm1
   public :: one_minus_cos, sm_log1p, sm_expm1, log1mexp, log1pexp, norm3, lerp
   public :: compensated_sum, compensated_dot, cell_index_clamped, wrap_periodic
   public :: normalize_cell_offset, add_with_carry
   integer, parameter :: wp = real64
   real(wp), parameter :: split = 134217729.0_wp   ! 2**27 + 1
contains
   include 'stable_math_impl.inc'
end module stable_math_r8

module stable_math

   use, intrinsic :: iso_fortran_env, only: int32, int64, error_unit
   use stable_math_r4, only: &
      two_sum_r4 => two_sum, two_prod_r4 => two_prod, &
      diff_of_products_r4 => diff_of_products, diff_of_squares_r4 => diff_of_squares, &
      sqrt1pm1_r4 => sqrt1pm1, one_minus_cos_r4 => one_minus_cos, &
      sm_log1p_r4 => sm_log1p, sm_expm1_r4 => sm_expm1, log1mexp_r4 => log1mexp, &
      log1pexp_r4 => log1pexp, norm3_r4 => norm3, lerp_r4 => lerp, &
      compensated_sum_r4 => compensated_sum, compensated_dot_r4 => compensated_dot, &
      cell_index_clamped_r4 => cell_index_clamped, wrap_periodic_r4 => wrap_periodic, &
      normalize_cell_offset_r4 => normalize_cell_offset, add_with_carry_r4 => add_with_carry
   use stable_math_r8, only: &
      two_sum_r8 => two_sum, two_prod_r8 => two_prod, &
      diff_of_products_r8 => diff_of_products, diff_of_squares_r8 => diff_of_squares, &
      sqrt1pm1_r8 => sqrt1pm1, one_minus_cos_r8 => one_minus_cos, &
      sm_log1p_r8 => sm_log1p, sm_expm1_r8 => sm_expm1, log1mexp_r8 => log1mexp, &
      log1pexp_r8 => log1pexp, norm3_r8 => norm3, lerp_r8 => lerp, &
      compensated_sum_r8 => compensated_sum, compensated_dot_r8 => compensated_dot, &
      cell_index_clamped_r8 => cell_index_clamped, wrap_periodic_r8 => wrap_periodic, &
      normalize_cell_offset_r8 => normalize_cell_offset, add_with_carry_r8 => add_with_carry

   implicit none
   private

   public :: two_sum, two_prod, diff_of_products, diff_of_squares, sqrt1pm1
   public :: one_minus_cos, sm_log1p, sm_expm1, log1mexp, log1pexp, norm3, lerp
   public :: compensated_sum, compensated_dot, cell_index_clamped, wrap_periodic
   public :: normalize_cell_offset, add_with_carry
   public :: fits_int32, checked_narrow, widen_mul

   interface two_sum
      procedure :: two_sum_r4, two_sum_r8
   end interface
   interface two_prod
      procedure :: two_prod_r4, two_prod_r8
   end interface
   interface diff_of_products
      procedure :: diff_of_products_r4, diff_of_products_r8
   end interface
   interface diff_of_squares
      procedure :: diff_of_squares_r4, diff_of_squares_r8
   end interface
   interface sqrt1pm1
      procedure :: sqrt1pm1_r4, sqrt1pm1_r8
   end interface
   interface one_minus_cos
      procedure :: one_minus_cos_r4, one_minus_cos_r8
   end interface
   interface sm_log1p
      procedure :: sm_log1p_r4, sm_log1p_r8
   end interface
   interface sm_expm1
      procedure :: sm_expm1_r4, sm_expm1_r8
   end interface
   interface log1mexp
      procedure :: log1mexp_r4, log1mexp_r8
   end interface
   interface log1pexp
      procedure :: log1pexp_r4, log1pexp_r8
   end interface
   interface norm3
      procedure :: norm3_r4, norm3_r8
   end interface
   interface lerp
      procedure :: lerp_r4, lerp_r8
   end interface
   interface compensated_sum
      procedure :: compensated_sum_r4, compensated_sum_r8
   end interface
   interface compensated_dot
      procedure :: compensated_dot_r4, compensated_dot_r8
   end interface
   interface cell_index_clamped
      procedure :: cell_index_clamped_r4, cell_index_clamped_r8
   end interface
   interface wrap_periodic
      procedure :: wrap_periodic_r4, wrap_periodic_r8
   end interface
   interface normalize_cell_offset
      procedure :: normalize_cell_offset_r4, normalize_cell_offset_r8
   end interface
   interface add_with_carry
      procedure :: add_with_carry_r4, add_with_carry_r8
   end interface

contains

   !> True if n is representable as a default 32-bit integer.
   elemental logical function fits_int32(n)
      integer(int64), intent(in) :: n
      fits_int32 = n >= -int(huge(1_int32), int64) - 1_int64 .and. n <= int(huge(1_int32), int64)
   end function fits_int32

   !> Narrow n to 32 bits, stopping with a message if the value does not fit.
   !> Use it once at the boundary where a quantity enters its narrow
   !> representation (setup, I/O, redistribution), not inside hot loops.
   function checked_narrow(n, what) result(m)
      integer(int64), intent(in) :: n
      character(len=*), intent(in), optional :: what
      integer(int32) :: m
      if (.not. fits_int32(n)) then
         if (present(what)) then
            write (error_unit, '(3a,i0,a)') 'checked_narrow: ', what, ' = ', n, &
               ' does not fit in a 32-bit integer'
         else
            write (error_unit, '(a,i0,a)') 'checked_narrow: ', n, ' does not fit in a 32-bit integer'
         end if
         error stop 1
      end if
      m = int(n, int32)
   end function checked_narrow

   !> Product of two 32-bit values without overflow (nx*ny, n*stride, ...).
   elemental function widen_mul(a, b) result(p)
      integer(int32), intent(in) :: a, b
      integer(int64) :: p
      p = int(a, int64)*int(b, int64)
   end function widen_mul

end module stable_math
