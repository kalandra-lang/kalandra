# Strict unary GPU graph fusion — 26 October 2026

Pure float32 matrix graphs now fuse a single-consumer, same-cell unary map into the preceding matrix store. This includes ReLU and captured scalar arithmetic. Eight projection/ReLU layers drop from 18 to 10 compute dispatches, with five submissions retained. The arithmetic tree, four-logical-stage cancellation allowance, work cap, shape checks and ownership rules remain unchanged.

The longer full study reports median elapsed reductions of 3.2–9.0% in the eight larger eligible cases. The smallest 1×256 case regresses 2.1%. Earlier runs contain additional regressing medians and every study has losing blocks. This is not a universal or regression-free speedup. The 128×128 four-layer lifetime payload peak falls from 589,856 to 458,784 bytes, saving 128 KiB (22.2%). Initial uploads dominate peaks for some skinny cases, so those unchanged lifetime peaks do not imply unchanged hot storage.

## Contract and mechanism

Baseline is d81ad1292dec2b5470c0a8c8c66051243a42fa12. The frozen optimized Metal archive SHA-256 is `a70d965b3e4949e363b9739887b413b8beac09d6e3e766408792e2e79a19851f`, matching the preceding graph-batching qualification. Both versions use the same current compiler-generated bundle, driver and task runtime; the old runtime ignores graph fusion. Source snapshots, commands, artifact hashes and baseline hashes are in each raw record. GPU timing is on M2 Max/macOS 27.2 with the pinned LLVM toolchain.

The compiler emits optional fused entries for maps reading only the immediately preceding matrix. Runtime admission also requires single consumption, a supported hardware variant and sufficient workspace. Explicit unique operand lists and scalar captures are supported. Shared matrix values, unsupported/indirect maps, typed programs, interleaved reductions, empty outputs and old libraries without the optional entry keep their staged behavior. No vendor numerical library is used.

Fused maps keep the exact original float32 operation order after the separately rounded matrix contraction tree. Both logical stages still count against the four-stage and 64-Mi estimated-work batching allowances. Parameters and all input owners remain charged until GPU acknowledgement. No new wall-clock cancellation guarantee is claimed. The final graph sum still submits separately. Extra optional pipelines add cold preparation work, excluded from throughput timing.

The workload, independent strict scalar oracle and initial-upload exclusions are the same as the preceding graph-batching study: fresh managed tasks/captures/intermediates/dispatch/completion/cleanup; dense resident weights reused at every layer. This is not a full LLM workload or a cross-language comparison. All calls, including warmups, check scalar bits; independent APFloat per-cell qualification is separate.

## Complete resident timing

Runs 1 and 2 each use six mirrored ABBA/BAAB blocks, two warmups and three measured rounds per fresh process, with a 20 ms calibration target. Mixed medians and wide control variation motivated a preselected full repeat of all fourteen cases with ten blocks and a 100 ms target; no cases or samples were dropped. No builds, tests or profilers ran concurrently with acceptance timing. Desktop activity, CPU affinity and clocks were not isolated. Positive percentages mean reduced elapsed time. Percentages are medians of paired reductions; absolute times below are medians of block means.

