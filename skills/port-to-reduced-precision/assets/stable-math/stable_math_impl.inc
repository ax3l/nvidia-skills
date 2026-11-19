! SPDX-License-Identifier: Apache-2.0
!
! Kind-generic bodies of the stable_math helpers. This file is included once
! per real kind by stable_math.f90, inside a module that defines
!     integer, parameter :: wp        the working kind
!     real(wp), parameter :: split    2**ceiling(digits/2) + 1 (Veltkamp)
! Do not compile it on its own.

   !> a + b = s + e exactly, for any magnitudes (Knuth).
   elemental subroutine two_sum(a, b, s, e)
      real(wp), intent(in) :: a, b
      real(wp), intent(out) :: s, e
      real(wp) :: bb
      s = a + b
      bb = s - a
      e = (a - (s - bb)) + (b - bb)
   end subroutine two_sum

   !> a * b = p + e exactly, barring over/underflow (Dekker, with Veltkamp
   !> splitting, so that no fused multiply-add is required).
   elemental subroutine two_prod(a, b, p, e)
      real(wp), intent(in) :: a, b
      real(wp), intent(out) :: p, e
      real(wp) :: a_hi, a_lo, b_hi, b_lo, t
      p = a*b
      t = split*a
      a_hi = t - (t - a)
      a_lo = a - a_hi
      t = split*b
      b_hi = t - (t - b)
      b_lo = b - b_hi
      e = ((a_hi*b_hi - p) + a_hi*b_lo + a_lo*b_hi) + a_lo*b_lo
   end subroutine two_prod

   !> a*b - c*d, accurate when the two products nearly cancel.
   elemental function diff_of_products(a, b, c, d) result(r)
      real(wp), intent(in) :: a, b, c, d
      real(wp) :: r, p1, e1, p2, e2
      call two_prod(a, b, p1, e1)
      call two_prod(c, d, p2, e2)
      r = (p1 - p2) + (e1 - e2)
   end function diff_of_products

   !> a**2 - b**2 as (a - b)(a + b): no cancellation of the squares.
   elemental function diff_of_squares(a, b) result(r)
      real(wp), intent(in) :: a, b
      real(wp) :: r
      r = (a - b)*(a + b)
   end function diff_of_squares

   !> sqrt(1 + x) - 1 for x >= -1, accurate for small |x|.
   !> Example: the relativistic gamma - 1 is sqrt1pm1(u**2 / c**2).
   elemental function sqrt1pm1(x) result(r)
      real(wp), intent(in) :: x
      real(wp) :: r
      r = x/(sqrt(1.0_wp + x) + 1.0_wp)
   end function sqrt1pm1

   !> 1 - cos(x) as 2 sin(x/2)**2: accurate for small |x|.
   elemental function one_minus_cos(x) result(r)
      real(wp), intent(in) :: x
      real(wp) :: r, s
      s = sin(0.5_wp*x)
      r = 2.0_wp*s*s
   end function one_minus_cos

   !> log(1 + x), accurate for small |x| (Kahan's correction of log(1 + x)).
   elemental function sm_log1p(x) result(r)
      real(wp), intent(in) :: x
      real(wp) :: r, u
      u = 1.0_wp + x
      if (u == 1.0_wp) then
         r = x
      else
         r = log(u)*(x/(u - 1.0_wp))
      end if
   end function sm_log1p

   !> exp(x) - 1, accurate for small |x| (Kahan's correction of exp(x) - 1).
   elemental function sm_expm1(x) result(r)
      real(wp), intent(in) :: x
      real(wp) :: r, u
      u = exp(x)
      if (u == 1.0_wp) then
         r = x
      else if (u - 1.0_wp == -1.0_wp) then
         r = -1.0_wp
      else
         r = (u - 1.0_wp)*(x/log(u))
      end if
   end function sm_expm1

   !> log(1 - exp(x)) for x < 0, accurate over the whole range.
   elemental function log1mexp(x) result(r)
      real(wp), intent(in) :: x
      real(wp) :: r
      if (x > -0.69314718055994530942_wp) then
         r = log(-sm_expm1(x))
      else
         r = sm_log1p(-exp(x))
      end if
   end function log1mexp

   !> log(1 + exp(x)) without overflow for large x.
   elemental function log1pexp(x) result(r)
      real(wp), intent(in) :: x
      real(wp) :: r
      if (x > 0.0_wp) then
         r = x + sm_log1p(exp(-x))
      else
         r = sm_log1p(exp(x))
      end if
   end function log1pexp

   !> sqrt(x**2 + y**2 + z**2) without overflow or underflow of the squares.
   !> For two components use the intrinsic hypot.
   elemental function norm3(x, y, z) result(r)
      real(wp), intent(in) :: x, y, z
      real(wp) :: r, m
      m = max(abs(x), abs(y), abs(z))
      if (m == 0.0_wp .or. m > huge(m)) then
         r = m
      else
         r = m*sqrt((x/m)**2 + (y/m)**2 + (z/m)**2)
      end if
   end function norm3

   !> Linear interpolation that returns exactly a at t = 0 and b at t = 1.
   elemental function lerp(a, b, t) result(r)
      real(wp), intent(in) :: a, b, t
      real(wp) :: r
      if (t < 0.5_wp) then
         r = a + t*(b - a)
      else
         r = b - (1.0_wp - t)*(b - a)
      end if
   end function lerp

   !> Compensated sum of x (two_sum based: the Kahan-Babuska-Neumaier /
   !> Ogita-Rump-Oishi "Sum2" scheme). As accurate as a sum accumulated in
   !> twice the working precision and then rounded; stays accurate when a
   !> term is larger than the running sum, unlike classic Kahan summation.
   pure function compensated_sum(x) result(r)
      real(wp), intent(in) :: x(:)
      real(wp) :: r, s, c, e, t
      integer :: i
      s = 0.0_wp
      c = 0.0_wp
      do i = 1, size(x)
         call two_sum(s, x(i), t, e)
         s = t
         c = c + e
      end do
      r = s + c
   end function compensated_sum

   !> Compensated dot product (Ogita-Rump-Oishi "Dot2"): as accurate as a dot
   !> product evaluated in twice the working precision.
   pure function compensated_dot(x, y) result(r)
      real(wp), intent(in) :: x(:), y(:)
      real(wp) :: r, p, s, h, h_err, q, q_err
      integer :: i
      p = 0.0_wp
      s = 0.0_wp
      do i = 1, min(size(x), size(y))
         call two_prod(x(i), y(i), h, h_err)
         call two_sum(p, h, q, q_err)
         p = q
         s = s + (q_err + h_err)
      end do
      r = p + s
   end function compensated_dot

   !> Index (1-based) of the cell containing x on a uniform grid of n cells
   !> starting at x_lo, clamped to [1, n]. (x - x_lo)*inv_dx can round up to
   !> exactly n for a position still inside the domain; in single precision
   !> that happens routinely. A NaN position maps to 1.
   elemental function cell_index_clamped(x, x_lo, inv_dx, n) result(i)
      real(wp), intent(in) :: x, x_lo, inv_dx
      integer, intent(in) :: n
      integer :: i
      real(wp) :: s
      s = (x - x_lo)*inv_dx
      if (.not. (s > 0.0_wp)) then
         i = 1
      else if (s >= real(n, wp)) then
         i = n
      else
         i = min(int(s) + 1, n)
      end if
   end function cell_index_clamped

   !> Map x into the half-open periodic interval [lo, hi). The textbook wrap
   !> can return exactly hi (for x a rounding error below lo); hi and lo are
   !> the same point, so that case is mapped to lo. NaN propagates.
   elemental function wrap_periodic(x, lo, hi) result(y)
      real(wp), intent(in) :: x, lo, hi
      real(wp) :: y, length
      length = hi - lo
      y = x - length*floor((x - lo)/length)
      if (y < lo) y = lo
      if (y >= hi) y = lo
   end function wrap_periodic

   !> A position stored as a cell index plus an offset in [0, 1) inside that
   !> cell (in units of the cell size): bring the pair back into this form
   !> after the offset has been moved. The sub-cell part then keeps the full
   !> significand whatever the size of the domain. Periodic wrapping and
   !> domain bounds are integer operations on `cell`, applied by the caller.
   !> An offset a rounding error below 0 rounds to exactly 1 in the cell
   !> below: that is offset 0 of the original cell and is returned as such.
   !> The move must stay within the integer range. NaN is left unchanged.
   elemental subroutine normalize_cell_offset(cell, offset)
      integer, intent(inout) :: cell
      real(wp), intent(inout) :: offset
      real(wp) :: whole
      if (.not. (offset < 0.0_wp .or. offset >= 1.0_wp)) return
      whole = aint(offset)
      if (whole > offset) whole = whole - 1.0_wp
      cell = cell + int(whole)
      offset = offset - whole
      if (offset >= 1.0_wp) then
         offset = 0.0_wp
         cell = cell + 1
      end if
   end subroutine normalize_cell_offset

   !> One step of a value that is advanced by many small increments (a
   !> position, a momentum, a time, a phase): value = value + increment, with
   !> `carry` keeping what the addition rounded away so that the next step
   !> adds it back. A plain update drops the low bits of every increment, in
   !> the same direction while `value` stays in one binade, so the error
   !> grows with the number of steps. With the carry, value + carry stays
   !> within about one rounding of the exact running total. Cost: 7
   !> additions instead of 1 and one more word of state per value; start the
   !> carry at 0 and keep it with its value (copies, sorting, restart files).
   elemental subroutine add_with_carry(value, carry, increment)
      real(wp), intent(inout) :: value, carry
      real(wp), intent(in) :: increment
      real(wp) :: s, e
      call two_sum(value, increment + carry, s, e)
      value = s
      carry = e
   end subroutine add_with_carry
