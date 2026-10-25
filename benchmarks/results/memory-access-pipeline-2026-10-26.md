# GPU memory access and grouped matrix traversal — 2026-10-26

The production follow-up changes **which output tile a workgroup computes**, preserving the original arithmetic and input representation. Two complete paired runs reduce 2048² resident pipeline median time by **4.88% and 4.89%**, and upload-inclusive time by **3.22% and 4.46%**. Selected rectangular diagnostics gain more, up to about 23%. No packed input copy, extra dispatch or per-invocation workspace is introduced. Small-input timings retain regressions; this is not a universal speedup claim.

Physical qualification is Apple M2 Max/macOS 27.2, targeting macOS 26. The new entry is enabled only in the existing M2 Max hardware-float32 matrix profile. Other devices and unsupported shapes keep their previous entries. No Accelerate or other vendor library is integrated.

## What caused the packing losses?

The [preceding packing study](weight-packing-investigation-2026-10-25.md) found that square-case gains did not generalize. This investigation separates construction from execution and changes access order without changing arithmetic:

- **Copies are too small to explain the large losses by themselves.** Public encoder timestamps put per-shape median packing fractions at roughly 0.68–1.03% of sampled pack+matrix ticks for B-only, and 2.03–2.75% for both inputs on the sampled shapes. These are compute-stage fractions, not whole-command wall time. They do not exclude cache-state effects induced by the copy.
- **Compact per-workgroup accesses are not sufficient.** Packing makes a panel contiguous but changes cross-panel strides and how nearby groups reuse operands. A 16-row-tile grouped order improves the original, unpacked kernel in paired tests, establishing that output-tile assignment affects execution cost even with identical arithmetic, loads per tile and storage lengths.
- **Reuse trades against address dispersion.** For the first K=64 panel of 32 logical workgroup IDs at 2048², the model requests 256 KiB total in either order. Distinct useful input bytes fall from 132 KiB to 72 KiB, while distinct illustrative 4-KiB address regions increase from 80 to 320. This offers a reason to test a size-dependent policy, not proof of physical cache/TLB misses. Actual execution order is not guaranteed to follow logical IDs.

The model exhaustively checks the tile-ID permutation for 1,690 row/column-grid combinations and 5,688,020 tile IDs, including partial final row groups. It models address sets, not hardware scheduling, cache geometry, memory transactions or timing. The 128-byte buckets and 4-KiB regions are analytical units, not asserted hardware cache/page sizes. GPU complete-cell checks independently cover the actual shader mapping.

The public counter API exposes timestamps on this device. A targeted Instruments capture additionally exposes 31 definitions, including buffer-load utilization, cache limiter, occupancy and TLB miss metrics. However, the exported per-kernel profile table is empty and value samples are GPU-global, including other applications. We therefore do **not** attribute those counters to our shader or claim a particular cache-conflict, bandwidth, register-spill or TLB mechanism has been proved. The [capture capability record](memory-access-trace-capability-2026-10-26.json) preserves this limitation and exported hashes; large raw traces stay in the ignored build directory.

## Controlled order experiments

The first screen compares row-major, column-major and 4/8/16/32-row-tile grouping, plus unchanged block/both packed controls and grouped packed controls. It uses two randomized processes, two warmups and three measured rounds per cell. That unpaired screen is noisy and is not acceptance evidence. All outputs, suffix guards, packed contents/padding and input immutability are checked outside timing. Exact dyadic inputs avoid changing the reference because of floating-point reassociation.

Seven mirrored ABBA/BAAB blocks then compare the original kernel against unconditional 16-row-tile grouping, with equal calibrated iterations targeting 20 ms GPU work per round. Positive reductions mean faster; parentheses retain the complete observed block range, not a confidence interval.

