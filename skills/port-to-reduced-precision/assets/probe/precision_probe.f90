! SPDX-License-Identifier: Apache-2.0

!> Value-range, cancellation and demotion probes for the reduced-precision
!> porting skill (Phase 1, Analyze). Fortran counterpart of
!> precision_probe.hpp; it writes the same JSON-lines records, so
!> scripts/probe-summary.py reads both.
!>
!>   call pp_range(name, x)          values flowing through a point; x is a
!>                                   real32/real64 scalar or rank 1-3 array
!>   d = pp_sub(name, a, b)          a - b, recording cancelled leading bits
!>   s = pp_add(name, a, b)          a + b, likewise, plus the operand gap
!>   call pp_demote(name, ref, lo)   real64 reference next to its real32
!>                                   evaluation (scalar or rank 1-3 arrays)
!>   call pp_count(name, n)          integer magnitudes (int32/int64)
!>   call pp_report()                write the records; call once before the
!>                                   program ends (Fortran has no exit hook)
!>
!> The module is always active when compiled in. Keep the calls inside
!> `#ifdef PRECISION_PROBE` blocks so that production builds neither call it
!> nor need to compile it. pp_sub / pp_add have side effects and therefore
!> cannot be used inside PURE or ELEMENTAL procedures or DO CONCURRENT.
!>
!> Host code only: probe arrays at the boundary of offloaded regions, or run
!> the analysis on a CPU build. Calls are serialized with an OpenMP critical
!> section; probe whole arrays rather than single elements in threaded loops.
!>
!> Output: ${PRECISION_PROBE_DIR:-.}/precision_probe.<pid>.jsonl
module precision_probe

   use, intrinsic :: iso_fortran_env, only: real32, real64, int32, int64, error_unit
   use, intrinsic :: iso_c_binding, only: c_int
   use, intrinsic :: ieee_arithmetic, only: ieee_is_finite

   implicit none
   private

   public :: pp_range, pp_sub, pp_add, pp_demote, pp_count, pp_report

   integer, parameter :: dp = real64
   integer, parameter :: max_sites = 512
   integer, parameter :: name_len = 96
   integer, parameter :: kind_range = 1, kind_addsub = 2, kind_demote = 3, kind_count = 4
   integer, parameter :: bin_lo = -1100, bin_hi = 1100   ! binades of real64
   integer, parameter :: bit_hi = 130                    ! lost / gap / amplification bins

   type :: site_t
      character(len=name_len) :: name = ''
      integer            :: kind = 0
      character(len=8)   :: type_label = ''
      integer(int64)     :: count = 0, n_zero = 0, n_negative = 0, n_nonfinite = 0
      integer(int64)     :: n_subnormal = 0, n_nonintegral = 0
      real(dp)           :: vmin = huge(1.0_dp), vmax = -huge(1.0_dp)
      real(dp)           :: min_abs_nz = huge(1.0_dp), max_abs = 0.0_dp
      real(dp)           :: min_normal = 0.0_dp
      integer(int64), allocatable :: hist(:)    ! binades / lost bits / log2 amplification
      integer(int64), allocatable :: hist2(:)   ! operand gap bits (addsub)
      integer(int64)     :: n_total_cancel = 0
      integer            :: max_lost = 0, max_gap = 0
      real(dp)           :: worst_a = 0.0_dp, worst_b = 0.0_dp
      real(dp)           :: eps_lo = 0.0_dp, max_amp = 0.0_dp, sum_amp = 0.0_dp
      integer(int64)     :: n_amp = 0, n_lo_nonfinite = 0, n_ref_nonfinite = 0
      integer(int64)     :: n_ref_zero_mismatch = 0
      integer(int64)     :: imin = huge(1_int64), imax = -huge(1_int64)
   end type site_t

   type(site_t), allocatable, save :: sites(:)
   integer, save :: n_sites = 0

   interface pp_range
      module procedure range_r8_0d, range_r8_1d, range_r8_2d, range_r8_3d
      module procedure range_r4_0d, range_r4_1d, range_r4_2d, range_r4_3d
   end interface pp_range

   interface pp_sub
      module procedure sub_r8, sub_r4
   end interface pp_sub

   interface pp_add
      module procedure add_r8, add_r4
   end interface pp_add

   interface pp_demote
      module procedure demote_0d, demote_1d, demote_2d, demote_3d
   end interface pp_demote

   interface pp_count
      module procedure count_i8, count_i4
   end interface pp_count

   interface
      function c_getpid() bind(C, name='getpid') result(pid)
         import :: c_int
         integer(c_int) :: pid
      end function c_getpid
   end interface

