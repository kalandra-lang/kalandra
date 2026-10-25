# GPU matmul bottleneck and rejected microkernels — 2026-10-25

The next large resident bottleneck is the **matrix/fused-epilogue encoder**, not reduction. Encoder timestamps attribute a median **99.36% of sampled compute-stage ticks at 2048²** to that stage. Screening 29 alternatives did **not** establish a replacement worth shipping. The compiler, CPU/Metal runtimes, production shaders, selection policy, arithmetic and safety behavior remain unchanged and match the preceding qualified artifacts byte for byte. This is an investigation result and reusable measurement tooling, not a new speedup claim.

## Stage measurement

The Apple M2 Max exposes only the public timestamp counter set, supports encoder/stage boundaries, and does not support dispatch-boundary sampling. There are no public occupancy/register-stall/bandwidth counters in this probe. The diagnostic uses the existing fused matrix entry and serial multi-dispatch reduction entry under one command, with scalar readback. It compares encoder-boundary sampling against an unsampled command control, with three randomized processes, two warmups and seven measured rounds each. Every matrix cell, the output suffix guard and the independent scalar reference are checked every round.

Inputs are preallocated shared buffers, outputs are private, and reduction scratch is preallocated per level. This differs from managed input uploads/allocation and the runtime's ping-pong scratch ownership. These are diagnostic GPU-stage fractions, not complete language pipeline timings. Raw counter ticks avoid assuming a device clock conversion. Whole-command times come independently from Metal's GPU command timestamps; sampling and host/GPU drift can perturb the control comparison.

| Square size | Matrix + epilogue share of sampled stage ticks | Reduction share | Sampled command median (ms) | Unsampled command median (ms) |
|---:|---:|---:|---:|---:|
| 512 | 91.77% | 8.23% | 0.226 | 0.235 |
| 1024 | 97.58% | 2.42% | 1.482 | 1.497 |
| 2048 | 99.36% | 0.64% | 12.284 | 12.291 |

Even removing reduction completely would save under 1% of these sampled compute ticks at 2048². This does not prove which matrix instructions dominate: timestamps alone cannot distinguish arithmetic throughput, shared-memory traffic, register pressure, cache misses or synchronization stalls.

## Experiments

All candidate arithmetic retains separately rounded products and adjacent-pair subtree additions, with `-fno-fast-math -ffp-contract=off`. The screen uses runtime dyadic inputs, full output-cell checks against an independent integer reference, poisoned output and a suffix guard every round, outside GPU timing. It uses three randomized fresh processes, two warmups and five measured rounds at 512²/1024²/2048². This is preliminary numerical screening, not full non-dyadic/NaN/sanitizer qualification of a replacement. Rejected code never enters production.

- **Nine panel/reuse candidates:** 128/256-leaf panels, ping-pong panels, and explicit per-lane reuse across two/four outputs. These increase panel storage or live values; most regress substantially.
- **Nine scheduling candidates:** preserve the original compact eight-leaf helper while trying loop-unroll factors, larger panels and ping-pong loading. None establishes a large-matrix gain beyond noise.
- **Seven packed/specialized candidates:** adjacent float2/float4 outputs, shorter 32-leaf panels, constant-contraction templates, and full unrolling. Packed forms lose. Ordinary constant-contraction specialization appears promising in the unpaired screen, so it receives the paired confirmation below.
- **Four barrier-free SIMD input-sharing candidates:** 1×32, 2×16, 4×8 and 8×4 SIMD output layouts. These trade shared panels for device loads/shuffles and lose badly. The measured loss is retained; it is not evidence that synchronization is free or a direct measurement of cache stalls.

The pipeline's reported maximum thread count is 1024 for the current kernel, versus 384 for several explicit multi-output candidates. Larger contraction panels increase static threadgroup storage from 8 KiB to 16/32 KiB and sometimes reduce that thread cap. These are resource indicators, not measurements of actual occupancy or register allocation, and do not by themselves establish the cause of a slowdown.

