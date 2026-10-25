# Strict CPU and Metal matrix tuning — 2026-10-25

On the Apple M2 Max, the final paired run reduces float32 CPU matrix time by **5.3% at 512² and 4.9% at 1024²**, resident Metal time by **9.4% at 1024² and 13.2% at 2048²**, and upload-inclusive Metal time by **5.2% and 11.0%** at those larger sizes. Float64 keeps its baseline implementation after the larger subtree trial regressed it. These are tensor-backend improvements, not source-algorithm comparisons with Rust/C++ or a claim of optimized-library parity.

## Measurement boundary

Baseline is `48b812aa` before these changes. Both CPU versions are freshly compiled from the same source and linked against the same optimized runtime archive. The saved baseline Metal executable was linked before modifying the Metal archive; its driver text is identical to the final driver. Compiler, executable, generated LLVM and metallib hashes accompany the data. Final float64 benchmark LLVM is byte-for-byte identical to baseline.

Each cell measures a complete matrix product plus final sum: fresh invocation, result/intermediate allocation, CPU packing and cleanup, or GPU dispatch/completion/scalar readback. Resident excludes uploads; transfers includes uploads. Initialization, process startup, shader loading and compilation are excluded. CPU uses one thread; Metal uses the GPU. Arithmetic uses hardware float32/float64 on CPU and hardware float32 on Metal, retaining separately rounded products and the exact adjacent-pair reduction tree.

Seven seeded ABBA/BAAB blocks run fresh baseline/current processes with the same calibrated iteration counts. Each process warms two full batches and measures three. Builds, sanitizer tests, frame audits and correctness qualification finished before acceptance timing. Every warmup/measured batch is checked against an independent exact integer checksum, and the driver checks input immutability and cleanup. No affinity, frequency lock or host isolation is claimed. Timing rows were verified to contain no GPU profiling counters; instrumented counters are collected afterward.

Before/after columns are medians of the seven paired block times. Percentage is the median of the seven paired time reductions, so it need not equal the ratio of the two displayed medians. Negative percentages retain slowdowns. Small changes in unchanged paths are not attributed to the optimization.

## Complete-call results

| Execution | Type | Square size | Before ms | After ms | Paired time reduction |
| --- | --- | ---: | ---: | ---: | ---: |
| cpu | float32 | 128 | 0.1258 | 0.1215 | +3.22% |
| cpu | float32 | 256 | 0.9186 | 0.9019 | +1.73% |
| cpu | float32 | 512 | 7.2253 | 6.8310 | +5.34% |
| cpu | float32 | 1024 | 57.1960 | 54.4982 | +4.88% |
| cpu | float64 | 128 | 0.2426 | 0.2416 | +0.49% |
| cpu | float64 | 256 | 1.9868 | 1.9741 | -0.07% |
| cpu | float64 | 512 | 15.8503 | 15.8011 | -0.22% |
| cpu | float64 | 1024 | 128.8980 | 128.8427 | +0.29% |
| resident | float32 | 128 | 0.3009 | 0.3045 | -1.71% |
| resident | float32 | 256 | 0.3396 | 0.3404 | -0.57% |
| resident | float32 | 512 | 1.2812 | 1.2678 | +0.73% |
| resident | float32 | 1024 | 2.5263 | 2.2898 | +9.44% |
| resident | float32 | 2048 | 21.2967 | 18.4920 | +13.16% |
| transfers | float32 | 128 | 0.6475 | 0.6048 | +7.00% |
| transfers | float32 | 256 | 0.7774 | 0.7829 | -0.96% |
| transfers | float32 | 512 | 1.8859 | 1.8770 | -0.27% |
| transfers | float32 | 1024 | 3.9544 | 3.6889 | +5.23% |
| transfers | float32 | 2048 | 25.4633 | 22.7425 | +10.96% |

All seven paired blocks improve the larger resident GPU cases. Their ranges are:

- resident float32 1024²: 1.79% to 10.72% less time.
- resident float32 2048²: 9.66% to 13.97% less time.
- cpu float32 512²: 3.63% to 6.60% less time.
- cpu float32 1024²: 3.79% to 5.62% less time.

## Implementation and limits

- CPU float32 forms 32-leaf register subtrees instead of 16, storing and reducing fewer intermediate roots. Float64 retains 16 leaves. Tail workspace sizing and subtree generation share the same compiler helper; odd carries, strided fallbacks and checked byte arithmetic remain. At K=1024, wide scratch decreases from 5,184 to 4,160 bytes for float32. Some shorter contractions need more scratch; this is not a uniform memory-footprint reduction.
- Metal adds a 16×8 output tile with 128 lanes and exact 64-leaf contraction subtrees, staging 1,024 left and 512 right floats. Each RHS panel is reused across sixteen rows; two barriers cover 64 contraction elements. All lanes reach both barriers; inactive output lanes skip arithmetic.
- The new GPU entry requires at least sixteen rows/eight columns, K≥512 divisible by 64, and a pipeline supporting 128 threads. The existing quad profile takes precedence. Older bundles, other contraction tails, and smaller shapes retain prior entries. Work-item sizing now follows the selected entry, eliminating excess dispatch for the existing quad path.
- Hardware arithmetic, exact reduction order, immutable inputs, cancellation checks, stack guards, allocation faults, cleanup and submitted GPU-buffer ownership are retained. No FMA, fast math, reassociation, implicit software fallback, or vendor-library substitution is enabled.

