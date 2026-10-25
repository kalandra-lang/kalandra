# Complete float32 GPU projection pipeline — 2026-10-25

The workload is `reduceSum(relu((matmul(a,b)+bias)*scale))`, with full-shape bias and runtime inputs. This pass adds tensor ReLU and batches an eligible hardware float32 matrix/post-map/reduction suffix into one command submission. The matrix arithmetic and dispatch count are unchanged. This is command-buffer batching, not a single fused matrix kernel.

## Paired runtime comparison

Apple M2 Max, macOS 27. The frozen Metal source matches commit `e67c988910c655275dd8608c488d999856bd7f86`. Both versions use the same newly compiled ReLU shaders, benchmark driver and optimized task runtime; only the Metal archive differs. The old frontend did not support ReLU, so this isolates the runtime change rather than comparing entire old/new compiler releases.

Seven randomized mirrored ABBA/BAAB blocks, fresh processes, two warmups and three measured rounds per process. Iterations are calibrated identically within each pair. Times below are medians of process medians; reductions are medians of the seven paired reductions, so they need not exactly equal the ratio of the displayed times. All rounds, including warmups, must match the independent integer/tree scalar oracle exactly.

| Residency | Square size | Before ms | After ms | Paired time reduction | Block range |
|---|---:|---:|---:|---:|---:|
| resident | 128² | 0.511 | 0.350 | 31.4% | 25.6% to 38.4% |
| resident | 256² | 0.563 | 0.378 | 27.8% | 22.0% to 33.9% |
| resident | 512² | 1.608 | 1.400 | 12.2% | 4.2% to 14.3% |
| resident | 1024² | 2.737 | 2.578 | 6.1% | 3.9% to 8.0% |
| resident | 2048² | 19.679 | 19.439 | 0.7% | 0.5% to 3.9% |
| transfers | 128² | 1.094 | 0.911 | 17.2% | 10.1% to 20.4% |
| transfers | 256² | 1.179 | 1.057 | 10.6% | -14.6% to 13.9% |
| transfers | 512² | 2.631 | 2.369 | 7.5% | 2.9% to 12.2% |
| transfers | 1024² | 5.087 | 4.795 | 4.0% | 1.3% to 8.2% |
| transfers | 2048² | 27.241 | 26.284 | 2.5% | 1.0% to 6.3% |

Resident inputs exclude upload; transfers includes three uploads per invocation. Both include scheduling, fresh output/intermediate allocation, scalar completion/readback and cleanup. Initialization, shader loading and compilation are excluded. There is no buffer pool or allocation reuse in this pass. Batching retains earlier owners until acknowledgement and therefore uses more peak workspace; a conservative byte-budget preflight retains the staged path under tight budgets.

The gain is primarily submission/host-boundary overhead. Resident 2048² improves only 0.7% by the paired median. The transfer-inclusive 256² case has one regressing block (−14.6%); it is retained. No hardware arithmetic, rounding, checked faults, cancellation or ownership guarantees were removed.

## Complete workload controls

These are numerical API comparisons, not source-level language/compiler rankings. All compute GEMM, add bias, scale, ReLU and a balanced adjacent-pair sum. BLAS implementations may use different matrix arithmetic, layouts and hardware. Timed dyadic matrix inputs make each matrix entry exact; every timed scalar is checked against an independent float32 reduction-tree oracle. Libraries post-process and reduce their matrix output in place; Kalandra owns its lowering/allocation strategy. Three shuffled fresh-process trials, two warmups and five measured rounds per trial.

| Size | Kalandra CPU ms | GPU resident ms | GPU transfers ms | OpenBLAS 1 ms | OpenBLAS 12 ms | Accelerate SINGLE ms | Accelerate AUTO ms |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 128² | 0.128 | 0.350 | 0.911 | 0.073 | 0.088 | 0.010 | 0.010 |
| 256² | 0.932 | 0.378 | 1.057 | 0.449 | 0.245 | 0.049 | 0.049 |
| 512² | 7.001 | 1.400 | 2.369 | 2.901 | 1.093 | 0.268 | 0.217 |
| 1024² | 57.560 | 2.578 | 4.795 | 23.554 | 8.757 | 2.126 | 1.344 |
| 2048² | 462.475 | 19.439 | 26.284 | 174.457 | 47.304 | 16.031 | 10.537 |

At 2048², resident GPU is 23.8× faster than Kalandra CPU and about 2.4× faster than 12-thread OpenBLAS, but Accelerate AUTO is about 1.8× faster than Kalandra GPU. At 128², Kalandra CPU is faster than GPU. For this measured workload the GPU crossover occurs at 256² for resident inputs and 512² with uploads. These crossovers are not universal workload thresholds.

The remaining large-case bottleneck is matrix device work: the separately instrumented 2048² resident run spends about 18.0 ms on the GPU within about 19.4 ms uninstrumented wall time. Instrumented and uninstrumented samples are separate, so this is an indication, not an additive decomposition. Submission batching does not solve the matrix-kernel gap to Accelerate.