| M × K × N | Paired GPU time reduction |
|---|---:|
| 512 × 512 × 512 | -1.69% (-2.66…+0.89%) |
| 1024 × 1024 × 1024 | -0.27% (-0.52…-0.07%) |
| 1024 × 1024 × 2048 | +2.14% (+1.61…+3.01%) |
| 1024 × 4096 × 1024 | +11.89% (+4.80…+13.77%) |
| 1025 × 576 × 1027 | -0.89% (-18.15…-0.27%) |
| 1088 × 1024 × 1024 | -0.08% (-3.29…+0.90%) |
| 1536 × 1536 × 1536 | +5.45% (+3.54…+5.89%) |
| 2048 × 2048 × 2048 | +5.64% (+4.34…+6.97%) |

Unconditional grouping is rejected: small and odd cases lose. An inline shader selection branch also shows small-case overhead, so production selects the optional entry on the host instead.

The host policy requires existing register-kernel eligibility (512≤K≤4096 and K divisible by 64), M,N between 512 and 65,535, at least 1,048,576 output cells and at least 16 MiB combined float32 input storage. These are empirical performance limits, not language limits or an assertion of cache capacity. The upper bounds keep the new shader's 32-bit tile/index arithmetic safe. Other shapes retain the original shader itself, rather than running a new shader with a fallback branch.

The following five-block host-dispatch confirmation spans admission thresholds, rectangular/deep cases and partial tiles. Ineligible controls intentionally use the exact original GPU entry. Some small gains and losses overlap the variation in those controls; not every eligible case has a significant win.

| M × K × N | Grouped entry selected | Paired GPU time reduction |
|---|:---:|---:|
| 256 × 4096 × 4096 | no | +0.08% (-0.51…+0.29%) |
| 512 × 512 × 512 | no | -0.02% (-0.04…+0.76%) |
| 512 × 4096 × 2048 | yes | +22.72% (+20.36…+24.36%) |
| 513 × 4096 × 2049 | yes | +22.80% (+21.56…+23.88%) |
| 1024 × 1024 × 1024 | no | -2.37% (-2.88…+1.99%) |
| 1024 × 1984 × 1024 | no | +0.22% (-0.34…+1.99%) |
| 1024 × 2048 × 1024 | yes | +4.55% (+4.42…+9.14%) |
| 1024 × 2112 × 1024 | yes | +10.62% (+9.31…+13.47%) |
| 1024 × 4096 × 2048 | yes | +23.12% (+19.19…+24.14%) |
| 1025 × 2048 × 1027 | yes | +6.56% (+4.45…+8.34%) |
| 1536 × 1984 × 1024 | yes | +2.86% (-2.60…+5.55%) |
| 2048 × 1024 × 2048 | yes | +2.10% (-2.50…+2.65%) |
| 2048 × 4096 × 512 | yes | +9.17% (+3.01…+11.41%) |
| 2048 × 4096 × 1024 | yes | +10.60% (+8.09…+12.47%) |
| 2049 × 4096 × 513 | yes | +10.26% (+8.70…+10.64%) |
| 4096 × 4096 × 256 | no | +0.40% (-1.35…+1.97%) |

These preallocated diagnostics include fresh GPU construction for the packed controls, but exclude allocation, initial uploads and shader loading. They are not complete managed language throughput. All earlier failed layouts and the rejected inline policy remain in the raw evidence.

## Complete numerical pipeline

The acceptance workload is `matmul → bias → scale → ReLU → balanced reduceSum`. Fresh result/reduction allocation, scheduling, dispatch, scalar completion/readback and cleanup are included. Transfers mode additionally uploads A, B and bias. Compilation, loading and initial resident uploads are excluded. Both executables use the same new bundle, with the frozen preceding qualified Metal archive selecting the unchanged earlier entries and the new archive selecting the optional grouped entry. The original matrix and epilogue MSL remain unchanged prefixes of the generated source. CPU LLVM is byte-identical to the preceding epilogue study.

Each run has seven randomized mirrored ABBA/BAAB blocks, two warmup rounds and three measured rounds per process, with identical calibrated iteration counts per pair. The table's milliseconds are medians of process medians from run 1; paired reductions are medians of per-block ratios and need not equal the ratio of the marginal millisecond medians.

