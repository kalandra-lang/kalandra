# Guarded private-array reductions — 2026-10-27

Both final warm-batch runs meet the 10% target in **29/32 cases**, including all
20 array cases. The remaining misses are the one-round affine, polynomial and
Euclidean-GCD cases. The original eight-case ordinary control also qualifies.
This is not universal C++/Rust parity.

Paired comparisons against the frozen pre-change compiler show approximately
**3.0–3.7× faster zero-round sums** for 32–128-element arrays and **1.6–2.0× faster
one-round maps**. Long maps see smaller gains: summation is a small part of their
work. Results are for ordinary CPU execution on this Apple M2 Max/macOS 27.2
host; they do not qualify other CPUs, operating systems or physical macOS 26.

## Implementation and correctness boundary

[OrdinaryReductions.h](../../native/lib/OrdinaryReductions.h) recognizes an
isolated, constant-trip, read-only int64 sum over private storage. It masks terms
to unsigned 32 bits and independently accumulates discarded high bits. At most
65,536 bounded terms cannot overflow int64. The exit accepts only a zero witness;
otherwise it musttail-replays the original unrestricted worker using the original
scalar arguments and fault pointer. It never returns an unvalidated masked sum.

A whole-region extent guard prevents extra reads past the allocation after an
original early arithmetic fault. Addresses must have an invariant private base
and exactly eight-byte induction. Speculative loads drop UB-implying metadata
and alignment promises and freeze their values. Exact instruction recognition
rejects additional arithmetic, effects, phis and data-dependent addressing.