The following are per-suite medians of process GPU-command medians (ms), not managed pipeline results. Each suite has its own contemporaneous production control; cross-suite comparisons are not paired. Full samples, source snapshots, generated MSL and capability fields are in the raw evidence.

| Suite | Candidate | 512² | 1024² | 2048² |
|---|---|---:|---:|---:|
| screening | kalandra_matmul_hardware_register16 | 0.277 | 1.427 | 12.077 |
| screening | micro_16_16_256_128_0 | 0.695 | 1.702 | 15.524 |
| screening | micro_16_16_256_256_0 | 0.874 | 2.120 | 19.467 |
| screening | micro_16_16_256_64_1 | 0.253 | 2.268 | 15.068 |
| screening | micro_16_16_256_128_1 | 0.425 | 2.945 | 25.250 |
| screening | micro_16_16_128_64_0 | 0.653 | 4.553 | 42.413 |
| screening | micro_32_16_256_64_0 | 0.824 | 5.986 | 49.597 |
| screening | micro_16_32_256_64_0 | 0.794 | 5.810 | 51.864 |
| screening | micro_32_32_256_64_0 | 2.251 | 14.396 | 112.978 |
| screening | micro_32_16_256_64_1 | 1.216 | 5.801 | 46.678 |
| scheduled-screening | kalandra_matmul_hardware_register16 | 0.201 | 1.456 | 12.095 |
| scheduled-screening | scheduled_64_0_2 | 0.203 | 1.480 | 12.090 |
| scheduled-screening | scheduled_64_0_4 | 0.594 | 1.436 | 12.111 |
| scheduled-screening | scheduled_64_0_8 | 0.211 | 1.444 | 12.117 |
| scheduled-screening | scheduled_64_0_16 | 0.205 | 1.429 | 12.071 |
| scheduled-screening | scheduled_128_0_0 | 0.390 | 1.926 | 14.802 |
| scheduled-screening | scheduled_256_0_0 | 0.485 | 2.061 | 18.810 |
| scheduled-screening | scheduled_64_1_0 | 0.427 | 1.809 | 14.847 |
| scheduled-screening | scheduled_64_1_2 | 0.259 | 1.969 | 15.108 |
| scheduled-screening | scheduled_128_0_2 | 0.240 | 1.932 | 14.307 |
| vector-screening | kalandra_matmul_hardware_register16 | 0.201 | 1.469 | 12.464 |
| vector-screening | vector_2_64_0 | 0.330 | 2.949 | 20.447 |
| vector-screening | vector_2_32_0 | 0.268 | 1.857 | 18.495 |
| vector-screening | vector_4_64_0 | 0.378 | 2.666 | 26.654 |
| vector-screening | vector_2_64_1 | 0.293 | 2.156 | 20.051 |
| vector-screening | vector_2_32_1 | 0.266 | 1.866 | 19.037 |
| vector-screening | fixed_0 | 0.193 | 1.455 | 12.072 |
| vector-screening | fixed_1 | 0.223 | 1.619 | 16.668 |
| simd-screening | kalandra_matmul_hardware_register16 | 0.201 | 1.474 | 13.577 |
| simd-screening | simd_1_32 | 1.111 | 9.103 | 72.631 |
| simd-screening | simd_2_16 | 17.429 | 136.520 | 1149.071 |
| simd-screening | simd_4_8 | 17.193 | 133.201 | 1115.176 |
| simd-screening | simd_8_4 | 17.176 | 133.848 | 1117.957 |

## Paired confirmation and decision

The only plausible specialized candidate receives seven randomized mirrored ABBA/BAAB blocks across nine square/rectangular, narrow and partial-tile shapes. Both entries use the same library/driver; every process has two warmups and five measured GPU intervals. All numerical and guard checks pass. Paired reductions include every block, including regressions. The medians of displayed baseline/current block times need not yield the median paired reduction.

