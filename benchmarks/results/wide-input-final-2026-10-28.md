# Wide and signed CPU inputs

Compiler/test source: `a7e826042fee7fda711fac915064b06a43b5a06a`. Baseline is the frozen published compiler from `9ea7918c` (compiler implementation `2840a4b8`). Apple M2 Max/macOS 27.2, Clang 23.1.3, Rust 1.93.0. No vendor numerical library, fast floating arithmetic or benchmark-name recognition was added.

## Outcome

The new baseline qualifies **34/54** cases against the faster Rust/C++ reference. Both final complete runs qualify **54/54**. The median of all six process medians per language/case puts **29/54 ahead of both references**, with a worst remaining gap of **6.6%**. The individual worst gaps are 6.3% and 7.3%; no complete run is discarded. These are measured cases, not universal performance across programs or all int64 values.

Nine paired blocks show negative arrays taking **50–59% less time**, wide/signed 16-round floating recurrences **43–45% less time**, and one-round Euclidean cases **5–26% less time**. The worst final paired regression is **0.8%**, on positive-wide array32/1. The existing 36-case small-input corpus and eight legacy array/closure controls still meet the 10% target.

## Cause and changes

The unsigned-only deferred reduction witness rejected negative terms and then replayed the **entire computation**, including all previous map iterations. An owned-child sample found 852 observations in the original reduction worker and 843 in its replay. The corrected profile has 1717 observations in the retained worker and none in replay. These sample counts locate work; they are not exact exclusive-time percentages.

For a reduction of N terms, let k = ceil(log2(N)) and M = 2^(63-k). If every term lies in [-M,M), every partial sum fits int64, regardless of order: -N*M >= INT64_MIN and N*(M-1) < 2^63. The new witness validates that window using unsigned modular bias and shift before any result escapes; a one-term reduction accepts all int64 values. Accumulation is defined modular arithmetic until validation. Invalid terms, unsafe extents or faults retain the original checked tail replay and first-fault ordering. No overflow flags or untrusted load-range promises are added. This covers the former unsigned-32 window and wider signed values instead of treating their sign bits as failure.

Four-way floating unrolling now has a count-only private version: counts 2–65535 use the original operation chain for arbitrary input data. It preserves multiply/add ordering, flags and precision; existing exact small-domain zero/one paths remain separate. Other counts retain their original implementation. This optimization interval is not a language count limit.

Integer one-round specialization fixes the count before simplifying the recurrence. A separate signed data guard admits [-2^46,2^46); the old small-positive version remains intact. LLVM can prove safe first-iteration arithmetic for wider/negative data without carrying general-loop state ranges. Outside those guards, public inputs retain their unrestricted implementation. The timings do not establish performance outside the recorded seed bank; correctness tests cover both guard boundaries and extreme int64 values.

[Baseline/current owned-process profiles](wide-input-profile-qualification-2026-10-28.json), [private range and derived-witness IR](wide-input-ir-qualification-2026-10-28.json).

## Corpus and measurement

Six source programs (affine, polynomial, repeated GCD, changing-state GCD, array32, array128) × three input regimes × 1/16/4096 rounds give 54 cases. Fixed-positive and fixed-negative inputs use ±(2^40+17). The rotating bank is `17, -17, 65536, -65536, 2^32+17, -(2^32+17), 2^40+17, -(2^40+17)`, selected cyclically on each call. Every language uses the same driver, including that selection and per-call bit validation overhead. The driver receives data at runtime; references have normal optimization and no forced allocation/inlining controls.

Source algorithms are unchanged. Signed remainder truncates toward zero; a user-written Euclidean function can return a negative GCD value. The harness integer oracle checks all intermediate arithmetic fits int64. Its floating oracle rounds each multiply and add separately. The independent auditor uses Fraction rounding/quotients and separately checks GCD magnitudes. It validates every checksum, corpus dimension, raw statistic, pair ordering and artifact hash. Each regime also exercises every seed lane before measurement.

Each complete run uses three independent processes, two warmups and seven measured adaptive batches targeting 10 ms. The paired study uses nine adjacent ABBA/BAAB blocks with equal iterations within each group. All warmup/calibration/measured return bits and wrapping checksums are checked. Times below are nanoseconds per call and aggregate six process medians. Negative reference differences mean less time; positive paired reductions mean improvement. Builds/tests/profiling stopped for acceptance timing; CPU affinity, frequency and desktop activity were not isolated. These are warm CPU calls, not process startup, GPU timings or physical macOS 26 qualification.