contains

   !> Index of the site (name, kind, type), created on first use.
   !> Call only inside the critical section.
   function find_site(name, kind, type_label) result(idx)
      character(len=*), intent(in) :: name, type_label
      integer, intent(in) :: kind
      integer :: idx
      integer :: i

      if (.not. allocated(sites)) allocate (sites(max_sites))
      do i = 1, n_sites
         if (sites(i)%kind == kind .and. sites(i)%type_label == type_label &
             .and. sites(i)%name == name) then
            idx = i
            return
         end if
      end do
      if (n_sites >= max_sites) then
         write (error_unit, '(a,i0,a)') 'precision_probe: more than ', max_sites, &
            ' sites; raise max_sites'
         error stop 1
      end if
      n_sites = n_sites + 1
      idx = n_sites
      sites(idx)%name = name
      sites(idx)%kind = kind
      sites(idx)%type_label = type_label
      if (kind == kind_range) then
         allocate (sites(idx)%hist(bin_lo:bin_hi), source=0_int64)
      else if (kind /= kind_count) then
         allocate (sites(idx)%hist(0:bit_hi), source=0_int64)
         if (kind == kind_addsub) allocate (sites(idx)%hist2(0:bit_hi), source=0_int64)
      end if
   end function find_site

   !> floor(log2(|x|)) for finite non-zero x
   pure integer function ilogb(x)
      real(dp), intent(in) :: x
      ilogb = exponent(x) - 1
   end function ilogb

   subroutine range_add(s, x)
      type(site_t), intent(inout) :: s
      real(dp), intent(in) :: x
      real(dp) :: ax
      integer :: b

      s%count = s%count + 1
      if (.not. ieee_is_finite(x)) then
         s%n_nonfinite = s%n_nonfinite + 1
         return
      end if
      s%vmin = min(s%vmin, x)
      s%vmax = max(s%vmax, x)
      if (x == 0.0_dp) then
         s%n_zero = s%n_zero + 1
         return
      end if
      if (x < 0.0_dp) s%n_negative = s%n_negative + 1
      ax = abs(x)
      s%min_abs_nz = min(s%min_abs_nz, ax)
      s%max_abs = max(s%max_abs, ax)
      if (ax < s%min_normal) s%n_subnormal = s%n_subnormal + 1
      if (x /= anint(x)) s%n_nonintegral = s%n_nonintegral + 1
      b = max(bin_lo, min(bin_hi, ilogb(ax)))
      s%hist(b) = s%hist(b) + 1
   end subroutine range_add

   subroutine range_core_r8(name, x, n)
      character(len=*), intent(in) :: name
      integer(int64), intent(in) :: n
      real(real64), intent(in) :: x(n)
      integer :: idx
      integer(int64) :: i
      !$omp critical (precision_probe_lock)
      idx = find_site(name, kind_range, 'float64')
      sites(idx)%min_normal = real(tiny(1.0_real64), dp)
      do i = 1, n
         call range_add(sites(idx), x(i))
      end do
      !$omp end critical (precision_probe_lock)
   end subroutine range_core_r8

   subroutine range_core_r4(name, x, n)
      character(len=*), intent(in) :: name
      integer(int64), intent(in) :: n
      real(real32), intent(in) :: x(n)
      integer :: idx
      integer(int64) :: i
      !$omp critical (precision_probe_lock)
      idx = find_site(name, kind_range, 'float32')
      sites(idx)%min_normal = real(tiny(1.0_real32), dp)
      do i = 1, n
         call range_add(sites(idx), real(x(i), dp))
      end do
      !$omp end critical (precision_probe_lock)
   end subroutine range_core_r4

   subroutine range_r8_0d(name, x)
      character(len=*), intent(in) :: name
      real(real64), intent(in) :: x
      call range_core_r8(name, [x], 1_int64)
   end subroutine range_r8_0d

   subroutine range_r8_1d(name, x)
      character(len=*), intent(in) :: name
      real(real64), intent(in) :: x(:)
      call range_core_r8(name, x, size(x, kind=int64))
   end subroutine range_r8_1d

   subroutine range_r8_2d(name, x)
      character(len=*), intent(in) :: name
      real(real64), intent(in) :: x(:, :)
      call range_core_r8(name, x, size(x, kind=int64))
   end subroutine range_r8_2d

   subroutine range_r8_3d(name, x)
      character(len=*), intent(in) :: name
      real(real64), intent(in) :: x(:, :, :)
      call range_core_r8(name, x, size(x, kind=int64))
   end subroutine range_r8_3d

   subroutine range_r4_0d(name, x)
      character(len=*), intent(in) :: name
      real(real32), intent(in) :: x
      call range_core_r4(name, [x], 1_int64)
   end subroutine range_r4_0d

   subroutine range_r4_1d(name, x)
      character(len=*), intent(in) :: name
      real(real32), intent(in) :: x(:)
      call range_core_r4(name, x, size(x, kind=int64))
   end subroutine range_r4_1d

   subroutine range_r4_2d(name, x)
      character(len=*), intent(in) :: name
      real(real32), intent(in) :: x(:, :)
      call range_core_r4(name, x, size(x, kind=int64))
   end subroutine range_r4_2d

   subroutine range_r4_3d(name, x)
      character(len=*), intent(in) :: name
      real(real32), intent(in) :: x(:, :, :)
      call range_core_r4(name, x, size(x, kind=int64))
   end subroutine range_r4_3d

   !> Record one addition or subtraction: cancelled leading bits of the
   !> result and exponent gap between the operands.
   subroutine addsub_record(name, type_label, a, b, res)
      character(len=*), intent(in) :: name, type_label
      real(dp), intent(in) :: a, b, res
      integer :: idx, lost, gap
      real(dp) :: big

      !$omp critical (precision_probe_lock)
      idx = find_site(name, kind_addsub, type_label)
      associate (s => sites(idx))
         s%count = s%count + 1
         if (.not. (ieee_is_finite(a) .and. ieee_is_finite(b) .and. ieee_is_finite(res))) then
            s%n_nonfinite = s%n_nonfinite + 1
         else
            big = max(abs(a), abs(b))
            if (big /= 0.0_dp) then
               if (a /= 0.0_dp .and. b /= 0.0_dp) then
                  gap = min(bit_hi, abs(ilogb(abs(a)) - ilogb(abs(b))))
                  s%hist2(gap) = s%hist2(gap) + 1
                  s%max_gap = max(s%max_gap, gap)
               end if
               if (res == 0.0_dp) then
                  s%n_total_cancel = s%n_total_cancel + 1
               else
                  lost = max(0, min(bit_hi, ilogb(big) - ilogb(abs(res))))
                  s%hist(lost) = s%hist(lost) + 1
                  if (lost > s%max_lost) then
                     s%max_lost = lost
                     s%worst_a = a
                     s%worst_b = b
                  end if
               end if
            end if
         end if
      end associate
      !$omp end critical (precision_probe_lock)
   end subroutine addsub_record

   function sub_r8(name, a, b) result(d)
      character(len=*), intent(in) :: name
      real(real64), intent(in) :: a, b
      real(real64) :: d
      d = a - b
      call addsub_record(name, 'float64', a, b, d)
   end function sub_r8

   function sub_r4(name, a, b) result(d)
      character(len=*), intent(in) :: name
      real(real32), intent(in) :: a, b
      real(real32) :: d
      d = a - b
      call addsub_record(name, 'float32', real(a, dp), real(b, dp), real(d, dp))
   end function sub_r4

   function add_r8(name, a, b) result(s)
      character(len=*), intent(in) :: name
      real(real64), intent(in) :: a, b
      real(real64) :: s
      s = a + b
      call addsub_record(name, 'float64', a, b, s)
   end function add_r8

   function add_r4(name, a, b) result(s)
      character(len=*), intent(in) :: name
      real(real32), intent(in) :: a, b
      real(real32) :: s
      s = a + b
      call addsub_record(name, 'float32', real(a, dp), real(b, dp), real(s, dp))
   end function add_r4

   !> Reference (real64) next to a real32 evaluation of the same quantity.
   subroutine demote_core(name, ref, lo, n)
      character(len=*), intent(in) :: name
      integer(int64), intent(in) :: n
      real(real64), intent(in) :: ref(n)
      real(real32), intent(in) :: lo(n)
      integer :: idx, b
      integer(int64) :: i
      real(dp) :: r, l, amp

      !$omp critical (precision_probe_lock)
      idx = find_site(name, kind_demote, 'float32')
      associate (s => sites(idx))
         s%eps_lo = real(epsilon(1.0_real32), dp)
         do i = 1, n
            r = ref(i)
            l = real(lo(i), dp)
            s%count = s%count + 1
            if (.not. (ieee_is_finite(r) .and. ieee_is_finite(l))) then
               if (ieee_is_finite(r)) then
                  s%n_lo_nonfinite = s%n_lo_nonfinite + 1
               else
                  s%n_ref_nonfinite = s%n_ref_nonfinite + 1
               end if
            else if (r == 0.0_dp) then
               if (l /= 0.0_dp) then
                  s%n_ref_zero_mismatch = s%n_ref_zero_mismatch + 1
               else
                  s%hist(0) = s%hist(0) + 1
               end if
            else
               amp = abs(l - r)/abs(r)/s%eps_lo
               s%sum_amp = s%sum_amp + amp
               s%n_amp = s%n_amp + 1
               ! bin 0: below 1 eps; bin k: [2^(k-1), 2^k) eps
               b = 0
               if (amp >= 1.0_dp) b = min(bit_hi, ilogb(amp) + 1)
               s%hist(b) = s%hist(b) + 1
               if (amp > s%max_amp) then
                  s%max_amp = amp
                  s%worst_a = r
                  s%worst_b = l
               end if
            end if
         end do
      end associate
      !$omp end critical (precision_probe_lock)
   end subroutine demote_core

   subroutine demote_0d(name, ref, lo)
      character(len=*), intent(in) :: name
      real(real64), intent(in) :: ref
      real(real32), intent(in) :: lo
      call demote_core(name, [ref], [lo], 1_int64)
   end subroutine demote_0d

   subroutine demote_1d(name, ref, lo)
      character(len=*), intent(in) :: name
      real(real64), intent(in) :: ref(:)
      real(real32), intent(in) :: lo(:)
      call check_same_size(name, size(ref, kind=int64), size(lo, kind=int64))
      call demote_core(name, ref, lo, size(ref, kind=int64))
   end subroutine demote_1d

   subroutine demote_2d(name, ref, lo)
      character(len=*), intent(in) :: name
      real(real64), intent(in) :: ref(:, :)
      real(real32), intent(in) :: lo(:, :)
      call check_same_size(name, size(ref, kind=int64), size(lo, kind=int64))
      call demote_core(name, ref, lo, size(ref, kind=int64))
   end subroutine demote_2d

   subroutine demote_3d(name, ref, lo)
      character(len=*), intent(in) :: name
      real(real64), intent(in) :: ref(:, :, :)
      real(real32), intent(in) :: lo(:, :, :)
      call check_same_size(name, size(ref, kind=int64), size(lo, kind=int64))
      call demote_core(name, ref, lo, size(ref, kind=int64))
   end subroutine demote_3d

   subroutine check_same_size(name, n_ref, n_lo)
      character(len=*), intent(in) :: name
      integer(int64), intent(in) :: n_ref, n_lo
      if (n_ref /= n_lo) then
         write (error_unit, '(3a,i0,a,i0)') 'precision_probe: pp_demote(', trim(name), &
            '): reference has ', n_ref, ' elements, low-precision array ', n_lo
         error stop 1
      end if
   end subroutine check_same_size

   subroutine count_record(name, type_label, v)
      character(len=*), intent(in) :: name, type_label
      integer(int64), intent(in) :: v
      integer :: idx
      !$omp critical (precision_probe_lock)
      idx = find_site(name, kind_count, type_label)
      sites(idx)%count = sites(idx)%count + 1
      sites(idx)%imin = min(sites(idx)%imin, v)
      sites(idx)%imax = max(sites(idx)%imax, v)
      if (v < 0) sites(idx)%n_negative = sites(idx)%n_negative + 1
      !$omp end critical (precision_probe_lock)
   end subroutine count_record

   subroutine count_i8(name, n)
      character(len=*), intent(in) :: name
      integer(int64), intent(in) :: n
      call count_record(name, 'int64', n)
   end subroutine count_i8

   subroutine count_i4(name, n)
      character(len=*), intent(in) :: name
      integer(int32), intent(in) :: n
      call count_record(name, 'int32', int(n, int64))
   end subroutine count_i4

   !> JSON number, or null for values that JSON cannot represent.
   function json_number(v) result(text)
      real(dp), intent(in) :: v
      character(len=:), allocatable :: text
      character(len=32) :: buffer
      if (ieee_is_finite(v) .and. abs(v) < huge(1.0_dp)) then
         write (buffer, '(es24.16e3)') v
         text = trim(adjustl(buffer))
      else
         text = 'null'
      end if
   end function json_number

   function json_int(v) result(text)
      integer(int64), intent(in) :: v
      character(len=:), allocatable :: text
      character(len=24) :: buffer
      write (buffer, '(i0)') v
      text = trim(buffer)
   end function json_int

   subroutine write_hist(unit, key, hist)
      integer, intent(in) :: unit
      character(len=*), intent(in) :: key
      integer(int64), intent(in) :: hist(:)
      integer :: i, lower
      logical :: first

      ! assumed-shape dummies start at 1: recover the binade / bit offset
      lower = merge(bin_lo, 0, size(hist) == bin_hi - bin_lo + 1)
      write (unit, '(4a)', advance='no') ',"', key, '":', '{'
      first = .true.
      do i = 1, size(hist)
         if (hist(i) == 0) cycle
         if (.not. first) write (unit, '(a)', advance='no') ','
         write (unit, '(5a)', advance='no') '"', json_int(int(i - 1 + lower, int64)), '":', &
            json_int(hist(i)), ''
         first = .false.
      end do
      write (unit, '(a)', advance='no') '}'
   end subroutine write_hist

   !> Write every site to the report file. Call once before the program ends;
   !> calling again rewrites the file with the statistics gathered so far.
   subroutine pp_report()
      character(len=1024) :: dir
      character(len=:), allocatable :: path
      integer :: unit, ios, i, dir_len, dir_status

      !$omp critical (precision_probe_lock)
      if (n_sites > 0) then
         call get_environment_variable('PRECISION_PROBE_DIR', dir, dir_len, dir_status)
         if (dir_status /= 0 .or. dir_len == 0) dir = '.'
         path = trim(dir)//'/precision_probe.'//json_int(int(c_getpid(), int64))//'.jsonl'
         open (newunit=unit, file=path, status='replace', action='write', iostat=ios)
         if (ios /= 0) then
            write (error_unit, '(2a)') 'precision_probe: cannot write ', path
         else
            do i = 1, n_sites
               call write_site(unit, sites(i))
            end do
            close (unit)
         end if
      end if
      !$omp end critical (precision_probe_lock)
   end subroutine pp_report

   subroutine write_site(unit, s)
      integer, intent(in) :: unit
      type(site_t), intent(in) :: s
      character(len=*), parameter :: kinds(4) = [character(len=6) :: 'range', 'addsub', 'demote', 'count']
      real(dp) :: nan_free_max

      write (unit, '(7a)', advance='no') '{"site":"', trim(escape(s%name)), '","kind":"', &
         trim(kinds(s%kind)), '","type":"', trim(s%type_label), '"'
      write (unit, '(2a)', advance='no') ',"count":', json_int(s%count)
      select case (s%kind)
      case (kind_range)
         write (unit, '(10a)', advance='no') ',"n_zero":', json_int(s%n_zero), &
            ',"n_negative":', json_int(s%n_negative), ',"n_nonfinite":', json_int(s%n_nonfinite), &
            ',"n_subnormal":', json_int(s%n_subnormal), ',"n_nonintegral":', json_int(s%n_nonintegral)
         if (s%vmin <= s%vmax) then
            write (unit, '(4a)', advance='no') ',"min":', json_number(s%vmin), ',"max":', json_number(s%vmax)
         else
            write (unit, '(a)', advance='no') ',"min":null,"max":null'
         end if
         nan_free_max = s%max_abs
         if (s%max_abs > 0.0_dp) then
            write (unit, '(4a)', advance='no') ',"min_abs_nonzero":', json_number(s%min_abs_nz), &
               ',"max_abs":', json_number(nan_free_max)
         else
            write (unit, '(a)', advance='no') ',"min_abs_nonzero":null,"max_abs":null'
         end if
         call write_hist(unit, 'binades', s%hist)
      case (kind_addsub)
         write (unit, '(8a)', advance='no') ',"n_total_cancel":', json_int(s%n_total_cancel), &
            ',"n_nonfinite":', json_int(s%n_nonfinite), ',"max_lost_bits":', &
            json_int(int(s%max_lost, int64)), ',"max_gap_bits":', json_int(int(s%max_gap, int64))
         write (unit, '(4a)', advance='no') ',"worst_a":', json_number(s%worst_a), &
            ',"worst_b":', json_number(s%worst_b)
         call write_hist(unit, 'lost_bits', s%hist)
         call write_hist(unit, 'gap_bits', s%hist2)
      case (kind_demote)
         write (unit, '(6a)', advance='no') ',"n_lo_nonfinite":', json_int(s%n_lo_nonfinite), &
            ',"n_ref_nonfinite":', json_int(s%n_ref_nonfinite), ',"n_ref_zero_mismatch":', &
            json_int(s%n_ref_zero_mismatch)
         write (unit, '(4a)', advance='no') ',"eps_lo":', json_number(s%eps_lo), &
            ',"max_amplification":', json_number(s%max_amp)
         if (s%n_amp > 0) then
            write (unit, '(2a)', advance='no') ',"mean_amplification":', &
               json_number(s%sum_amp/real(s%n_amp, dp))
         else
            write (unit, '(a)', advance='no') ',"mean_amplification":0'
         end if
         write (unit, '(4a)', advance='no') ',"worst_ref":', json_number(s%worst_a), &
            ',"worst_lo":', json_number(s%worst_b)
         call write_hist(unit, 'log2_amplification', s%hist)
      case (kind_count)
         write (unit, '(2a)', advance='no') ',"n_negative":', json_int(s%n_negative)
         if (s%imin <= s%imax) then
            write (unit, '(4a)', advance='no') ',"min":', json_int(s%imin), ',"max":', json_int(s%imax)
         else
            write (unit, '(a)', advance='no') ',"min":null,"max":null'
         end if
      end select
      write (unit, '(a)') '}'
   end subroutine write_site

   !> Quotes and backslashes would break the JSON string.
   pure function escape(text) result(clean)
      character(len=*), intent(in) :: text
      character(len=len(text)) :: clean
      integer :: i
      clean = text
      do i = 1, len(text)
         if (clean(i:i) == '"' .or. clean(i:i) == '\' .or. iachar(clean(i:i)) < 32) clean(i:i) = ' '
      end do
   end function escape

end module precision_probe
