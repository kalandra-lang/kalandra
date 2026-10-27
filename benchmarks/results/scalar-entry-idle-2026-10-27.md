# Scalar entry: idle-host measurements — 2026-10-27

The frozen candidate still meets the 10% C++/Rust target in **29/32 cases** in
both complete runs. The baseline also meets 29/32. All 20 array cases qualify;
the one-round affine, polynomial and Euclidean-GCD cases remain outside target.
The candidate therefore does **not** close the remaining performance gap.

The user confirmed Docker was idle before this study. No build, correctness
test or profiler overlapped measurements. The compiler, runtime, source and
program artifact hashes were verified before measurement and independently
audited afterward. These measurements supersede the provisional probes taken
before the Docker-load warning; no earlier sample is substituted for these runs.

## What changed

The candidate combines private nonzero argument facts, an optional explicit
value-bits/error-code return ABI, and guarded exact floating algebra at loop
entry. Scalar comparisons select the explicit-status companion; arrays retain
the previously qualified ordinary ABI. The algorithms, input values and C++/Rust
flags are unchanged. Checked arithmetic and recoverable first-fault metadata
remain. The candidate source remains uncommitted; full CPU/Metal regression
qualification and any decision to promote the new ABI to the CLI are pending.

## Protocol and audits

Each full comparison covers eight programs at 0/1/16/256 rounds, with three
languages, three fresh processes per cell, two warm batches and seven measured
batches. Warm adaptive calibration targets 10 ms and permits up to 20 million
calls. A separate study compares baseline/candidate through nine adjacent
ABBA/BAAB blocks with identical counts within each pair. No frequency or CPU
affinity isolation is imposed.

Every call checks exact return bits and a wrapping integer checksum. Each full
run additionally verifies 360 input workloads. Independent Fraction rounding,
modular-power and divisor oracles validated **25,146 calibration, warmup and
measured samples** across the four studies, recomputed statistics and checked
artifact hashes. All checks passed. This is validation of these measurements,
not a substitute for the pending full regression suite.

[Baseline](scalar-entry-idle-before-2026-10-27.json),
[candidate](scalar-entry-idle-final-2026-10-27.json),
[repeat](scalar-entry-idle-repeat-2026-10-27.json),
[paired study](scalar-entry-idle-paired-2026-10-27.json),
[independent audit](scalar-entry-idle-audit-2026-10-27.json).

## Remaining misses

Gap is against the faster C++/Rust reference in that run.

| One-round program | First gap | Repeat gap | Paired change versus baseline |
|---|---:|---:|---:|
| affine | +34.89% | +37.06% | +0.35% time |
| polynomial | +33.57% | +31.35% | -0.62% time |
| euclid | +17.47% | +18.54% | -5.37% time |

The floating one-round changes are effectively flat. GCD takes about **5.4% less
time**, but remains 17–19% behind the faster reference. The floating 16-round
cases take about 5% less time. The current arithmetic specialization helps some
workloads but does not remove the fixed dispatch/error-handling cost of tiny
calls. Further changes should address that cost rather than claim the target
has been reached.

## Complete corpus

Median nanoseconds per invocation; negative gaps mean Kalandra is faster.
All cases, including misses, remain in acceptance.