OpenBLAS requested counts are checked through its API; for the installed OpenMP build, its OpenMP maximum and dynamic-team setting are also checked. Accelerate uses its SINGLE or automatic threading API and the mode is queried back; no claim is made about its internal accelerator dispatch. The first trial incorrectly restricted OpenMP to one while requesting 12 OpenBLAS threads; it stalled in GEMM and was stopped. The whole timing trial was discarded and repeated after fixing the environment. A negative test now rejects that contradiction before GEMM; the matching 12-thread case completes.

## Separate profiling

| Size | Submissions before → after | Dispatches | GPU duration before → after ms | CPU encoding before → after µs |
|---:|---:|---:|---:|---:|
| 128² | 2 → 1 | 4 | 0.091 → 0.074 | 41.5 → 30.0 |
| 256² | 2 → 1 | 5 | 0.124 → 0.151 | 38.1 → 32.7 |
| 512² | 2 → 1 | 5 | 0.996 → 1.004 | 40.7 → 32.9 |
| 1024² | 2 → 1 | 5 | 2.011 → 2.002 | 45.3 → 38.6 |
| 2048² | 2 → 1 | 6 | 17.775 → 17.991 | 64.4 → 57.7 |

Profiling is disabled during acceptance timing. Device timestamps vary and do not establish kernel acceleration; the device kernels are identical in the pair.

## Correctness and language behavior

- ReLU supports all ten numeric types and tensor ranks 0–8 on CPU and Metal. Floating NaN bits/payloads and positive subnormals are preserved; negative values and either signed zero become positive zero. Signed minimum integers are safe; unsigned values are unchanged. Float64 ReLU selects integer bits and requires no emulated floating arithmetic. Other float64 Metal math retains its explicit fallback policy.
- CPU ReLU: 6,831 bit-exact cells per type/build across ten types and four modes. GPU ReLU: 23,520 bit-exact cells in 160 type/shape cases per build across four modes. Debug, optimized, ASan/UBSan and TSan hosts pass. Explicit software policy passes optimized CPU/GPU controls. Invalid scalar/array/boolean/arity calls are rejected without replacing existing outputs.
- Pipeline: 38 APFloat/integer-reference cases and 5,593,265 exact cells per build across four modes, including every matrix/post-map cell at all five benchmark sizes. Tests cover odd/empty axes, non-dyadic values, infinities, NaNs, subnormals, a one-slot in-flight limit, the 512-byte staged fallback, 28 allocation-failure positions, held cancellation/deadline/completion failure and balanced cleanup.
- Optimized libraries: 40 independent Decimal-reference cases, 11,496 output cells, including non-dyadic inputs, checked against explicit forward-error bounds. Six evidence/oracle unit tests pass, including float32 rounding ties, independent cubic pipeline references and corrupt-output rejection.
- 76 distinct focused CTest gates pass after reruns. The first run had five timeouts; all five pass unchanged on rerun. Their cause is not established, and the initial timeout output is retained in qualification evidence.
- Source shape checks exposed and fixed a pre-existing rank-zero Metal host-helper bug: scalar rank zero and tensor rank zero now retain their distinct identities. CPU/GPU source checks cover float32/float64 ranks 0, 3 and 8.
- Website build and runnable documentation verification pass: 40 native examples, 64 rendered blocks, 33 CPU programs and two GPU programs. Windows-only invocation is not executed on this Mac; source-build recipes receive syntax checks.

See [ReLU contract](../../docs/design/relu.md), [raw timing evidence](gpu-pipeline-2026-10-25.json) and [qualification evidence](gpu-pipeline-qualification-2026-10-25.json). Raw timing evidence includes all observations, command arguments, source/artifact hashes and tool versions. Compilation uses the existing hardware policy with contraction disabled; GPU validation is used for conformance and disabled for timing.

## Reproduction and limits

Build and retain `libkalandra-metal-optimized.a` from the baseline revision, then build the current native compiler, Metal archive and optimized task runtime. The baseline source hash is recorded and checked against Git. The benchmark compiles the current `Pipeline.k` once and links identical driver sources against the two Metal archives.

```sh
python3 native/benchmarks/pipeline_run.py \
  --baseline-metal /path/to/baseline-metal.a \
  --output /path/to/new-unused-results.json
```

Requires the pinned native/Metal toolchain and Homebrew OpenBLAS on Apple Silicon. Clear GPU profiling, Metal debug/shader validation and DYLD injection settings before timing. `--build-only` prepares artifacts without measuring; `--reuse-build` verifies all cached input/artifact hashes. The result path and its Markdown sibling must be unused.

Qualification is local to this M2 Max/macOS 27 machine; deployment still targets macOS 26. There is no affinity/frequency control or exclusive host isolation. This is one square float32 workload family, not attention, softmax, GELU, a full LLM, implicit bias broadcasting or general graph batching. No extra primitive beyond ReLU was needed to finish this measured pipeline. The next useful optimization is the strict matrix kernel at large sizes, followed by reducing allocation/transfer overhead where it dominates.
