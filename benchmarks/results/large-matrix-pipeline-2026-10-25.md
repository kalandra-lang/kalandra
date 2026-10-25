# Large strict GPU matrix kernels — 2026-10-25

The complete workload is `reduceSum(relu((matmul(a,b)+bias)*scale))`, with full-shape bias and runtime float32 inputs. A qualified 16×16 Metal tile replaces the older path on Apple M2 Max. It reduces resident pipeline time by a paired median 55.5% at 512², 19.3% at 1024² and 29.5% at 2048². Upload-inclusive reductions are 36.6%, 10.6% and 21.3%. These are local workload results, not general language or library parity.

## Change and mechanism

The old 16×8 tile uses 128 threads, 6 KiB of threadgroup panels and dynamically indexed private roots. The new 16×16 tile uses 256 threads, 8 KiB of panels and seven explicit scalar roots. Doubling columns doubles reuse of each left panel; for full output tiles, cooperative input-panel loads per output decrease by one third. Products are still separately rounded, followed by the same adjacent-pair tree. The change does not use fused multiply-add or vendor matrix operations.

The specialization admits `M,N >= 16`, `M*N >= 65536`, and `512 <= K <= 4096` divisible by 64. At most 64 aligned block roots justify seven storage levels, including the top root. Outside this envelope the existing GPU kernels remain available: there is no new language shape limit, CPU fallback or change to other numeric types/software policy. Only the qualified M2 Max profile selects the new entry, after checking its 256-thread pipeline capability. It also overrides the older quad-layout choice that masked the faster hardware path at some 512² shapes.

Kernel preparation reflects and validates the same four-buffer ABI. Cancellation, checked allocation, byte budgets, immutable inputs, owners retained until GPU acknowledgement and the previous one-submission pipeline batching remain enabled. Loading a current hardware bundle prepares an additional optional pipeline; cold initialization/preparation time is outside the throughput benchmark and is not claimed to improve.

## Final paired complete-pipeline timing

Baseline: frozen Metal archive from `515294a2ec86d38c4cc772c003863bf7421ca00b`, whose hash matches the preceding qualified pipeline evidence. Both versions use the same current shaders, benchmark driver and optimized task runtime; the old Metal implementation ignores the new optional entry. The old hardware shader bodies are unchanged. Generated CPU pipeline LLVM remains byte-identical to the preceding benchmark.

Seven randomized mirrored ABBA/BAAB blocks, fresh processes, two warmups and three measured rounds per process; identical calibrated iterations within each pair. Times are medians of process medians; reductions are medians of seven paired reductions, so displayed-time ratios can differ. Every measured and warmup scalar must exactly match the independent integer/float32-tree oracle.

| Residency | Size | Baseline ms | Current ms | Paired time reduction | Block range |
|---|---:|---:|---:|---:|---:|
| resident | 128² | 0.318 | 0.331 | -1.3% | -9.6% to 5.8% |
| resident | 256² | 0.364 | 0.373 | -1.7% | -28.6% to 7.2% |
| resident | 512² | 1.388 | 0.669 | 55.5% | 44.6% to 61.3% |
| resident | 1024² | 2.566 | 2.024 | 19.3% | -3.0% to 24.3% |
| resident | 2048² | 19.174 | 13.598 | 29.5% | 27.8% to 30.5% |
| transfers | 128² | 0.903 | 0.919 | -0.5% | -11.6% to 4.8% |
| transfers | 256² | 1.030 | 1.016 | 2.5% | -2.6% to 13.3% |
| transfers | 512² | 2.295 | 1.448 | 36.6% | 35.5% to 37.7% |
| transfers | 1024² | 4.703 | 4.181 | 10.6% | 7.1% to 17.0% |
| transfers | 2048² | 26.311 | 20.461 | 21.3% | 18.8% to 24.5% |

