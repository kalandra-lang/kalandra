# Broader ordinary CPU qualification — 2026-10-27

The expanded corpus exposes limits hidden by the earlier eight cases. The final
compiler meets the within-10% target in **24 of 32 cases in both complete runs**.
All 16- and 256-iteration cases qualify. **Eight short cases still miss**: one
iteration of affine/polynomial/Euclid work, zero iterations for arrays of 32/64/128
elements, and one iteration for arrays of 64/128 elements. The broader target is
not complete. The worst remaining gap is 128-element zero-round initialization
and reduction (about 172–177% slower than the faster reference).

Against frozen native `542d78be`, nine paired blocks show 23.5–25.2% less time for
the 16-iteration floating recurrences and 26.2–84.9% less time across the new
16–128-element array cases. The original eight-case control still meets its
within-10% target (worst gap 3.1%). Neither statement establishes universal
superiority over C++ or Rust.

Host: Apple M2 Max, macOS 27.2; deployment target macOS 26. These timings do not
qualify physical macOS 26, Linux or Windows performance. This is a CPU study;
Metal tests below check regressions, not new GPU speedups.

## Broader comparison and numerical controls

New programs include two strict binary64 recurrences and repeated Euclidean GCD.
Immutable fresh-array maps now cover 8, 16, 32, 64 and 128 elements with a linear
functional reduction at every size. C++/Rust use the same algorithms, fresh
vectors and ordinary optimization. Each program has its own external entry:
there is no extra reference-only workload dispatch in the timed call. No
numerical library or forced allocation/inlining restriction is introduced.

Every language uses the same external driver, separate compilation and no LTO.
Every return bit must match the reference. The checksum sums result bit patterns
with defined unsigned 64-bit wrapping, avoiding floating checksum accumulation
rounding. Decimal operations with explicit per-operation binary64 rounding provide
the primary floating oracle. The independent audit instead uses exact Fraction
arithmetic and rounding; arrays use modular powers rather than repeated maps,
and GCD uses divisor enumeration rather than Euclid. Three seeds and five sizes
supply 360 verification workloads per complete expanded run. NaN/Inf and negative
counter behavior are separately covered by source tests.

Durations are ns/call. Gap compares Kalandra to the faster reference within the
same run; negative means faster. All cases, including misses, are retained.

| Program | Rounds | Kalandra | C++ | Rust | Gap | Repeat gap |
|---|---:|---:|---:|---:|---:|---:|
| affine | 0 | 1.175 | 1.175 | 1.175 | +0.0% | -0.2% |
| affine | 1 | 1.083 | 0.883 | 0.881 | +22.9% | +33.6% |
| affine | 16 | 7.885 | 7.792 | 7.521 | +4.8% | +1.2% |
| affine | 256 | 360.133 | 357.720 | 358.999 | +0.7% | +0.7% |
| polynomial | 0 | 1.083 | 1.083 | 1.175 | +0.0% | +1.9% |
| polynomial | 1 | 1.083 | 0.881 | 0.817 | +32.7% | +32.7% |
| polynomial | 16 | 7.888 | 7.737 | 7.177 | +9.9% | +1.8% |
| polynomial | 256 | 361.917 | 359.060 | 358.802 | +0.9% | +0.6% |
| euclid | 0 | 1.085 | 1.087 | 1.177 | -0.2% | +1.4% |
| euclid | 1 | 3.958 | 2.992 | 4.696 | +32.3% | +14.8% |
| euclid | 16 | 157.771 | 149.369 | 151.383 | +5.6% | +5.4% |
| euclid | 256 | 3091.305 | 3036.308 | 3119.585 | +1.8% | +1.8% |
| array8 | 0 | 6.756 | 22.302 | 21.779 | -69.0% | -69.3% |
| array8 | 1 | 11.523 | 51.181 | 45.333 | -74.6% | -74.8% |
| array8 | 16 | 123.744 | 466.651 | 433.204 | -71.4% | -71.8% |
| array8 | 256 | 1941.659 | 7705.411 | 7223.499 | -73.1% | -72.9% |
| array16 | 0 | 13.506 | 25.129 | 23.683 | -43.0% | -43.4% |
| array16 | 1 | 28.121 | 62.123 | 53.202 | -47.1% | -52.2% |
| array16 | 16 | 256.363 | 569.737 | 553.679 | -53.7% | -48.8% |
| array16 | 256 | 3786.322 | 9342.195 | 8794.509 | -56.9% | -56.1% |
| array32 | 0 | 33.763 | 28.071 | 25.073 | +34.7% | +35.1% |
| array32 | 1 | 69.979 | 72.106 | 72.017 | -2.8% | -2.0% |
| array32 | 16 | 624.764 | 723.083 | 703.062 | -11.1% | -11.9% |
| array32 | 256 | 9325.827 | 11663.450 | 11352.524 | -17.9% | -18.4% |
| array64 | 0 | 69.033 | 38.727 | 30.319 | +127.7% | +127.3% |
| array64 | 1 | 126.381 | 105.731 | 99.419 | +27.1% | +28.3% |
| array64 | 16 | 968.829 | 1119.406 | 1074.166 | -9.8% | -10.1% |
| array64 | 256 | 14410.473 | 17925.664 | 17304.945 | -16.7% | -17.5% |
| array128 | 0 | 144.169 | 60.542 | 53.085 | +171.6% | +176.7% |
| array128 | 1 | 253.108 | 185.573 | 163.852 | +54.5% | +61.9% |
| array128 | 16 | 1852.967 | 2006.292 | 1864.665 | -0.6% | -2.2% |
| array128 | 256 | 27454.472 | 33759.860 | 31419.025 | -12.6% | -14.1% |

