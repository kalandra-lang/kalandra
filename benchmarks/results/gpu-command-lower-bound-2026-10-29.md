# GPU command and caller lower bounds

**Diagnostic experiments, not production optimizations.** The controls remove the prepared-region work checkpoints; the blocking variant additionally waits on its task worker. Applying either to arbitrary cancellable source tasks would change their guarantees. All shaders, operation cost/fusion checks, inputs and numerical trees remain unchanged.

For 64×512 resident execution, instrumented medians are 1.452 ms bounded, 0.847 ms whole-block, 0.840 ms caller-blocking and 0.537 ms MPSGraph. The whole-block reduction is about 42%; blocking adds little here. Reported GPU intervals are 0.510, 0.478, 0.481 and 0.255 ms respectively. Removing handoffs does not remove the remaining device-compute/dispatch gap.

The result warrants a distinct closed synchronous entry with an opaque private scheduler. It must expose no external cancellation handle and dynamically prove that no independent runnable/external operation can progress. Ordinary TaskCore calls retain bounded cancellation. This proof and implementation are future work, not properties of these controls.

## Measurements

Five shuffled fresh-process rounds, two warmup and three measured batches per process, adaptive 30 ms target. Profiling is enabled on the Kalandra controls; these wall times are diagnostic and must not replace uninstrumented acceptance. Encoding and GPU intervals are inclusive, overlapping counters, not additive exclusive wall-time buckets. Background load is retained separately.

| Shape | Mode | Control | Wall µs | GPU interval µs | Recorded encoding µs | Commands |
|---|---|---|---:|---:|---:|---:|
| 1 × 64 | resident | bounded | 381.52 | 153.10 | 37.70 | 1 |
| 1 × 64 | resident | indivisible | 388.57 | 154.36 | 39.66 | 1 |
| 1 × 64 | resident | blocking | 381.22 | 153.04 | 37.59 | 1 |
| 1 × 64 | resident | mps | 304.00 | 41.60 | — | — |
| 1 × 64 | activation | bounded | 383.79 | 153.51 | 36.10 | 1 |
| 1 × 64 | activation | indivisible | 384.80 | 152.94 | 36.78 | 1 |
| 1 × 64 | activation | blocking | 390.13 | 153.40 | 36.24 | 1 |
| 1 × 64 | activation | mps | 315.68 | 44.86 | — | — |
| 1 × 64 | all uploads | bounded | 458.95 | 154.49 | 35.90 | 1 |
| 1 × 64 | all uploads | indivisible | 480.20 | 158.78 | 36.31 | 1 |
| 1 × 64 | all uploads | blocking | 454.50 | 150.16 | 36.01 | 1 |
| 1 × 64 | all uploads | mps | 387.95 | 47.42 | — | — |
| 1 × 512 | resident | bounded | 390.43 | 154.22 | 39.56 | 1 |
| 1 × 512 | resident | indivisible | 387.55 | 155.07 | 38.27 | 1 |
| 1 × 512 | resident | blocking | 392.05 | 154.68 | 38.15 | 1 |
| 1 × 512 | resident | mps | 306.63 | 58.74 | — | — |
| 1 × 512 | activation | bounded | 395.39 | 156.75 | 36.11 | 1 |
| 1 × 512 | activation | indivisible | 392.84 | 155.81 | 35.95 | 1 |
| 1 × 512 | activation | blocking | 393.66 | 155.82 | 36.23 | 1 |
| 1 × 512 | activation | mps | 321.08 | 59.66 | — | — |
| 1 × 512 | all uploads | bounded | 1826.93 | 194.08 | 44.68 | 1 |
| 1 × 512 | all uploads | indivisible | 1856.79 | 206.74 | 43.75 | 1 |
| 1 × 512 | all uploads | blocking | 1861.97 | 246.60 | 44.13 | 1 |
| 1 × 512 | all uploads | mps | 1717.69 | 130.52 | — | — |
| 16 × 512 | resident | bounded | 942.81 | 327.68 | 73.41 | 3 |
| 16 × 512 | resident | indivisible | 665.23 | 348.08 | 66.33 | 1 |
| 16 × 512 | resident | blocking | 594.46 | 302.46 | 61.38 | 1 |
| 16 × 512 | resident | mps | 449.04 | 177.72 | — | — |
| 16 × 512 | activation | bounded | 931.43 | 324.77 | 68.88 | 3 |
| 16 × 512 | activation | indivisible | 617.61 | 298.77 | 61.15 | 1 |
| 16 × 512 | activation | blocking | 612.55 | 297.15 | 61.80 | 1 |
| 16 × 512 | activation | mps | 468.83 | 175.05 | — | — |
| 16 × 512 | all uploads | bounded | 2507.74 | 419.54 | 86.05 | 3 |
| 16 × 512 | all uploads | indivisible | 2022.70 | 348.46 | 68.33 | 1 |
| 16 × 512 | all uploads | blocking | 2045.71 | 342.69 | 70.07 | 1 |
| 16 × 512 | all uploads | mps | 1935.56 | 247.81 | — | — |
| 64 × 512 | resident | bounded | 1452.05 | 509.88 | 95.93 | 5 |
| 64 × 512 | resident | indivisible | 846.70 | 478.08 | 73.31 | 1 |
| 64 × 512 | resident | blocking | 839.93 | 480.62 | 70.68 | 1 |
| 64 × 512 | resident | mps | 536.90 | 254.74 | — | — |
| 64 × 512 | activation | bounded | 1520.14 | 522.30 | 97.20 | 5 |
| 64 × 512 | activation | indivisible | 859.35 | 481.39 | 66.23 | 1 |
| 64 × 512 | activation | blocking | 859.11 | 478.61 | 64.90 | 1 |
| 64 × 512 | activation | mps | 551.18 | 242.10 | — | — |
| 64 × 512 | all uploads | bounded | 2997.30 | 553.13 | 101.06 | 5 |
| 64 × 512 | all uploads | indivisible | 2254.96 | 536.80 | 71.31 | 1 |
| 64 × 512 | all uploads | blocking | 2207.17 | 536.36 | 72.03 | 1 |
| 64 × 512 | all uploads | mps | 1949.35 | 300.96 | — | — |

## Verification and provenance

117 full-output comparisons across 13 regular/odd shapes and all upload modes pass bit-for-bit against the frozen runtime with Metal API/shader validation. The frozen outputs already passed the independent CPU/MPSGraph numerical oracle. Runtime resource cleanup assertions pass on every call. Every variant uses the same 11 dispatches for one-row blocks and 12 otherwise; only prepared-command boundaries change.

The initial probe changed the unrelated generic-graph limit. Its submission-count assertion rejected it during calibration despite correct outputs. That failure is retained. The corrected probe changes only the prepared-progression checkpoint threshold, leaving cost saturation and fusion admission unchanged. No production source is edited.

Reproduce with `native/benchmarks/command_lower_bound.py --evidence benchmarks/results/gpu-encoding-overlap-paired-2026-10-29.json --build <new-directory> --output <new-json>`, using the frozen artifacts recorded in the source. No cancellation/lifetime qualification is claimed for these benchmark-only controls.

- [Raw diagnostics](gpu-command-lower-bound-2026-10-29.json)
- [Load](gpu-command-lower-bound-load-2026-10-29.json)
- [Rejected initial probe](gpu-command-lower-bound-initial-failure-2026-10-29.json)
- [Initial load](gpu-command-lower-bound-initial-load-2026-10-29.json)