| M×N | Layers | ReLU | Run 1 reduction | Run 2 reduction | Longer-run baseline → fused µs | Longer reduction (all-block range) |
|---|---:|:---:|---:|---:|---:|---:|
| 1×256 | 2 | yes | -4.43% | +1.03% | 418.04 → 423.95 | -2.06% (-6.22…+7.92) |
| 1×512 | 4 | yes | +11.00% | +11.71% | 544.61 → 533.59 | +3.23% (-3.67…+20.58) |
| 1×2048 | 1 | no | -3.88% | -1.78% | 375.98 → 372.81 | +3.20% (-24.47…+9.09) |
| 1×2048 | 2 | no | -0.29% | +0.27% | 426.18 → 425.40 | +0.64% (-4.13…+5.97) |
| 1×2048 | 2 | yes | +20.84% | +15.71% | 464.66 → 427.73 | +7.12% (-20.19…+23.72) |
| 1×2048 | 4 | no | -0.29% | -0.23% | 510.86 → 507.79 | -0.57% (-11.66…+19.43) |
| 1×2048 | 4 | yes | -10.04% | -3.00% | 710.33 → 701.31 | +3.89% (-30.88…+14.55) |
| 1×2048 | 8 | no | +0.18% | +5.27% | 842.30 → 837.22 | +0.38% (-5.79…+16.85) |
| 1×2048 | 8 | yes | +4.83% | +4.98% | 1290.37 → 1205.65 | +9.02% (-6.95…+17.12) |
| 1×4096 | 4 | yes | +5.33% | -0.02% | 1475.69 → 1424.61 | +4.50% (-2.04…+7.17) |
| 1×4096 | 8 | yes | +2.81% | +8.83% | 2849.95 → 2623.84 | +5.30% (-4.81…+19.53) |
| 2×2048 | 4 | yes | +6.41% | +6.66% | 860.93 → 810.83 | +6.01% (+0.97…+11.72) |
| 128×128 | 4 | yes | +9.28% | +7.30% | 795.61 → 745.62 | +7.93% (-2.94…+14.13) |
| 512×512 | 2 | no | -0.22% | -16.28% | 1008.72 → 1003.32 | +0.55% (-3.06…+7.40) |

The four-layer 1×2048 case loses 10.0% and 3.0% in the first runs, then gains 3.9% in the longer run; that later result does not erase the earlier regressions. The unchanged 512² no-ReLU control loses 16.3% in run 2 and gains 0.6% in the longer run. These controls expose timing variability but do not prove every loss is noise.

## Separate profiling

Four alternating blocks, three calls per measured round. Values are medians of process medians in microseconds. GPU command durations and CPU encoding intervals are diagnostics, not an additive decomposition of uninstrumented wall time.

| Case | Wall old → new µs | GPU old → new µs | Encoding old → new µs | Dispatches old → new |
|---|---:|---:|---:|---:|
| 1×256, 2 layers + ReLU | 494.29 → 459.54 | 60.15 → 62.84 | 38.49 → 31.54 | 6 → 4 |
| 1×2048, 4 layers + ReLU | 1070.40 → 790.53 | 441.88 → 312.26 | 59.25 → 49.67 | 10 → 6 |
| 1×4096, 8 layers + ReLU | 2671.77 → 2532.32 | 1615.55 → 1612.36 | 138.78 → 103.15 | 18 → 10 |
| 128×128, 4 layers + ReLU | 829.98 → 792.94 | 196.49 → 194.61 | 75.96 → 56.56 | 10 → 6 |

## Correctness and reproduction

The extended graph gate passes optimized/debug/ASan+UBSan/TSan with Metal API/shader validation: 350 APFloat cases and 6,790 core-loop output checks per mode, plus explicit shared-intermediate, scalar-capture, unique-operand-list and absent-optional-entry checks. It covers 70 allocation checkpoints per mode, held cancellation/deadline/device failure at all three boundaries, teardown, concurrent graphs, input immutability, 121 tight budgets, indirect captures, empty/rectangular shapes and the work cap. Source compilation/execution also passes in every mode. Host sanitizers do not instrument GPU instructions.

The final qualification adds the existing 44 graph/matrix/control fixtures, four all-ten-type numeric program/matrix-control CLI gates, and epilogue code-generation checks: 50 gates total. See the qualification record and retained logs for actual outcomes.

Use `native/benchmarks/graph_batching.py --baseline-metal <frozen-archive> --baseline-batched --build-dir <new-directory> --blocks 10 --target-ms 100 --output <unused.json>`. The earlier unbatched-baseline mode remains available. `--profile-only` separates profiling. `native/benchmarks/graph_batching_audit.py` recomputes checksums, counts, source digests and paired statistics.

- [First full run](graph-fusion-paired-2026-10-26.json)
- [Second full run](graph-fusion-confirmation-2026-10-26.json)
- [Longer full run](graph-fusion-precision-2026-10-26.json)
- [Profiles](graph-fusion-profile-2026-10-26.json)
- [Audit](graph-fusion-audit-2026-10-26.json)
- [Qualification](graph-fusion-qualification-2026-10-26.json)