[Final observations](broad-ordinary-final-2026-10-27.json),
[independent repeat](broad-ordinary-repeat-2026-10-27.json),
[baseline](broad-ordinary-before-2026-10-27.json),
[primary audit](broad-ordinary-primary-audit-2026-10-27.json),
[repeat audit](broad-ordinary-repeat-audit-2026-10-27.json),
[original-corpus control](broad-ordinary-original-control-2026-10-27.json),
[control audit](broad-ordinary-control-audit-2026-10-27.json).
The independent audit also rejects four deliberately corrupted evidence controls:
[checksums, medians, sample order and source hashes](broad-ordinary-audit-self-check-2026-10-27.json).

## Accepted compiler changes

The ordinary-entry buffer layout sizes four aligned slots for the largest
constant allocation in that entry, with a minimum stride of 128 bytes. Storage
is capped at 8 KiB, and the complete entry must satisfy the existing conservative
16 KiB native frame budget, including all SSA values, fixed allocations and
outgoing argument copies. For example, width 128 uses four 1088-byte slots.
Graphs outside those limits retain checked heap execution. Allocation-failure
injection and exhaustion still replay from original scalar arguments. Nothing
changes the language's allocation limits, escaping-resource rules, Invocation
layout or task/cancellation path.

A three-slot/2-KiB prototype replayed through the heap starting at the second
64-element map; treating it as steady local execution would misdescribe the
result. [Instrumented diagnostics](broad-ordinary-replay-diagnostic-2026-10-27.json)
show zero heap replays at the tested counts for the accepted four-slot 64/128
layouts. Instrumented durations are not performance evidence. The
[three-slot](broad-ordinary-adaptive-2026-10-27.json),
[scalar-proof](broad-ordinary-scalar-probe-2026-10-27.json), and
[four-slot](broad-ordinary-four-slot-probe-2026-10-27.json) exploratory controls
remain recorded separately. Their intermediate compiler files were overwritten;
only baseline/final primary artifacts are fully revalidated by the artifact audit.

For an ordinary scalar two-block countdown, a structural proof checks that its
initial value is an entry argument, its sole backedge subtracts one, and zero
exits before the next subtraction. A nonnegative guard then permits removing
the signed-minimum case from that version. Negative inputs keep the original
ordinary worker and fault location. Tasks and buffered entries are excluded.

A conservative IEEE proof removes NaN classification only where NaN cannot
occur. It follows supported constants, integer-to-float conversions, selected
casts and inductive phi/select graphs. Products with finite nonzero constants
preserve absence of NaN; addition/subtraction requires a finite operand. Unknown
floating arguments and unsupported operations remain unknown; zero times infinity
and explicit NaN origins reject the proof. No fast-math flags, reassociation,
FMA contraction or infinity exclusion are introduced. Values and valid NaN
payloads remain governed by the existing semantics. See the
[design proof boundaries](../../docs/design/checked-call-optimization.md).

## Paired before/after measurements

The [nine-block study](broad-ordinary-paired-2026-10-27.json) retains all 1,152
fresh-process measurements, adjacent ABBA/BAAB order, matching iterations within
each pair, exact-bit oracles and source/artifact hashes. Source/reference/driver
hashes must match. No build, test or profiler overlapped acceptance timing; CPU
frequency, affinity and desktop activity were not controlled.