| Mode | Square N | Old ms, run 1 | New ms, run 1 | Run 1 paired reduction (range) | Run 2 paired reduction (range) |
|---|---:|---:|---:|---:|---:|
| resident | 128 | 0.310 | 0.314 | -4.29% (-7.24…+7.44%) | -2.74% (-6.82…+0.33%) |
| resident | 256 | 0.353 | 0.341 | +2.03% (-1.25…+9.45%) | -0.59% (-5.30…+0.44%) |
| resident | 512 | 0.504 | 0.494 | -1.06% (-13.14…+14.50%) | -2.16% (-25.45…+5.38%) |
| resident | 1024 | 1.824 | 1.836 | -0.55% (-6.91…+2.06%) | +0.36% (-0.80…+2.00%) |
| resident | 2048 | 13.050 | 12.396 | +4.88% (+2.51…+5.92%) | +4.89% (+4.68…+6.48%) |
| transfers | 128 | 0.771 | 0.794 | -4.87% (-17.64…+8.75%) | +2.96% (-7.37…+6.04%) |
| transfers | 256 | 0.968 | 0.975 | -0.26% (-3.47…+12.82%) | +1.43% (-5.97…+9.55%) |
| transfers | 512 | 1.390 | 1.430 | -2.42% (-7.71…+2.88%) | +0.90% (-1.14…+7.20%) |
| transfers | 1024 | 3.836 | 3.945 | -1.65% (-4.27…+15.37%) | -0.45% (-4.95…+6.13%) |
| transfers | 2048 | 19.699 | 18.984 | +3.22% (+1.96…+6.38%) | +4.46% (+3.78…+5.67%) |

Every 2048² block improves in both modes/runs. At smaller sizes the new entry is not selected, yet the complete host/driver timings still vary and include regressions: resident 128² is 2.74–4.29% slower by paired median, and resident 512² loses 1.06–2.16%. Keeping the old GPU shader does not prove the complete runtime has identical latency; additional runtime code, CPU/driver scheduling and GPU frequency/state remain possible influences. These losses are unresolved and retained. This desktop run is not isolated from other applications, and no affinity/frequency control is imposed.

Profiles are separate diagnostic runs with profiling enabled, not the acceptance timing. At 2048², both versions retain **one submission and five compute dispatches resident**, or four submissions including uploads. Both report **112.126 MiB** process-lifetime budget peak. There is no new per-invocation buffer or packed-weight cache. The benchmark explicitly uses a 512 MiB budget as before; the production default remains 64 MiB. Two extra optional pipeline objects (plain and fused) increase cold preparation/driver storage and are excluded from throughput timing; budget peak is not RSS or all driver memory.

Independent numerical-API controls from run 1 are retained below (ms). They use equivalent numerical intent but may choose different reduction/GEMM arithmetic and hardware; this is not a Rust/C++ compiler ranking, optimized-library parity claim or full-model LLM benchmark. All 40 small library qualification cases per run check complete cells/scalars against independent high-precision references before timing.

| N | Kalandra CPU | OpenBLAS 1 thread | OpenBLAS 12 threads | Accelerate 1 thread | Accelerate AUTO | Kalandra GPU resident |
|---:|---:|---:|---:|---:|---:|---:|
| 128 | 0.128 | 0.071 | 0.086 | 0.010 | 0.010 | 0.314 |
| 256 | 0.938 | 0.451 | 0.206 | 0.048 | 0.048 | 0.341 |
| 512 | 7.016 | 2.846 | 0.852 | 0.270 | 0.214 | 0.494 |
| 1024 | 55.666 | 23.320 | 6.410 | 1.940 | 1.300 | 1.836 |
| 2048 | 443.228 | 180.421 | 32.819 | 15.021 | 9.054 | 12.396 |