Resident excludes input uploads; transfers includes all three uploads. Scheduling, fresh output/intermediate allocations, scalar completion/readback and cleanup are included. Initialization, compilation and shader loading are excluded. There is no buffer pool, zero-copy shortcut or cached answer. Profiling and GPU validation are disabled for timing. No affinity, frequency or exclusive host isolation is imposed.

The new path is not selected at the 128²/256² control sizes. Their small median variations and larger individual outliers are retained rather than described as improvements. One 1024² resident block regresses by 3.0%; the seven-block median improves. All seven 2048² resident blocks improve by 27.8–30.5%.

## Optimized-library and CPU controls

These are numerical API comparisons, not source-level compiler rankings. Every implementation performs GEMM, bias, scale, ReLU and an adjacent-pair sum. Libraries may use different matrix arithmetic, layouts and hardware; their post-processing/reduction is in place. Dyadic timed matrix inputs make each matrix entry exact, and each timed scalar is checked against the common independent float32-tree oracle. Three shuffled fresh-process trials, two warmups and five measured rounds per trial. OpenBLAS thread requests match the verified OpenMP limit; Accelerate SINGLE/AUTO is set and queried through its API, without claims about internal accelerator dispatch.

| Size | Kalandra CPU ms | GPU resident ms | GPU transfers ms | OpenBLAS 1 ms | OpenBLAS 12 ms | Accelerate SINGLE ms | Accelerate AUTO ms |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 128² | 0.128 | 0.331 | 0.919 | 0.071 | 0.082 | 0.010 | 0.010 |
| 256² | 0.927 | 0.373 | 1.016 | 0.450 | 0.211 | 0.048 | 0.048 |
| 512² | 6.865 | 0.669 | 1.448 | 2.805 | 0.792 | 0.266 | 0.201 |
| 1024² | 56.541 | 2.024 | 4.181 | 23.794 | 6.337 | 1.977 | 1.174 |
| 2048² | 440.365 | 13.598 | 20.461 | 174.732 | 37.072 | 15.090 | 8.814 |

At 2048², resident GPU is 32.4× faster than Kalandra CPU and 2.7× faster than 12-thread OpenBLAS, but takes 54% more time than Accelerate AUTO (13.60 versus 8.81 ms). GPU remains slower than Kalandra CPU at 128². This does not establish universal GPU placement thresholds or solve the remaining optimized-library gap.

## Profile and candidate selection

Instrumented samples are separate from acceptance timing. Submissions remain one per pipeline and dispatch counts remain 4/5/5/5/6 across the five sizes. At 2048², summed device duration falls from about 18.30 to 12.77 ms; CPU encoding is about 66 to 62 µs. The remaining large-case cost is chiefly GPU work, while fixed host/submission cost is more prominent at small sizes. These separate samples are not an additive decomposition of uninstrumented wall time.

Two randomized three-process screening studies test 16 distinct alternatives plus the old tile, with every output cell and a poisoned suffix guard checked outside GPU timestamps. They include smaller indexed root storage, explicit scalar roots, 8×8 through 32×32 tiles, multiple output cells per lane and five SIMD-cooperative reductions that preserve the same tree. The 16×16/256-thread candidate wins both studies. These are preallocated raw-kernel experiments, not language throughput scores. They do not establish a physical register-spill diagnosis.

| Extended screening, 2048² | Median GPU ms |
|---|---:|
| register_16_16_256 | 12.838 |
| wide_16_32_256 | 14.730 |
| cooperative_16_16_2 | 15.829 |
| wide_32_32_512 | 17.198 |
| cooperative_16_8_2 | 17.205 |
| baseline | 17.838 |
| wide_32_32_256 | 18.924 |
| cooperative_16_8_4 | 22.329 |
| cooperative_8_8_4 | 24.499 |
| cooperative_4_4_8 | 33.297 |