| Program | Rounds | Baseline Kalandra ns | Candidate ns | C++ ns | Rust ns | Candidate gap | Repeat gap |
|---|---:|---:|---:|---:|---:|---:|---:|
| affine | 0 | 1.170 | 1.160 | 1.139 | 1.184 | +1.88% | +3.48% |
| affine | 1 | 1.198 | 1.204 | 0.905 | 0.892 | +34.89% | +37.06% |
| affine | 16 | 7.646 | 7.295 | 7.570 | 7.702 | -3.63% | -3.86% |
| affine | 256 | 373.633 | 370.962 | 383.017 | 371.941 | -0.26% | +0.01% |
| polynomial | 0 | 1.166 | 1.163 | 1.165 | 1.150 | +1.15% | -2.42% |
| polynomial | 1 | 1.206 | 1.184 | 0.902 | 0.886 | +33.57% | +31.35% |
| polynomial | 16 | 7.765 | 7.304 | 7.535 | 7.462 | -2.12% | +0.30% |
| polynomial | 256 | 374.788 | 370.412 | 372.203 | 374.930 | -0.48% | -1.86% |
| euclid | 0 | 1.166 | 1.145 | 1.155 | 1.191 | -0.92% | +2.31% |
| euclid | 1 | 4.050 | 3.785 | 3.223 | 3.888 | +17.47% | +18.54% |
| euclid | 16 | 163.144 | 162.577 | 157.719 | 158.310 | +3.08% | +3.14% |
| euclid | 256 | 3270.262 | 3275.326 | 3188.058 | 3162.852 | +3.56% | +0.01% |
| array8 | 0 | 6.853 | 6.892 | 21.183 | 22.077 | -67.46% | -69.04% |
| array8 | 1 | 11.920 | 11.775 | 52.875 | 47.717 | -75.32% | -75.72% |
| array8 | 16 | 128.238 | 126.328 | 473.578 | 445.336 | -71.63% | -71.69% |
| array8 | 256 | 1945.388 | 1974.775 | 7709.432 | 7331.119 | -73.06% | -73.17% |
| array16 | 0 | 13.386 | 13.553 | 25.964 | 24.170 | -43.93% | -43.62% |
| array16 | 1 | 28.863 | 28.488 | 63.453 | 54.877 | -48.09% | -51.84% |
| array16 | 16 | 258.783 | 260.701 | 579.943 | 508.885 | -48.77% | -48.98% |
| array16 | 256 | 3892.124 | 3861.907 | 9558.838 | 9054.524 | -57.35% | -56.39% |
| array32 | 0 | 9.508 | 9.443 | 28.791 | 25.761 | -63.34% | -63.38% |
| array32 | 1 | 34.836 | 34.872 | 73.311 | 72.581 | -51.96% | -51.52% |
| array32 | 16 | 477.215 | 478.556 | 731.945 | 709.318 | -32.53% | -31.74% |
| array32 | 256 | 7375.497 | 7408.639 | 11854.508 | 11526.005 | -35.72% | -35.46% |
| array64 | 0 | 20.749 | 20.944 | 40.001 | 31.124 | -32.71% | -33.54% |
| array64 | 1 | 79.513 | 80.023 | 109.005 | 97.859 | -18.23% | -21.00% |
| array64 | 16 | 920.484 | 942.457 | 1135.058 | 1096.553 | -14.05% | -8.77% |
| array64 | 256 | 14473.541 | 14486.682 | 18331.283 | 17539.031 | -17.40% | -16.55% |
| array128 | 0 | 49.585 | 48.868 | 62.003 | 53.479 | -8.62% | -9.69% |
| array128 | 1 | 155.510 | 156.500 | 186.567 | 165.362 | -5.36% | -3.98% |
| array128 | 16 | 1779.740 | 1799.622 | 2032.396 | 1910.371 | -5.80% | -9.88% |
| array128 | 256 | 27566.008 | 27942.534 | 34607.704 | 32081.335 | -12.90% | -16.05% |

Baseline medians above come from a separate complete run and can include drift.
Use the adjacent paired measurements below for attributing compiler changes.

| Program | Zero rounds | One round | 16 rounds | 256 rounds |
|---|---:|---:|---:|---:|
| affine | -0.72% | +0.35% | -4.96% | -0.33% |
| polynomial | -1.16% | -0.62% | -5.15% | -1.75% |
| euclid | -0.95% | -5.37% | -0.89% | +0.62% |
| array8 | -0.45% | -0.42% | +0.44% | -0.03% |
| array16 | -0.08% | -0.70% | +0.41% | -0.39% |
| array32 | +0.17% | +0.19% | -0.26% | +0.75% |
| array64 | +0.07% | -0.40% | -0.54% | -0.07% |
| array128 | +0.02% | +0.25% | +0.02% | -0.20% |

Values are medians of within-block time changes; negative means faster. Small
losses are retained rather than removed from the report. These results describe
this Apple M2 Max/macOS 27.2 host and the recorded corpus, not all programs or
platforms.
