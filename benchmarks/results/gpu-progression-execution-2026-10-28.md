# GPU command progression

Against frozen `090d4087`, **24/27 case medians improve**, with 6 positive in all five paired blocks. Resident 64 × 512 takes **2.43% less API time**. In the independent control run it is 5.02× faster than the Kalandra CPU block, and takes 4.10× the MPSGraph time. These are transformer-block results, not whole-language or full-LLM rankings.

The accepted path pre-encodes eligible float32 chunks and lets completion callbacks check cancellation/faults and commit the next chunk. One external lease replaces repeated task-scheduler resumes. Four-stage/64 Mi-work quanta, immutable inputs, arithmetic order and first-fault behavior remain. Pipeline and backing ownership is retained explicitly through acknowledgement, permitting unretained Metal references in the admitted path.

Finite command-pool capacity is reserved before encoding. Long graphs, resource contention, tight budgets, unproved/checked math, other numeric formats and control/readback barriers use the existing streaming path. A 513-map / 129-chunk regression proves this is an optimization guard, not a language limit.

## Frozen paired results

Five adjacent ABBA/BAAB fresh-process blocks use identical iterations within each pair, calibrated to the faster implementation. Times are medians of paired block means; reductions are medians of within-block ratios. Both include hot metadata validation, capture binding, allocations, task/group setup, all submitted GPU work, acknowledgement and retirement. Neither includes pipeline loading, plan preparation or output readback. Timed checksums validate result extents; actual numerical cells are independently checked outside timing.

| Rows × width | Residency | Before (µs) | After (µs) | Time reduction | Faster blocks |
|---|---|---:|---:|---:|---:|
| 1 × 64 | Resident | 979.70 | 939.92 | +4.14% | 4/5 |
| 1 × 64 | Activation upload | 1118.77 | 1116.56 | +2.12% | 4/5 |
| 1 × 64 | All seven uploads | 2278.85 | 2276.53 | +0.55% | 4/5 |
| 16 × 64 | Resident | 1003.16 | 975.31 | +2.94% | 5/5 |
| 16 × 64 | Activation upload | 1187.70 | 1141.79 | +2.97% | 4/5 |
| 16 × 64 | All seven uploads | 2324.98 | 2301.18 | +1.02% | 3/5 |
| 64 × 64 | Resident | 1056.04 | 1040.80 | +1.76% | 4/5 |
| 64 × 64 | Activation upload | 1222.31 | 1237.55 | +0.09% | 3/5 |
| 64 × 64 | All seven uploads | 2192.04 | 2205.06 | +0.87% | 3/5 |
| 1 × 256 | Resident | 1035.13 | 994.13 | +4.97% | 5/5 |
| 1 × 256 | Activation upload | 1197.47 | 1130.99 | +4.02% | 5/5 |
| 1 × 256 | All seven uploads | 2590.66 | 2498.38 | +2.70% | 4/5 |
| 16 × 256 | Resident | 1194.04 | 1157.53 | +3.07% | 3/5 |
| 16 × 256 | Activation upload | 1363.40 | 1330.91 | +1.25% | 3/5 |
| 16 × 256 | All seven uploads | 2671.14 | 2675.30 | -0.16% | 2/5 |
| 64 × 256 | Resident | 1623.69 | 1473.45 | +9.25% | 5/5 |
| 64 × 256 | Activation upload | 1715.06 | 1676.93 | +2.57% | 4/5 |
| 64 × 256 | All seven uploads | 2944.12 | 3132.18 | -5.31% | 1/5 |
| 1 × 512 | Resident | 981.49 | 929.00 | +5.35% | 4/5 |
| 1 × 512 | Activation upload | 1149.44 | 1118.96 | +3.96% | 4/5 |
| 1 × 512 | All seven uploads | 3687.73 | 3649.91 | +0.86% | 3/5 |
| 16 × 512 | Resident | 1623.23 | 1544.20 | +4.87% | 5/5 |
| 16 × 512 | Activation upload | 1829.15 | 1757.56 | +3.04% | 5/5 |
| 16 × 512 | All seven uploads | 4255.33 | 4259.62 | -0.63% | 1/5 |
| 64 × 512 | Resident | 2236.83 | 2167.68 | +2.43% | 4/5 |
| 64 × 512 | Activation upload | 2455.17 | 2358.49 | +3.52% | 4/5 |
| 64 × 512 | All seven uploads | 4940.47 | 4860.53 | +0.51% | 3/5 |

Individual negative blocks and outliers remain in the raw records. Mixed signs are not evidence of a reliable per-case win. No gain is attributed solely to one subchange: pre-encoding, callback progression, pool sizing and reference retention are measured together.

## Rejected event-gated prototype