A separate three-block mirrored boundary screen uses the actual production entry on ten tall, wide, odd and maximum-contraction shapes. All ten paired medians improve, by 15.7–43.4%; every individual block remains positive, with considerable variation. This screen checks policy edges, not every possible shape. Rejected alternatives and all samples are retained.

## Qualification

- 23 distinct selected CTest gates pass: four software matrix/matrix-reduction/graph configurations across five fixture families, hardware pipeline, all-type GPU ReLU and the evidence/oracle tests. After adding the new preparation-failure hook, the final hardware pipeline and ReLU gates pass again against the final runtime; unchanged software paths retain their preceding regression results.
- Four host build modes (optimized, debug, ASan/UBSan, TSan) each pass 51 large matrix cases and 3,362,388 complete-cell APFloat checks, using 21,343,296 independent reference products. Periodic input phases permit independent references for every output cell without mirroring the GPU tile. Finite values are exact; arithmetic NaNs use classification equivalence. Non-dyadic values, explicit negative zero, subnormals, cancellation-sensitive magnitudes, non-power-of-two block folds, K=4096 full root capacity, and K=4160/511/513/small-output fallbacks are covered. Inputs are downloaded and checked unchanged.
- All five kernel-preparation failure checkpoints and execution allocation failures pass in each mode. Submitted work retains resources through held cancellation, deadlines and injected device failure; cleanup is balanced. Host sanitizers do not instrument GPU instructions.
- Each mode also retains the 38 complete projection cases and 5,593,265 exact matrix/post-map cell comparisons at the five benchmark sizes, plus 28 allocation-failure positions, tight-budget staged fallback and cancellation/completion tests. Together the new matrix and pipeline harnesses perform 35,822,612 numerical cell checks across the four modes. Final hardware gates enable Metal API and shader validation.
- GPU ReLU still passes all ten types: 160 type/shape cases and 23,520 raw-bit checks per mode. The numerical evidence/oracle suite passes six tests. Forty optimized-library cases check 11,496 output cells against independent Decimal references and explicit error bounds before timing, including non-dyadic inputs.
- An initial local run used the preceding linked compiler; it is excluded from new-kernel qualification. The test runner now requires the new register-kernel symbol in generated MSL, and all final runs pass that check.

Sources, artifacts, commands, logs and limits are retained in [raw pipeline timing](large-matrix-pipeline-2026-10-25.json), [qualification](large-matrix-qualification-2026-10-25.json), [initial screening](large-matrix-screening-2026-10-25.json), [extended screening](large-matrix-extended-screening-2026-10-25.json) and [boundary screening](large-matrix-boundaries-2026-10-25.json). Screening snapshots embed their generator, driver and generated MSL for development-stage reproducibility.

## Reproduction and limits

Build and retain the optimized Metal archive at baseline commit `515294a2`, then build the current native compiler and all four Metal runtime variants. With the pinned Apple toolchain and Homebrew OpenBLAS installed:

```sh
python3 native/benchmarks/pipeline_run.py \
  --baseline-metal /path/to/baseline-metal.a \
  --output /path/to/unused-results.json
```

For qualification run `ctest --test-dir build/native --output-on-failure -R "^native-metal-(pipeline|relu)$"`. For screening, `matrix_kernel_study.py --suite initial|extended --output ...` builds the candidate library/driver; `matrix_kernel_boundaries.py --library /path/to/current/kernel0.metallib --output ...` compares the two production entries at policy edges. Output paths must be unused. Performance runs reject profiling/validation/injection environments; cache reuse verifies all recorded input/artifact hashes.

Physical qualification is this Apple M2 Max on macOS 27; deployment still targets macOS 26. Other devices keep their existing selection pending their own measurements. Square complete-pipeline performance is measured through 2048²; boundary qualification tests contraction lengths through and beyond the specialization limit, but this is not a full model/attention/LLM benchmark. Broadcasting, buffer reuse, epilogue fusion, automatic placement and broader device tuning are unchanged. The remaining large-kernel and Accelerate gap is explicitly open.
