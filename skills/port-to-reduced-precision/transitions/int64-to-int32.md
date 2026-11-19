# Transition: 64-bit → 32-bit Integers

Facts for `"int64-to-int32"`. Integer narrowing is exact or wrong: there is
no tolerance, so the work is establishing ranges and guarding them.

## Why Narrow at All

| Reason | Where it shows |
|--------|----------------|
| memory traffic of index data | sparse-matrix column indices, neighbor and connectivity lists, particle-to-cell maps: often as many bytes as the floating-point data |
| vector and device throughput | gathers and scatters with 32-bit indices, 64-bit multiply and divide on devices where they are slower |
| devices and shading languages with limited 64-bit integer support | kernels must not use them |
| atomics and hashing on 32-bit keys | cheaper and more widely available |

If none applies to a hot path, leave the integers alone and say so.

## The Limits

| Type | Range |
|------|-------|
| `int32` | −2 147 483 648 … 2 147 483 647 (about 2.1e9) |
| `uint32` | 0 … 4 294 967 295 (about 4.3e9) |
| integers held exactly in float32 / float64 | up to 2^24 / 2^53 |

Signed overflow is undefined behavior in C and C++ and not detected in
Fortran; unsigned arithmetic wraps silently. Neither announces itself.

## Classify Every 64-bit Quantity

The inventory lists the 64-bit declarations (`type_int64`, `type_size`);
`PP_COUNT` probes give their values on the workload. Assign each one a class:

| Class | Examples | Narrow? |
|-------|----------|---------|
| **local index or count** | cell index within a box, particle index within a tile, loop counter over a local array | yes: bounded by construction of the decomposition; state the bound |
| **global size or index** | total cells, total particles, global degree-of-freedom number | only with a documented maximum problem size; usually keep wide and derive local ones |
| **identifier** | particle or element ids that must be unique across the run | split into (owner, local id) or keep wide; uniqueness over the *whole run*, not one step, sets the range |
| **byte offset or size** | file offsets, allocation sizes, `size_t` arithmetic on bytes | no |
| **accumulating counter** | total steps, collisions, events summed over ranks and time | no, or prove the bound over the longest run |
| **algorithmic width** | random generator state, hashes, space-filling-curve keys, bit masks | only by switching to the 32-bit variant of the algorithm (a different generator, a shorter key): a decision |
| **product of sizes** | `nx*ny*nz`, `n*stride`, `rows*nnz_per_row` | compute in 64 bits even when every factor is 32-bit |

## Patterns

| Situation | Pattern |
|-----------|---------|
| global index needed occasionally, local index in hot loops | **base + offset**: one wide base per box / rank / block, 32-bit offsets in the arrays |
| sparse matrices | 32-bit column indices (bounded by the local column count), 64-bit row pointers (bounded by the number of non-zeros) |
| unique ids | (rank or tile, local counter) packed or stored as two 32-bit fields; widen only where ids are compared globally |
| sizes multiplied together | `widen_mul(a, b)`, or cast one factor before the product |
| a value entering a narrow field | `checked_narrow(value, "what")` once at the boundary: setup, redistribution, file read |
| space-filling-curve keys | per-block keys with fewer bits per dimension, plus the block index |
| loop counters over narrow ranges | signed 32-bit; unsigned counters wrap silently and defeat some loop optimizations |
| counts stored in floating point | integer type, or a wide accumulator: float32 stops counting at 16 777 216 |
| API boundaries taking `size_t` / default integers | convert at the call, with the check; do not let the narrow type spread into byte arithmetic |

## Analysis

1. `PP_COUNT` on every candidate, on the largest workload available;
   `scripts/probe-summary.py --int-target int32` flags `INT_OVERFLOW` and
   `INT_LITTLE_HEADROOM`.
2. For each candidate, derive the bound **analytically** from the
   configuration (grid size, particles per cell, ranks, run length). The
   probe confirms a bound; it cannot establish one.
3. Ask the user for the largest problem that must be supported. That number
   is an assumption recorded as a decision, with the guard that enforces it.

## Verification

- **Range argument** per narrowed quantity, written into the plan item.
- **Runtime guard** at the entry to the narrow representation; the program
  stops with a message naming the quantity and the limit.
- **Sanitizer run** of the tests and the correctness case:

  | Compiler | Flags |
  |----------|-------|
  | GCC, Clang | `-fsanitize=signed-integer-overflow` |
  | Clang | `-fsanitize=implicit-integer-truncation,implicit-integer-sign-change` (narrowing conversions that change the value) |
  | GCC, Clang | `-ftrapv` (abort on signed overflow, without a report) |
  | static | `-Wconversion` (GCC), `-Wshorten-64-to-32` (Clang), clang-tidy `bugprone-narrowing-conversions`, `bugprone-implicit-widening-of-multiplication-result`, `bugprone-too-small-loop-variable` |

  Check a flag against the installed compiler before relying on it. A clean
  sanitizer run covers the executed paths of that workload only.
- **Exact QoIs**: counts and integer results compared with `"exact": true`.
- **A deliberately oversized test**: run a small configuration whose
  *declared* sizes exceed the limit and check that the guard fires, instead
  of trusting that it would.

## Decisions

Narrowing a global quantity, switching a generator or key width, and the
maximum supported problem size are decisions (triggers 2 and 6). Present
them with the memory traffic saved, measured, and with what happens when the
limit is exceeded: a clear stop at setup, never a wrong result.