| Program | Regime | Rounds | Kalandra ns | C++ ns | Rust ns | Reference difference | Paired reduction |
|---|---|---:|---:|---:|---:|---:|---:|
| affine | wide-positive | 1 | 1.045 | 1.013 | 1.007 | +3.86% | -0.27% |
| affine | wide-positive | 16 | 5.205 | 8.126 | 8.106 | -35.79% | +44.23% |
| affine | wide-positive | 4096 | 8152.528 | 8293.424 | 8445.514 | -1.70% | -0.30% |
| affine | wide-negative | 1 | 1.051 | 1.006 | 1.005 | +4.57% | -0.35% |
| affine | wide-negative | 16 | 5.195 | 8.169 | 8.121 | -36.03% | +44.32% |
| affine | wide-negative | 4096 | 8088.758 | 8270.242 | 8415.059 | -2.19% | +0.21% |
| affine | rotating | 1 | 1.050 | 1.013 | 1.010 | +3.96% | -0.01% |
| affine | rotating | 16 | 5.170 | 8.150 | 8.094 | -36.13% | +43.29% |
| affine | rotating | 4096 | 8392.700 | 8420.573 | 8271.603 | +1.46% | -0.10% |
| polynomial | wide-positive | 1 | 1.119 | 1.099 | 1.081 | +3.54% | -0.31% |
| polynomial | wide-positive | 16 | 5.174 | 8.149 | 8.124 | -36.32% | +45.12% |
| polynomial | wide-positive | 4096 | 8414.164 | 8273.985 | 8268.822 | +1.76% | -0.45% |
| polynomial | wide-negative | 1 | 1.124 | 1.086 | 1.082 | +3.89% | -0.39% |
| polynomial | wide-negative | 16 | 5.154 | 8.153 | 8.156 | -36.78% | +44.48% |
| polynomial | wide-negative | 4096 | 8136.149 | 8128.418 | 8204.020 | +0.10% | -0.33% |
| polynomial | rotating | 1 | 1.130 | 1.087 | 1.087 | +3.97% | +0.01% |
| polynomial | rotating | 16 | 5.158 | 8.154 | 8.145 | -36.67% | +43.62% |
| polynomial | rotating | 4096 | 8093.526 | 8404.806 | 8449.870 | -3.70% | +0.24% |
| euclid | wide-positive | 1 | 12.046 | 11.859 | 12.718 | +1.58% | +4.78% |
| euclid | wide-positive | 16 | 439.674 | 432.894 | 432.171 | +1.74% | -0.14% |
| euclid | wide-positive | 4096 | 124876.091 | 124510.031 | 123910.805 | +0.78% | +0.06% |
| euclid | wide-negative | 1 | 5.727 | 5.556 | 6.249 | +3.09% | +16.18% |
| euclid | wide-negative | 16 | 354.590 | 359.413 | 357.901 | -0.93% | -0.41% |
| euclid | wide-negative | 4096 | 104100.761 | 105685.127 | 102610.927 | +1.45% | +0.40% |
| euclid | rotating | 1 | 6.656 | 6.565 | 7.540 | +1.37% | +13.89% |
| euclid | rotating | 16 | 305.619 | 307.844 | 311.395 | -0.72% | +0.21% |
| euclid | rotating | 4096 | 84136.697 | 94513.946 | 91162.868 | -7.71% | -0.06% |
| euclid_mixed | wide-positive | 1 | 13.414 | 12.681 | 14.579 | +5.78% | +16.60% |
| euclid_mixed | wide-positive | 16 | 1014.840 | 985.592 | 1004.437 | +2.97% | -0.11% |
| euclid_mixed | wide-positive | 4096 | 314460.297 | 314934.891 | 314106.723 | +0.11% | -0.26% |
| euclid_mixed | wide-negative | 1 | 6.970 | 6.544 | 7.598 | +6.51% | +26.41% |
| euclid_mixed | wide-negative | 16 | 935.911 | 900.323 | 904.941 | +3.95% | -0.08% |
| euclid_mixed | wide-negative | 4096 | 307863.266 | 311227.366 | 307667.312 | +0.06% | -0.10% |
| euclid_mixed | rotating | 1 | 7.774 | 7.445 | 8.565 | +4.42% | +21.83% |
| euclid_mixed | rotating | 16 | 723.640 | 724.464 | 714.276 | +1.31% | -0.14% |
| euclid_mixed | rotating | 4096 | 222768.700 | 219325.919 | 219573.500 | +1.57% | -0.37% |
| array32 | wide-positive | 1 | 40.381 | 73.278 | 71.412 | -43.45% | -0.74% |
| array32 | wide-positive | 16 | 480.743 | 739.349 | 703.917 | -31.70% | +0.15% |
| array32 | wide-positive | 4096 | 118354.651 | 192793.706 | 182523.486 | -35.16% | +0.06% |
| array32 | wide-negative | 1 | 40.353 | 75.646 | 71.858 | -43.84% | +58.27% |
| array32 | wide-negative | 16 | 481.705 | 737.685 | 705.477 | -31.72% | +50.60% |
| array32 | wide-negative | 4096 | 118195.012 | 192847.760 | 184110.381 | -35.80% | +50.03% |
| array32 | rotating | 1 | 36.384 | 73.575 | 71.365 | -49.02% | +43.51% |
| array32 | rotating | 16 | 483.880 | 737.949 | 705.754 | -31.44% | +34.52% |
| array32 | rotating | 4096 | 117431.930 | 193283.663 | 183312.856 | -35.94% | +33.16% |
| array128 | wide-positive | 1 | 167.038 | 187.191 | 167.035 | +0.00% | -0.11% |
| array128 | wide-positive | 16 | 1786.717 | 2025.256 | 1923.534 | -7.11% | -0.10% |
| array128 | wide-positive | 4096 | 438867.188 | 554301.526 | 520401.050 | -15.67% | -0.15% |
| array128 | wide-negative | 1 | 166.460 | 186.325 | 166.597 | -0.08% | +58.63% |
| array128 | wide-negative | 16 | 1781.341 | 2034.193 | 1920.346 | -7.24% | +50.89% |
| array128 | wide-negative | 4096 | 440498.250 | 556525.444 | 522199.975 | -15.65% | +50.55% |
| array128 | rotating | 1 | 147.301 | 185.826 | 157.684 | -6.58% | +45.45% |
| array128 | rotating | 16 | 1783.072 | 2030.766 | 1910.029 | -6.65% | +34.54% |
| array128 | rotating | 4096 | 441340.565 | 550622.842 | 524106.250 | -15.79% | +33.73% |

