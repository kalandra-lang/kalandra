# Strict GPU matrix epilogue fusion — 2026-10-25

Eligible float32 hardware matmul now applies its result arithmetic before storing each cell. The measured workload is `reduceSum(relu((matmul(a,b)+bias)*scale))`, including fresh managed output/scratch allocation, scheduling, dispatch, scalar readback and cleanup. Bias matches the full output shape. Resident inputs and explicit three-upload calls are reported separately.

Two complete runs show a paired median resident time reduction of **3.0–4.1% at 2048²** and **8.3–9.3% at 1024²**, with regressing blocks retained. Each eligible call removes one dispatch and one full matrix allocation. At 2048² this saves **16 MiB**, reducing measured total runtime-budget peak from **128.126 MiB to 112.126 MiB** (12.5%). This is budget accounting, including host wrappers and initial upload staging, not RSS or physical GPU-memory measurement. Small/medium timings and upload-inclusive results are substantially noisier; no blanket speedup is claimed.

## Implementation and arithmetic

The compiler marks matrix post-maps, then lifts only pinned SPIRV-Cross canonical straight-line, same-cell float32 code into an optional helper. It checks buffer layouts, indexing and the final output store; unsupported code, control/indexing, sources above 16 KiB and more than 27 physical reads keep the staged kernel. The 27-read bound leaves four extra bindings within Metal's 31 direct slots. Larger direct/Tier 2 signatures still work. The standalone entry remains in the bundle.

The helper is inserted at the final store of each existing hardware matrix variant. Matrix products and their adjacent-pair sums remain separately rounded; post arithmetic also retains its original order. Both builds use `-fno-fast-math -ffp-contract=off`. ReLU retains the existing signed-zero/NaN selection. No FMA, reassociation, broadcasting, vendor matrix API or Accelerate integration is added.

The runtime preserves the existing device/shape policy, reflects the extra bindings and validates captured inputs before encoding. One owned post-parameter block and the kernel/capture owners remain alive through GPU acknowledgement. Invalid captures, byte/in-flight budgets, checked allocation, cancellation, deadlines and device-failure cleanup remain enabled. Older bundles without optional entries use the staged path. Pre-mapped operands, graphs, numeric programs, scalar epilogues, software policy and other numeric types retain their previous path. Empty outputs and empty contractions retain their numerical behavior.

Preparation adds four optional pipelines per eligible post library. Cold initialization/loading is outside this throughput study; startup speed is not claimed to improve.

## Complete-pipeline timings

The frozen baseline is commit `199bd2dfdfd6a3548fb711ba3d2a9e5e199a8cd5`; its Metal archive SHA-256 is `952ef8341d4ea93e81caed8e89e777b3ebf9c58a544db41267f4bf19517b36d1`, matching the preceding qualified report. Both versions use identical current shaders, driver and task archive. The old runtime ignores optional fused entries. Matrix/reduction MSL and the standalone post body are unchanged; CPU pipeline LLVM is byte-identical to the preceding study.

Each run has seven randomized mirrored ABBA/BAAB blocks, fresh processes, two warmup rounds and three measured rounds per process, with equal calibrated iterations per pair. Times below are medians of process medians. Reductions are medians of seven paired reductions; their ranges include every block. These different summaries need not equal the ratio of displayed times. Profiling and Metal validation are disabled for acceptance timing.

The first run contained large resident outliers at 512²/1024². The second repeats the entire unchanged protocol, including controls, rather than selecting favorable cells. Both runs and all samples are retained. The repeat reduces resident variation, but does not eliminate small/medium or transfer outliers. No affinity, fixed frequency or host isolation is imposed.

| Path | Square size | First baseline → fused (ms) | First paired reduction (range) | Repeat baseline → fused (ms) | Repeat paired reduction (range) |
|---|---:|---:|---:|---:|---:|
| resident | 128 | 0.437 → 0.410 | +9.6% (-9.4 to +11.5) | 0.414 → 0.354 | +7.8% (+3.0 to +20.0) |
| resident | 256 | 0.371 → 0.362 | +1.5% (-17.5 to +11.9) | 0.359 → 0.349 | +3.7% (-17.6 to +7.7) |
| resident | 512 | 0.617 → 0.582 | +3.6% (-58.2 to +16.2) | 0.534 → 0.494 | +6.7% (+0.5 to +12.5) |
| resident | 1024 | 2.076 → 1.859 | +9.3% (-47.9 to +17.3) | 2.049 → 1.881 | +8.3% (-0.2 to +11.8) |
| resident | 2048 | 13.661 → 13.187 | +4.1% (+2.6 to +5.2) | 13.490 → 13.027 | +3.0% (+2.7 to +4.3) |
| transfers | 128 | 0.988 → 0.998 | -2.6% (-6.4 to +2.0) | 0.857 → 0.842 | +4.6% (-3.7 to +20.3) |
| transfers | 256 | 1.085 → 1.072 | +0.7% (-9.3 to +6.8) | 1.049 → 1.017 | +1.3% (-5.9 to +6.0) |
| transfers | 512 | 1.859 → 1.960 | +1.8% (-18.8 to +6.8) | 1.476 → 1.463 | +0.8% (-30.4 to +3.6) |
| transfers | 1024 | 4.417 → 4.499 | +0.5% (-18.1 to +5.8) | 4.088 → 4.060 | +1.5% (-34.1 to +6.5) |
| transfers | 2048 | 20.763 → 20.146 | +2.1% (-10.1 to +3.9) | 19.990 → 19.187 | +3.5% (+2.9 to +4.4) |