## Profiles

The refreshed two-second CPU sample places 1,675 of 1,704 samples in the generated matrix worker. Sampled hot PCs include packed SIMD multiply/add instructions; allocation helpers are not the dominant sampled cost. This supports optimizing the numerical kernel, but does not establish an exclusive cache/register/throughput decomposition. The transcript and pilot data are retained in the qualification record.

Separate post-acceptance GPU counters cover the complete composed command buffer, not individual shader occupancy or spilling. Intervals may overlap; subtracting their medians is not an exact time decomposition.

| Size | Version | Wall ms | Device ms | Encoding ms | Submissions/call | Dispatches/call |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 256 | baseline | 0.3551 | 0.1184 | 0.0234 | 1 | 4 |
| 256 | current | 0.3369 | 0.0993 | 0.0245 | 1 | 4 |
| 512 | baseline | 1.2459 | 0.9812 | 0.0248 | 1 | 4 |
| 512 | current | 1.2587 | 0.9821 | 0.0260 | 1 | 4 |
| 1024 | baseline | 2.5021 | 2.1524 | 0.0291 | 1 | 4 |
| 1024 | current | 2.2654 | 1.9206 | 0.0290 | 1 | 4 |
| 2048 | baseline | 20.8564 | 20.0571 | 0.0420 | 1 | 5 |
| 2048 | current | 18.4836 | 17.7872 | 0.0325 | 1 | 5 |

## Correctness and rejected experiments

- The broad run passed 32 distinct selected CTest gates: CPU numerical/static-shape/benchmark-cell checks, source matrix ownership/allocation checks, Metal matrix/graph variants, checked inlining, stack outlining and Windows tail-codegen control. The initial CPU suite covered 40 kernel/type/build variants plus higher-rank controls. After retaining float64’s original subtree, all 16 matrix/type/build variants passed again on the final compiler.
- Final actual CPU benchmark matrices through 1024² for both float widths passed **2,752,512 independent per-cell integer checks**, including immutability, status and balanced cleanup.
- Each of four GPU build variants passed **2,399 cases and 5,804,431 exact comparisons**, using independent APFloat/FTZ and integer oracles, including every cell through 2048², per-cell/aggregate results, exceptional floats, tails, and lifetime/failure controls. The final matrix metallib is byte-identical to the qualified library.
- Final ARM64 frame audit covers eight matrix binaries and 336 emitted frame records; maximum 6,928 bytes, below the unchanged 16 KiB function allowance with the 128 KiB native reserve retained. This is evidence for these binaries, not a cross-platform machine-code proof. Five fair-benchmark protocol/reference unit tests passed.
- Four stack-outline gates initially failed because their assertion still required runtime v3 after the preceding v4 migration. The saved baseline compiler reproduces the failure. Updating the expected symbol preserves the 40-helper count and individual noinline frame checks; all four gates then pass.
- Exploratory CPU four-row tiles, 64-leaf subtrees and forced explicit packed vector loads were not retained: gains were mixed or smaller shapes regressed. GPU 128-leaf blocks were slower. No register-spill cause is claimed without counters.
- The first complete paired trial used 32 leaves for both CPU widths. Float64 regressions caused rejection of that choice; its full results remain in [trial data](matrix-tuning-trial-2026-10-25.json). The final repeat retains every control, including small GPU variations and identical-code float64 timing variation.

## Evidence and reproduction

[Final raw timings and commands](matrix-tuning-2026-10-25.json) · [Qualification, logs, pilots and baseline failure reproduction](matrix-tuning-qualification-2026-10-25.json) · [Final frame audit](matrix-tuning-stack-2026-10-25.json).

Save the baseline compiler and the fair suite’s float32 GPU executable/bundle from revision `48b812aa` before rebuilding. Baseline binaries remain ignored local build artifacts; recreate them from that revision. This run used the following saved inputs:

```sh
python3 native/benchmarks/matrix_run.py \
  --baseline-compiler benchmarks/.build/matrix-2026-10-25/compiler-baseline \
  --baseline-gpu benchmarks/.build/matrix-2026-10-25/gpu-baseline \
  --baseline-gpu-bundle benchmarks/.build/matrix-2026-10-25/gpu-baseline-bundle \
  --output benchmarks/results/matrix-tuning-2026-10-25.json
```

Use a new output path. `--build-only` prepares artifacts without timing; `--reuse-build` requires matching input/artifact hashes. Qualification uses `CpuOptimizationTests.py`, `CpuBenchmarkCells.py`, `GpuOptimizationTests.py` and the selected CTest gates recorded above. Physical GPU access is required. Findings apply to this M2 Max/macOS 27.2 run; the macOS deployment minimum remains 26. Linux/Windows performance was not measured. Remaining numerical-library gaps are not closed by these modest kernel improvements; complete resident graphs and allocation/synchronization costs remain separate optimization targets.