The isolated replay regresses **27/27 case medians**. One pre-encoded command with CPU/GPU shared-event checkpoints and zeroable indirect grids has fewer command submissions, but worse elapsed API time. It is not enabled in production. Its numerical outputs match the frozen executable bit for bit, and its independent Metal witness verifies grid suppression with API/shader validation. Full cancellation/device-failure qualification is not claimed for this rejected implementation.

| Rows × width | Resident change | Activation change | All-upload change |
|---|---:|---:|---:|
| 1 × 64 | -17.12% | -17.41% | -14.60% |
| 16 × 64 | -19.96% | -17.17% | -7.91% |
| 64 × 64 | -24.70% | -33.80% | -27.24% |
| 1 × 256 | -29.91% | -20.94% | -17.59% |
| 16 × 256 | -33.83% | -31.22% | -12.63% |
| 64 × 256 | -44.89% | -31.21% | -30.40% |
| 1 × 512 | -18.06% | -25.60% | -11.45% |
| 16 × 512 | -42.60% | -36.86% | -18.36% |
| 64 × 512 | -71.22% | -54.10% | -22.01% |

Positive remains less API time; negative means regression. Event measurements use their own adjacent frozen baseline controls. The tiny gate witness is a synchronization experiment, not an application benchmark.

## Retention isolation

A second five-block paired control changes only retained versus unretained references in the same compiled command-chain runtime and shader bundle. Here positive means retained references are faster. Retention is not a general fix for the all-upload regression: results vary by case. Resident 64 × 512 is slower with retained references in all five blocks; the default remains lease-owned unretained references. The 64 × 256 all-upload regression against the original runtime remains visible rather than being routed around by benchmark case.

| Rows × width | Resident retained vs unretained | Activation | All upload |
|---|---:|---:|---:|
| 1 × 64 | -1.65% | +1.16% | -1.04% |
| 16 × 64 | +1.37% | -0.02% | -1.18% |
| 64 × 64 | -1.12% | -1.35% | -0.49% |
| 1 × 256 | +1.17% | -1.77% | -1.33% |
| 16 × 256 | -0.05% | +1.38% | -0.29% |
| 64 × 256 | +0.00% | +1.48% | +0.87% |
| 1 × 512 | +0.42% | +1.46% | +0.96% |
| 16 × 512 | -0.13% | -0.19% | +1.63% |
| 64 × 512 | -1.38% | -0.50% | -0.97% |

## CPU and vendor controls

A separate five-block counterbalanced CPU/Metal/MPSGraph study measures the same bias-free single-head transformer block: pre-RMSNorm, separate Q/K/V projections, unmasked attention, output projection, a four-times-width ReLU FFN and two residuals. This is not token generation or a full model. Production code uses its own kernels; MPSGraph is benchmark-only. Vendor reduction/FMA choices need not be bit-identical to Kalandra’s separately rounded products and adjacent-pair tree; all outputs meet the independent per-cell oracle/bound.

| Rows × width | CPU (µs) | Metal resident (µs) | MPSGraph resident (µs) | CPU / Metal | Metal / MPS |
|---|---:|---:|---:|---:|---:|
| 1 × 64 | 14.80 | 934.39 | 306.76 | 0.02× | 3.05× |
| 16 × 64 | 68.82 | 977.68 | 303.26 | 0.07× | 3.22× |
| 64 × 64 | 266.18 | 1020.38 | 309.23 | 0.26× | 3.30× |
| 1 × 256 | 59.13 | 996.86 | 301.22 | 0.06× | 3.31× |
| 16 × 256 | 1163.90 | 1150.01 | 366.69 | 1.01× | 3.14× |
| 64 × 256 | 2728.17 | 1484.71 | 422.44 | 1.84× | 3.51× |
| 1 × 512 | 263.43 | 942.67 | 313.83 | 0.28× | 3.00× |
| 16 × 512 | 4849.39 | 1562.88 | 452.10 | 3.10× | 3.46× |
| 64 × 512 | 10836.67 | 2158.55 | 526.49 | 5.02× | 4.10× |

Small workloads still favor CPU. MPSGraph remains ahead. Submission progression alone does not close that gap. Input transfers still use individual staged blits; a unified-memory copy policy is a separate experiment requiring matched vendor controls and qualification. Further compiler/kernel fusion and call-site cancellability analysis are architectural targets; simply batching unbounded work or reading an unsynchronized CPU/GPU cancellation word is not the accepted solution.

## Qualification and profiling