[OrdinaryInputs.h](../../native/lib/OrdinaryInputs.h) can guard an internal
small-argument version when that materially simplifies an expensive reduction.
The public ABI remains unrestricted. Both transformations retain the existing
8 KiB storage and conservative 16 KiB frame budgets. Replay releases the private
frame; error locations, ownership, allocation-failure behavior and cancellable
entries retain their checked implementations. See the [proof and eligibility
rules](../../docs/design/checked-call-optimization.md#guarded-private-array-reductions).

## Measurement and independent verification

The source algorithms, per-program entries, drivers, C++/Rust flags and numerical
contracts are unchanged. All three languages execute the same source-level
algorithms; references may optimize and allocate normally. Floating operations
remain strict binary64, without FMA or reassociation. Every call validates exact
returned bits; integer bit checksums wrap modulo 2^64.

The earlier protocol capped calibration at 20,000 calls and estimated cost from
one cold call. Some scalar batches lasted only tens of microseconds and results
crossed the acceptance threshold between repeats. The primary results below use
adaptive warm calibration, up to 20 million calls, for **every language and case**.
Calibration grows through sub-resolution zero-duration probes; measured batches
must have positive duration. The faster member determines shared paired counts.
All calibration samples are retained and independently audited. Legacy callers
keep the old default; the original control explicitly opts into warm batching.
Deterministic convergence, pairing, timer-resolution, cap and legacy controls
are [recorded here](reduction-ranges-calibration-check-2026-10-27.log).

Each complete run has three independently launched processes per cell, two warm
batches and seven measured batches. The paired study has nine ABBA/BAAB blocks,
with equal call counts within each pair. Compiler/runtime and generated artifacts
are frozen and hashed. No build, test or profiler overlaps any measured run.
There is no CPU affinity or frequency isolation. Primary measured batches were
at least 7.4 ms across the two complete runs.

The normal oracle uses Decimal rounding after each floating operation and exact
integer/GCD operations. An independent audit uses Fraction rounding, modular
exponentiation and divisor enumeration, checks every calibration/warm/measured
checksum, recomputes medians and validates artifact hashes. Each complete run
also has 360 seed/round verification workloads.

[First warm run](reduction-ranges-adaptive-final-2026-10-27.json),
[repeat](reduction-ranges-adaptive-repeat-2026-10-27.json),
[paired raw data](reduction-ranges-adaptive-paired-2026-10-27.json),
[independent audit](reduction-ranges-adaptive-audit-2026-10-27.json),
[original control](reduction-ranges-adaptive-control-2026-10-27.json),
[control audit](reduction-ranges-control-audit-2026-10-27.json).

## Complete equivalent-source corpus

Times are median nanoseconds per invocation. Gap is against the faster C++/Rust
reference; negative means Kalandra is faster. The last column independently
recomputes the gap in the repeat. All 32 cases remain in acceptance.

| Program | Rounds | Kalandra ns | C++ ns | Rust ns | Gap | Repeat gap |
|---|---:|---:|---:|---:|---:|---:|
| affine | 0 | 1.180 | 1.155 | 1.175 | +2.2% | +0.6% |
| affine | 1 | 1.204 | 0.903 | 0.892 | +34.9% | +32.7% |
| affine | 16 | 7.678 | 7.626 | 7.674 | +0.7% | +1.5% |
| affine | 256 | 376.050 | 372.667 | 375.347 | +0.9% | +1.1% |
| polynomial | 0 | 1.182 | 1.163 | 1.153 | +2.5% | -1.2% |
| polynomial | 1 | 1.203 | 0.902 | 0.910 | +33.3% | +35.9% |
| polynomial | 16 | 7.679 | 7.587 | 7.662 | +1.2% | +2.9% |
| polynomial | 256 | 377.515 | 375.828 | 376.420 | +0.4% | +4.8% |
| euclid | 0 | 1.184 | 1.169 | 1.183 | +1.3% | +2.0% |
| euclid | 1 | 4.031 | 3.284 | 4.067 | +22.8% | +25.1% |
| euclid | 16 | 163.836 | 158.465 | 159.568 | +3.4% | +4.6% |
| euclid | 256 | 3213.576 | 3162.000 | 3220.219 | +1.6% | +0.8% |
| array8 | 0 | 6.980 | 21.462 | 22.263 | -67.5% | -69.7% |
| array8 | 1 | 11.810 | 52.283 | 51.154 | -76.9% | -74.6% |
| array8 | 16 | 127.216 | 476.602 | 443.255 | -71.3% | -68.9% |
| array8 | 256 | 1989.115 | 7793.823 | 7321.358 | -72.8% | -73.1% |
| array16 | 0 | 13.850 | 26.016 | 24.270 | -42.9% | -43.7% |
| array16 | 1 | 28.946 | 63.513 | 60.262 | -52.0% | -51.6% |
| array16 | 16 | 262.316 | 580.174 | 571.650 | -54.1% | -54.3% |
| array16 | 256 | 3891.834 | 9548.828 | 9049.521 | -57.0% | -57.0% |
| array32 | 0 | 9.289 | 28.902 | 26.056 | -64.3% | -63.5% |
| array32 | 1 | 34.769 | 73.326 | 64.873 | -46.4% | -46.5% |
| array32 | 16 | 473.910 | 737.484 | 654.715 | -27.6% | -27.3% |
| array32 | 256 | 7469.776 | 11975.049 | 11134.126 | -32.9% | -33.8% |
| array64 | 0 | 20.949 | 39.900 | 31.646 | -33.8% | -32.8% |
| array64 | 1 | 80.844 | 109.190 | 100.949 | -19.9% | -20.1% |
| array64 | 16 | 933.194 | 1141.064 | 1095.763 | -14.8% | -14.4% |
| array64 | 256 | 14640.803 | 18164.701 | 17717.759 | -17.4% | -16.6% |
| array128 | 0 | 49.161 | 62.020 | 53.841 | -8.7% | -7.4% |
| array128 | 1 | 156.778 | 188.869 | 167.204 | -6.2% | -6.0% |
| array128 | 16 | 1790.683 | 2073.186 | 1937.813 | -7.6% | -7.2% |
| array128 | 256 | 27934.306 | 34573.573 | 32359.098 | -13.7% | -14.4% |

## Paired changes against the pre-change compiler

The baseline is the compiler frozen at the start of this work on `native`, Git
base `b73ce17950bb362ff87c081410fe483d79a74b47`. Source signatures and algorithms
match in both artifact sets. These gains use the same warm protocol on both
versions, rather than comparing different historical batch policies. Values
below are medians of the nine within-block speedup ratios.

| Width | Zero rounds | One round | 16 rounds | 256 rounds |
|---|---:|---:|---:|---:|
| 32 | 3.699× | 2.048× | 1.322× | 1.280× |
| 64 | 3.344× | 1.589× | 1.056× | 0.996× |
| 128 | 2.995× | 1.642× | 1.054× | 1.005× |

For example, the 64-element 256-round case is 0.996× baseline: that small loss
is retained. This optimization primarily removes startup and final-reduction
checks; it does not accelerate every map iteration.

## Safety regression coverage

The proof unit adds 19 reduction cases and three guarded-input cases, alongside
its existing ordinary-entry, IEEE/countdown, pool-budget and ABI controls.
Rejections include effects, external memory, incorrect overflow bounds,
non-unit/zero/excessive counts, extra phis/arithmetic, data-dependent addresses,
regions larger than storage and unsupported pointer widths. A positive control
checks that load metadata promises do not leak into speculation.

The four-build source matrices cover 1,744 scalar/fault cases, 1,968 strict
recurrence/countdown cases, 16,896 private-buffer/replay cases and 64,800 new
reduction cases. The latter include int64, float64 and Boolean returns; signed
extremes, 16/32-bit guard boundaries, intermediate overflow, independent wide
accumulation, exact first-fault positions and six allocation-failure settings.
Debug, optimized, ASan/UBSan and TSan modes compare the original checked entry
with the ordinary entry and independent arithmetic oracles.

The [instrumented replay diagnostic](reduction-ranges-replay-2026-10-27.json)
checks 28 cases across widths 32/128: bounded data has zero range replays, rejected data
has one range replay, and every replay receives the exact original seed and rounds.
Those binaries are separate from every timed binary.

A prototype reused a replay block after simplification had specialized its
round count to zero. The existing negative map test caught the wrong result:
32 elements, seed -31, one round returned -496 instead of -6224. Fresh blocks per
reduction fix it, and the diagnostic covers rounds 0, 1 and 2. No result from
that rejected implementation is used for acceptance.

All **425 selected CPU regressions and 50 selected Metal tests passed**, with
no failures or skips. Frozen native sources and compiler/runtime hashes remained
unchanged through qualification. The full native build and focused matrices
also passed before measurements. The suite covers arithmetic/numerical behavior,
first faults, aliases, ownership, stack admission, task cancellation and managed
execution across debug, optimized, ASan/UBSan and TSan builds.

[Qualification manifest](reduction-ranges-qualification-2026-10-27.json),
[CPU log](reduction-ranges-cpu-2026-10-27.log),
[Metal log](reduction-ranges-metal-2026-10-27.log),
[full build](reduction-ranges-build-2026-10-27.log),
[focused checks](reduction-ranges-focused-2026-10-27.log).

## Retained exploratory measurements

The [reduction-only probe](reduction-ranges-probe-2026-10-27.json) and its
[repeat](reduction-ranges-repeat-probe-2026-10-27.json) predate the replay fix;
although their positive benchmark inputs validate, the compiler was incorrect
for other valid inputs. They are rejected candidates. The [combined
probe](reduction-ranges-combined-probe-2026-10-27.json) predates final memory and
musttail tightening. These are exploratory controls, not qualified results;
mutable exploratory compiler binaries were not retained.

The final-code [short-batch run](reduction-ranges-final-2026-10-27.json),
[repeat](reduction-ranges-repeat-2026-10-27.json),
[paired study](reduction-ranges-paired-2026-10-27.json) and
[audit](reduction-ranges-audit-2026-10-27.json) retain the measurement-policy
investigation. Each complete short-batch run met 28/32, with different borderline
scalar misses. They are not substituted for the warm-batch primary studies.

## Remaining bottleneck

The [scalar IR control](reduction-ranges-scalar-control-2026-10-27.json) confirms
that all three scalar modules are unchanged from baseline after normalizing only
the source filename. This array optimization does not change their generated code.

The one-round affine/polynomial/GCD misses remain about 23–36% above the faster
reference across the two warm runs. Their fixed arithmetic, dispatch and fault
ABI costs dominate such tiny calls. Array reduction improvements do not remove
that scalar entry cost. The next investigation is the scalar success/error ABI
and specialization profitability, preserving negative-count and arithmetic
failure behavior. No broader performance target is marked complete.