[Baseline](wide-input-baseline-2026-10-28.json), [final run](wide-input-final-2026-10-28.json), [repeat](wide-input-final-repeat-2026-10-28.json), [paired study](wide-input-final-paired-2026-10-28.json), [independent audit](wide-input-final-audit-2026-10-28.json).

[Small-input controls](wide-input-final-small-controls-2026-10-28.json), [small-input audit](wide-input-small-controls-audit-2026-10-28.json), [legacy controls](wide-input-legacy-current-2026-10-28.json), [legacy pairs](wide-input-legacy-paired-2026-10-28.json), [legacy audit](wide-input-legacy-audit-2026-10-28.json). Task-scope diagnostics include fresh pool/monitor construction and settlement; they are not cross-language rankings or steady-state scheduler throughput.

## Retained stages

The reduction-witness stage alone qualifies 43/54 and retains all 36 small-input targets. The scalar stage is provisional data from the same final implementation before final repeats. No stage or miss is discarded. [Witness stage](wide-input-witness-2026-10-28.json), [witness pairs](wide-input-witness-paired-2026-10-28.json), [scalar stage](wide-input-scalar-2026-10-28.json), [stage audit](wide-input-prototype-audit-2026-10-28.json), [baseline audit](wide-input-baseline-audit-2026-10-28.json).

## Qualification

Focused checks passed **12,595,440 integer cases**, **4,726,784 strict floating cases** and **139,392 reduction/first-fault/allocation-failure cases**, across debug/optimized/ASan-UBSan/TSan modes and applicable precisions. They include values on both sides of the derived reduction and signed one-round guards, cycles, huge convergent counts, intermediate overflow, exact results, first-fault locations and ledger settlement. Actual hosted Windows qualification passed **9,446,580 integer**, **3,545,088 strict floating**, and **104,544 reduction/fault cases**, plus the typed/scalar ABI and installed/downloaded packages. Linux installed/downloaded packages passed. macOS installed/relocated and downloaded CPU/Metal packages, Developer ID signing, notarization, stapling and assessment passed. Branch publication was skipped. Local qualification completed: **432 unique selected CPU checks** passed across four focused checks, the 428-case broad run and one unchanged serial rerun. The initial cancellation-observer failure is documented below. All **50 selected Metal-suite tests passed** with the physical device required and zero skips. No compiler/runtime source changed after measurement.

[Windows run](https://github.com/kalandra-lang/kalandra/actions/runs/40698722038), [Linux run](https://github.com/kalandra-lang/kalandra/actions/runs/40698722083), [macOS run](https://github.com/kalandra-lang/kalandra/actions/runs/40698722020), [Windows numerical records](wide-input-windows-qualification-2026-10-28.json), [focused numerical records](wide-input-focused-numerical-qualification-2026-10-28.json), [frozen source/artifact manifest](wide-input-qualification-2026-10-28.json).


The broader `-j8` run exposed one cancellation-test observer race in
`native-uint16-matmul-optimized`: the operation completed after 61,178 polls,
while the signaling thread never published its token (`token=0`, status 0).
The complete generated LLVM module is byte-identical to the frozen published
compiler's module. The original failure is retained; the unchanged check passed serially in
10.9 seconds, with the generated module still byte-identical. This is not
counted as a clean first-pass suite.

[Original CPU log](wide-input-cpu-qualification-2026-10-28.log),
[original CPU JUnit](wide-input-cpu-qualification-2026-10-28.xml),
[cancellation/byte-equivalence record](wide-input-cancellation-qualification-2026-10-28.json),
[serial rerun](wide-input-cancellation-rerun-qualification-2026-10-28.log),
[rerun JUnit](wide-input-cancellation-rerun-qualification-2026-10-28.xml).


[Focused log](wide-input-focused-qualification-2026-10-28.log), [focused JUnit](wide-input-focused-qualification-2026-10-28.xml), [reduction checks](wide-input-reduction-qualification-2026-10-28.log), [full build](wide-input-build-qualification-2026-10-28.log), [Metal log](wide-input-metal-qualification-2026-10-28.log), [Metal JUnit](wide-input-metal-qualification-2026-10-28.xml).