## Numerical and runtime qualification

All **44 selected CTest gates pass** on the final implementation. The expanded pipeline gate executes **237,877,413 output-cell references**, including optimized/debug/ASan+UBSan/TSan current and staged-legacy paths, plus optimized bundles with the new optional entries removed. Each current matrix mode checks 78 cases and 31,709,289 cells against independent LLVM APFloat trees. The fused/staged fixtures check 43 cases and 8,814,552 output cells per bundle/mode, with 410,751,531 raw-bit input-immutability checks across their final runs; register tests independently check both original inputs too.

Coverage includes dyadic and non-dyadic products, NaNs/signaling values, signed zero, subnormals, cancellation-sensitive sums, exact/below size thresholds, partial 16×16 tiles, a partial final 16-row-tile group, and all seven root levels at K=4096. Each matrix still submits exactly once and dispatches once. Allocation injection, held cancellation/deadline/device completion failure, cleanup, old-bundle fallback, canonical capture limits, larger direct/Tier 2 fallbacks, graphs, indirect bindings and all-ten-type ReLU pass. CPU sanitizers do not instrument GPU instructions; the hardware gates enable Metal API/shader validation.

The first compatibility attempt failed because the reconstructed old-bundle fixture omitted the separately linked row/quad entries. The fixture was corrected to retain those existing entries; the rerun passed. That failure log is retained and is not counted as a final passing gate. No numerical mismatch was found in the new path.

The new entry changes tile assignment only: each tile still has one producer, each cell retains the same separately rounded products and adjacent-pair additions, and the epilogue uses the same flat cell index. Cancellation behavior, task ownership, acknowledgement-held backing, budget enforcement and public runtime ABI are unchanged. Existing entries remain available when the optional function is absent. The [qualification record](memory-access-qualification-2026-10-26.json) records source/artifact hashes, logs, scope and the initial fixture failure.

## Evidence and reproduction

- [Initial access-order screen](memory-access-screening-2026-10-26.json).
- [Seven-block unconditional order confirmation](memory-access-order-confirmation-2026-10-26.json).
- [Rejected inline-policy boundaries](memory-access-policy-boundaries-2026-10-26.json).
- [Host-dispatch boundary confirmation](memory-access-dispatch-boundaries-2026-10-26.json).
- [Pack/matrix stage timestamps and unsampled controls](memory-access-stages-2026-10-26.json).
- [Address model and tile permutation proof](memory-access-model-2026-10-26.json).
- [Complete pipeline run 1](memory-access-pipeline-2026-10-26.json), [run 2](memory-access-pipeline-confirmation-2026-10-26.json).

Raw records retain all samples, source snapshots, generated MSL, commands and hashes. The offline audit recomputes the paired results and verifies snapshots/round coverage. Reproduce on a supported physical Mac with the pinned Metal component; outputs must be fresh paths:

```sh
python3 native/benchmarks/memory_access_audit.py
python3 native/benchmarks/memory_access.py --output /tmp/access-screen.json
python3 native/benchmarks/memory_access.py --confirm order_m16 --blocks 7 --output /tmp/access-confirm.json
python3 native/benchmarks/memory_access.py --confirm order_dispatch --suite boundaries --blocks 5 --output /tmp/access-boundaries.json
python3 native/benchmarks/memory_access_stages.py --screen /tmp/access-screen.json --output /tmp/access-stages.json
python3 native/benchmarks/memory_access_model.py --output /tmp/access-model.json
python3 native/benchmarks/pipeline_run.py --baseline-metal /path/to/frozen-qualified-metal.a --output /tmp/access-pipeline.json
```

Builds, correctness tests and profiling are kept separate from acceptance timings. Physical qualification here does not establish performance on other Apple GPUs, Linux or Windows. Hosted checks run separately. Remaining work includes small resident-pipeline latency and more precise per-kernel counter attribution; the current evidence supports this bounded traversal optimization without claiming the memory bottleneck is eliminated.