- Independent control validation: 91 records / 476,455 cells; odd shapes and all residency modes included.
- Frozen GPU output dumps match bit for bit; all numerical shader source hashes are identical. The compiler binary and CPU full-block LLVM are unchanged.
- Optimized, debug, ASan/UBSan and TSan prepared-graph tests: 116 small-chain and 119 large-chain injected allocation checkpoints per mode, plus 112 streaming control checkpoints. Held cancellation, recorded deadline and device failure cover all four and seven chunk boundaries. Higher ranks, 513-map fallback, private reuse/aliases, dropped executable handles, concurrent invocations, core teardown and 16/32/64 KiB fallback pass. Compiler-generated source entry points run in all four modes.
- Typed hardware matrices: 1,724 APFloat cases / 1,598,840 cell/aggregate checks per mode. All-ten-type matrix CLI tests, legacy graph batching, tensor transforms, external leases, CPU source regression and relocated installation passed. A matrix CLI harness was updated to accept the shader-validation informational notice; numeric assertions remain strict.
- Profiling is a separate explicitly instrumented run. GPU and recorded encoding intervals are not an exhaustive exclusive-time breakdown; unclassified wall time must not be called entirely scheduler or driver overhead. Acceptance runs overlap no builds, tests, plots or profiling.

- Profile 1 × 512 resident: wall 1.233 ms, GPU command intervals 0.445 ms, recorded encoding 0.079 ms, 116 admission attempts, 4 submissions / 14 dispatches.
- Profile 64 × 512 resident: wall 2.166 ms, GPU command intervals 0.711 ms, recorded encoding 0.111 ms, 119 admission attempts, 7 submissions / 14 dispatches.

## Evidence and reproduction

Baseline artifacts were frozen before edits under `benchmarks/.build/gpu-progression-2026-10-28/baseline`. Its manifest is embedded in paired evidence. The baseline shader bundle is `benchmarks/.build/gpu-fusion-2026-10-28/acceptance/bundle`. Ignored binaries are not committed; rebuild `090d4087` in a separate directory to regenerate them. All samples, calibration, exact commands and artifact/source/toolchain hashes are retained. The event runner applies its archived patch to that exact commit in an isolated source tree.

- [transformer-progression-full-block-2026-10-28.json](transformer-progression-full-block-2026-10-28.json) — SHA256 `7903c8427ac4053e79fe603ecd42fa8d77c24054ccb9b74e2b5d6cf783d7e63b`
- [transformer-progression-paired-2026-10-28.json](transformer-progression-paired-2026-10-28.json) — SHA256 `946368846a97bc8769f652567e0c3614998a1a52311198a173e03b8c102e549a`
- [transformer-progression-profile-2026-10-28.json](transformer-progression-profile-2026-10-28.json) — SHA256 `d759c0f53e3ef92fdd1f3a93563ccf6bfa1fa7fc906a5bf46dc23eca16e8d7d3`
- [transformer-progression-events-2026-10-28.json](transformer-progression-events-2026-10-28.json) — SHA256 `75ea8af4d211f0a6923e67f925a0a0248b65fd0d7c7d5655eaf4d8d093f3c8be`
- [transformer-progression-qualification-2026-10-28.json](transformer-progression-qualification-2026-10-28.json) — SHA256 `4db2fe19d09ac4d6c956a0debf3a904c90a011b00c5650a5f012ae65e1226cd1`
- [transformer-progression-retention-2026-10-28.json](transformer-progression-retention-2026-10-28.json) — SHA256 `3d24d7ead28869d54b32ee494fa0b9c1abba5e4c71059b658bdcbc0e3baa93ee`

```sh
python3 native/benchmarks/transformer_block.py --build-dir benchmarks/.build/gpu-progression-2026-10-28/acceptance --output benchmarks/results/transformer-progression-full-block-2026-10-28.json --blocks 5 --target-ms 30
python3 native/benchmarks/prepared_execution.py --baseline benchmarks/.build/gpu-progression-2026-10-28/baseline --baseline-bundle benchmarks/.build/gpu-fusion-2026-10-28/acceptance/bundle --current-evidence benchmarks/results/transformer-progression-full-block-2026-10-28.json --output benchmarks/results/transformer-progression-paired-2026-10-28.json --blocks 5
python3 native/benchmarks/event_progression.py --baseline benchmarks/.build/gpu-progression-2026-10-28/baseline --bundle benchmarks/.build/gpu-fusion-2026-10-28/acceptance/bundle --build-dir benchmarks/.build/gpu-progression-2026-10-28/event-replay --output benchmarks/results/transformer-progression-events-2026-10-28.json --blocks 3
python3 native/benchmarks/transformer_profile.py --evidence benchmarks/results/transformer-progression-full-block-2026-10-28.json --build-dir benchmarks/.build/gpu-progression-2026-10-28/profile --output benchmarks/results/transformer-progression-profile-2026-10-28.json --dispatches 14 --large-submissions 7
```