| M × K × N | Baseline median (ms) | Specialized median (ms) | Median paired time reduction | All-block range |
|---|---:|---:|---:|---:|
| 16 × 2048 × 4097 | 0.369 | 0.441 | +6.21% | -22.09% to +23.01% |
| 128 × 1024 × 512 | 0.279 | 0.215 | +14.62% | +1.73% to +48.31% |
| 256 × 512 × 256 | 0.081 | 0.092 | -3.26% | -27.57% to +9.25% |
| 257 × 512 × 257 | 0.163 | 0.130 | +19.97% | -30.24% to +51.99% |
| 512 × 512 × 512 | 0.495 | 0.315 | +6.31% | -37.96% to +52.21% |
| 512 × 2048 × 128 | 0.550 | 0.358 | +6.04% | -10.64% to +34.86% |
| 1024 × 1024 × 1024 | 1.844 | 1.786 | +1.14% | -20.83% to +30.96% |
| 2048 × 2048 × 2048 | 13.996 | 13.671 | +0.48% | -27.26% to +9.29% |
| 4097 × 2048 × 16 | 0.376 | 0.486 | +2.00% | -34.55% to +29.61% |

At 2048² the paired median is only **+0.48%**, with **−27.26% to +9.29%** block reductions. One rectangle improves consistently, but there are regressions elsewhere and substantial variation. These measurements do not justify a broad replacement or a shape rule fitted to that one rectangle. Full unrolling is also rejected. No extra specialization, register storage, cold pipeline cost, language restriction or memory-policy change is shipped.

There is no new complete-pipeline benchmark because no production candidate passed the preceding acceptance step. The [current full pipeline measurements](epilogue-pipeline-2026-10-25.md) remain the applicable performance evidence. No new comparison against Rust/C++ or optimized libraries is claimed. Mathematical tree preservation in these candidates is not a claim of full numerical qualification; any future promoted candidate must still pass the APFloat/non-dyadic/NaN, allocation/cancellation and four-mode regression gates before complete-pipeline acceptance timing.

## Reproduction and next evidence needed

Use the pinned Metal toolchain and current native compiler to generate the `native/benchmarks/Pipeline.k` hardware bundle. Choose unused output paths:

```sh
python3 native/benchmarks/matrix_pipeline_stages.py \
  --bundle /path/to/Pipeline.k/bundle --output /path/to/stages.json
python3 native/benchmarks/matrix_microkernel_study.py \
  --suite initial --output /path/to/initial.json
# Repeat with --suite scheduled, vector and simd.
python3 native/benchmarks/matrix_microkernel_confirm.py \
  --screen /path/to/vector.json --variant fixed_0 --output /path/to/confirmation.json
```

Performance scripts reject profiling/validation/injection environments. The stage tool separately passes Metal API/shader validation, full-cell/suffix/scalar checks and a warnings-as-errors host build. Existing evidence/oracle and epilogue-codegen tests pass. Production source/archive hashes are checked against the preceding qualification, so its language/runtime tests remain applicable; rejected experimental kernels are not promoted on the basis of dyadic screening.

Physical measurements are this M2 Max on macOS 27; generated code targets macOS 26. No affinity/frequency/host isolation is imposed. Historic screening generators and drivers are embedded verbatim in each JSON, with their hashes, because the exploratory generator evolves between suites. The final generator retains all four suites for reproduction.

The next distinct experiment would change input layout/weight packing or obtain a richer GPU capture to distinguish matrix data movement from arithmetic stalls. Larger panels, more outputs, extra unrolling and barrier removal alone have now been tested and should not be assumed to help. Layout packing would need its construction cost, extra memory, repeated-weight reuse and immutable ownership measured before integration. Strict arithmetic remains the contract.

Evidence: [encoder stages](microkernel-stages-baseline-2026-10-25.json), [panel/reuse screen](microkernel-screening-2026-10-25.json), [scheduling screen](microkernel-scheduled-screening-2026-10-25.json), [packed/specialized screen](microkernel-vector-screening-2026-10-25.json), [SIMD screen](microkernel-simd-screening-2026-10-25.json), [paired confirmation](microkernel-fixed-confirmation-2026-10-25.json), [validation and production hashes](microkernel-qualification-2026-10-25.json).