Timings are medians of process medians. The final column is the median paired
time change and need not equal the ratio of those aggregate medians.

| Program | Rounds | Previous ns | Current ns | Paired time change |
|---|---:|---:|---:|---:|
| affine | 0 | 1.179 | 1.175 | +0.4% |
| affine | 1 | 1.470 | 1.175 | -21.1% |
| affine | 16 | 10.300 | 7.881 | -23.5% |
| affine | 256 | 386.413 | 362.857 | -6.7% |
| polynomial | 0 | 1.175 | 1.175 | -0.0% |
| polynomial | 1 | 1.469 | 1.175 | -20.0% |
| polynomial | 16 | 10.496 | 7.881 | -25.1% |
| polynomial | 256 | 386.700 | 360.343 | -7.0% |
| euclid | 0 | 1.175 | 1.175 | -0.1% |
| euclid | 1 | 3.957 | 3.960 | -0.1% |
| euclid | 16 | 156.657 | 157.077 | +0.2% |
| euclid | 256 | 3096.341 | 3094.654 | -0.4% |
| array8 | 0 | 6.754 | 6.754 | -0.0% |
| array8 | 1 | 11.523 | 11.527 | +4.0% |
| array8 | 16 | 124.499 | 123.858 | -0.3% |
| array8 | 256 | 1940.958 | 1945.571 | +0.1% |
| array16 | 0 | 87.752 | 13.500 | -84.9% |
| array16 | 1 | 127.528 | 28.052 | -78.1% |
| array16 | 16 | 469.412 | 255.923 | -45.2% |
| array16 | 256 | 5493.879 | 3811.461 | -30.4% |
| array32 | 0 | 111.554 | 33.742 | -69.6% |
| array32 | 1 | 164.218 | 69.741 | -57.5% |
| array32 | 16 | 959.338 | 621.989 | -35.1% |
| array32 | 256 | 13468.078 | 9336.720 | -30.6% |
| array64 | 0 | 160.267 | 69.134 | -56.9% |
| array64 | 1 | 243.544 | 127.205 | -47.6% |
| array64 | 16 | 1496.349 | 968.108 | -35.2% |
| array64 | 256 | 21141.369 | 14422.838 | -32.0% |
| array128 | 0 | 257.962 | 144.326 | -44.2% |
| array128 | 1 | 398.420 | 253.082 | -36.6% |
| array128 | 16 | 2570.655 | 1857.657 | -27.7% |
| array128 | 256 | 37075.570 | 27353.244 | -26.2% |

## Remaining bottlenecks

Short scalar cases retain the cost of checked signed arithmetic and the guard
that preserves defined negative-count failure. The absolute differences are
small, but the relative misses are real and are not removed from acceptance.
Wide-array startup/reduction still executes checked initialization and summation;
removing heap traffic does not prove their arithmetic checks redundant. The next
architectural investigation is propagation of proven numeric bounds through
private arrays and reductions, with guarded entry versions and a fully checked
fallback. Further buffer resizing alone does not address those checks.

## Final regression qualification

The final native build completed, and all **424 selected CPU regressions and
50 selected Metal tests passed**, with no failures or skips. Compiler/runtime
binaries and the qualified input files stayed unchanged through measurements
and qualification. The new proof unit covers nine IEEE/countdown cases, typed
float32/float64 paths, negative-version retention, rejection cases, budget
constraints, idempotence and preserved ABI attributes.

The four-build source matrices cover **1,744 scalar/fault/payload cases**,
**1,968 strict recurrence/countdown cases**, and **13,824 buffer/alias/replay
cases**. Recurrence inputs include negative counts at/near `INT64_MIN`, signed
zeros, subnormals, maximum finite values, infinities, quiet/signaling NaNs and
valid floating overflow. Buffers cover 16–128-element maps, aliases, extremes,
pool exhaustion and eight allocation-failure settings. Debug, optimized,
ASan/UBSan and TSan builds retain the checked-path and independent oracles.

[Qualification manifest](broad-ordinary-qualification-2026-10-27.json),
[CPU log](broad-ordinary-cpu-2026-10-27.log),
[Metal log](broad-ordinary-metal-2026-10-27.log). Correctness tests followed all
acceptance timings. Remaining performance misses above are still open.