## Mechanism and controls

Separate profiling runs confirm the same mechanism in every round, in both complete runs and both residency modes. Resident submissions remain one per call; upload-inclusive calls remain four (three uploads plus compute). Dispatches and total process-lifetime runtime-budget peaks below are per complete call/process respectively. Peak accounting includes resident host/device inputs and initialization, so it is not an isolated per-call allocation counter.

| Square size | Dispatches baseline → fused | Budget peak baseline → fused (MiB) | Saved (MiB) |
|---:|---:|---:|---:|
| 128 | 4 → 3 | 0.500523 → 0.438023 | 0.0625 |
| 256 | 5 → 4 | 2.002003 → 1.752003 | 0.2500 |
| 512 | 5 → 4 | 8.007908 → 7.007908 | 1.0000 |
| 1024 | 5 → 4 | 32.031528 → 28.031528 | 4.0000 |
| 2048 | 6 → 5 | 128.126015 → 112.126015 | 16.0000 |

The first run's complete numerical API controls follow (ms). Each uses three processes, two warmups and five measured rounds; repeat controls are retained in the second raw file. Libraries perform bias/scale/ReLU and balanced reduction in place. Different GEMM arithmetic, layout and internal hardware are allowed; the timed dyadic inputs have exact matrix entries. These compare operation pipelines, not equivalent source-level algorithms or language/compiler rankings.

| Square size | Fused GPU resident | Kalandra CPU | OpenBLAS 1 thread | OpenBLAS 12 threads | Accelerate 1 thread | Accelerate AUTO |
|---:|---:|---:|---:|---:|---:|---:|
| 128 | 0.410 | 0.129 | 0.073 | 0.090 | 0.010 | 0.010 |
| 256 | 0.362 | 0.950 | 0.452 | 0.221 | 0.049 | 0.049 |
| 512 | 0.582 | 6.991 | 2.842 | 0.846 | 0.267 | 0.206 |
| 1024 | 1.859 | 55.995 | 22.819 | 7.222 | 1.986 | 1.351 |
| 2048 | 13.187 | 441.052 | 172.526 | 41.874 | 16.097 | 9.558 |

At 2048² in the first run, resident GPU is 33.4× faster than Kalandra CPU but still 38.0% slower than Accelerate AUTO. At 128² Kalandra CPU remains faster than the GPU call. Initialization/compilation is excluded from all throughput claims. Accelerate remains solely a benchmark reference; production uses our own kernels.

## Qualification and reproduction

- All 44 selected CTest gates pass: codegen rejection/acceptance tests, evidence/oracle tests, ten matrix/graph/indirect fixture families in four modes, complete hardware pipeline and all-type GPU ReLU. Metal API validation is enabled; the final hardware pipeline/ReLU gates also enable shader validation.
- The expanded final pipeline gate passes again after adding non-dyadic cases for the 32-leaf and partial 16×16 fused variants. In each of optimized/debug/ASan+UBSan/TSan, current and simulated legacy bundles each pass 40 pipeline cases and 5,659,603 output-cell references. The unchanged matrix harness adds 51 cases and 3,362,388 APFloat output-cell checks per mode. Together these final harnesses perform **58,726,376 numerical output-cell checks**, plus **138,349,680 raw-bit input-immutability checks** in the pipeline harnesses. Arithmetic NaNs use classification equivalence; ReLU separately checks payload preservation. Host sanitizers do not instrument GPU instructions.
- Pipeline tests cover non-dyadic inputs, nonfinite values, signed zeros/subnormals, empty axes/contractions, scalar parameters, 27-read fusion, larger direct/Tier 2 fallback and legacy libraries. Invalid/unused host, missing and mismatched captures are rejected; an empty matrix-replacement placeholder remains valid. Dispatch counts prove selection/fallback. Each mode sweeps all six post-loader preparation checkpoints per current runtime (two for legacy), 23 current execution-allocation failures versus 28 legacy, and submitted cancellation/deadline/device failure with balanced cleanup. The matrix harness also retains five preparation failure checkpoints and its admission/root-capacity tests.
- GPU ReLU retains all ten numeric types: 160 type/shape cases and 23,520 raw-bit checks per mode. Each timing run first qualifies 40 optimized-library cases (11,496 cells) against independent 100-digit Decimal references and explicit error bounds. Every measured and warmup round checksum matches the independent integer/float32-tree oracle.

Build baseline/current optimized Metal archives and the current compiler with the pinned toolchain, then run:

```sh
python3 native/benchmarks/pipeline_run.py \
  --baseline-metal /path/to/199bd2df-metal.a \
  --output /path/to/unused-results.json
ctest --test-dir build/native --output-on-failure \
  -R '^(native-matmul-epilogue-codegen|native-metal-(pipeline|relu))$'
```

`--build-only` followed by `--reuse-build` separates compilation from quiet measurement and verifies all input/artifact hashes. Raw evidence records commands, environment/tool versions, sources, generated code and artifacts. Physical qualification is Apple M2 Max/macOS 27; deployment targets macOS 26. Other hardware needs physical measurement. Broader graphs, buffer reuse, automatic placement and full model/attention/LLM performance remain open.

Evidence: [first complete run](epilogue-pipeline-2026-10-25.json), [full confirmation](epilogue-pipeline-confirmation-2026-10-25.json), [qualification and logs](epilogue-qualification-2026-10-25.json), [design](../../docs/design/apple-gpu.md#float32-matrix-epilogue-fusion).
