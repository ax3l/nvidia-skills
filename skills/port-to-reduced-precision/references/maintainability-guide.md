# Maintainability Guide

A port that works but that nobody can read will be undone by the next
contributor who "simplifies" a stable formula back into the textbook one, or
adds a `double` local in a single-precision kernel. These conventions keep a
large code base portable across precisions without special knowledge at
every line. Language syntax is in the flavor guides; this file is the rules.

## One Place Defines Precision

- A single header or module defines the **role aliases** and how
  `PRECISION_PROFILE` (through the build system) selects them. It is the
  only place that names a concrete floating-point type for role data.
- **Roles, not variables.** Precision is a property of a kind of quantity
  ("particle data", "field data", "accumulators", "time", "indices"), chosen
  once. Two to five roles cover most codes. Per-variable tuning, the output
  of automatic precision tuners, is unmaintainable: nobody can say why
  variable 4711 is single, and every edit invalidates the tuning.
- If a role must be wide in every profile (time, global diagnostics), say so
  in its definition instead of hard-coding `double` at the use sites.
- Name aliases for the role (`ParticleReal`), not for the current type
  (`float_t`).

## Kernels Do Not Know Their Precision

- No concrete floating-point type in numerical code: declarations use role
  aliases, literals carry the role's type, math calls are type-generic.
- **Mostly the same statements at every precision**, also where a repair
  costs the original precision a little time or moves its last digits.
  Bit-identical original-precision results are a requirement only if the
  developer set one.
- **No precision `#ifdef` inside kernels.** Where the two builds really need
  different statements, a small, readable helper that branches on the type
  is the place for it: select once, through the type's traits, name the
  helper for what it does, and write down why. Use it when it is needed
  (typically to keep the original precision fast), not by default, and not
  to preserve the last digits of the original-precision results (the flavor guides show the pattern). Every
  such fork is code the `hi` build does not run: the helper's test
  instantiates both branches in every build, or the unused one rots.
- A function that converts between roles does so explicitly and visibly,
  at its start or end, never in the middle of an expression.
- New code follows the same rules. The cheapest enforcement is the
  compiler: keep the conversion warnings of the flavor guide on (as errors
  where the project can afford it) for the `lo` build in continuous
  integration, and run the FP64 audit there.

## Boundaries Are Named

Precision is fixed from outside at file formats, MPI datatypes, library
calls and language interop. Convert **once**, in a function whose name says
it is a boundary, and keep the conversion out of loops. A reader should be
able to list every place where the working precision meets a fixed one by
searching for a handful of function names, not for casts.

Decide and document per boundary which side owns the precision
(`io_policy`): a checkpoint written at one precision and read at another
changes results at the restart, which is fine only if it is intended.

## Stable Forms Have Names

The textbook formula is what every reader knows. The stable form is what
the port needs. Bridge the gap in two ways, always both:

1. **A named helper** for each stable form (`sqrt1pm1`, `diff_of_squares`,
   `CompensatedSum`, `cell_index_clamped`), collected in one small library
   (`assets/stable-math/` as a starting point, renamed into the project's
   namespace). A helper documents itself, is tested once, and is fixed once.
   An inlined trick is rediscovered, misunderstood and half-copied. Take the
   library whole rather than trimming it to what the port uses today: the
   unused templates cost nothing, and the shipped self-test then runs
   unchanged with the project's own compiler flags.
2. **A comment at the use site** with the three things a future maintainer
   needs to not undo it:

   ```cpp
   // Kinetic energy factor (gamma - 1). Written as sqrt1pm1(u^2/c^2):
   // the direct difference loses all digits in single precision for
   // u/c below about 3e-4 (and 8 digits in double). Verified for
   // u/c in [1e-6, 1e3]; see precision_tests/test_kinetic_energy.
   const Real gamma_minus_one = sqrt1pm1(u2 * inv_c2);
   ```

   What it computes in textbook terms; why the textbook form was not used,
   with the regime; where it was verified and for what range.

Comments describe the code as it is. History ("changed from ...") belongs in
the commit message and the decision record.

## Tolerances Say What They Mean

- A numerical tolerance is written as a multiple of the type's epsilon and
  the relevant scale, through one helper, so that it follows the precision.
- A physical tolerance (an accuracy the user asked for) stays a number, with
  a comment saying so, and is checked for attainability at `lo`.
- Sentinels use the type's limits, never a literal outside the target range.
- Loops are counted in integers.

## Tests Travel With the Code

- Each repaired function keeps its standalone test in the repository,
  wired into the project's test runner for **both** profiles. The frozen
  reference copy is part of the test and is marked as such.
- The helper library's self-test runs with the project's own compiler flags,
  so that a flag change that defeats compensation is caught by a failing
  test rather than by a wrong result.
- The `lo` build and its QoI gate run in continuous integration next to the
  `hi` build. A precision the project does not test is a precision it does
  not support.

## Readable Diffs

- Mechanical genericity (Phase 3) and numerical repairs (Phase 4) are
  separate commits, so that a reviewer can check "no value changed" for the
  first kind and "this formula changed, here is the evidence" for the
  second.
- One repair per commit, with the test.
- Do not reformat or rename beyond what the change needs.

## Review Checklist

Before an item is marked complete:

- [ ] No concrete floating-point type, unsuffixed literal or
      precision-specific math call was added in numerical code.
- [ ] No precision `#ifdef` in a kernel; any precision-dependent selection
      is in one named place with a reason.
- [ ] The stable form is a named helper; the use site has the three-part
      comment.
- [ ] Conversions happen at a named boundary, outside loops.
- [ ] Tolerances and sentinels follow the type.
- [ ] The standalone test runs in both profiles and is wired into the
      project's tests.
- [ ] Assumptions are in `decisions.json` and, where possible, checked at
      run time.
- [ ] The diff contains nothing unrelated.
