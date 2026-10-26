# Short resident GPU graph batching — 2026-10-26

Consecutive pure float32 matrix/map stages can share a Metal command. On the Apple M2 Max, two independent six-block comparisons reduce complete resident-chain median elapsed time by **29–61%** across the eligible cases. Every eligible paired block improves in both runs. This compares the prior Kalandra runtime with the changed runtime; it does not measure Rust/C++ parity, vendor-library parity or full-model LLM/token throughput.

## What changed

`native/metal/Metal.mm` keeps the existing kernel dispatches and arithmetic, but permits up to four rank-two product/map stages per command when the matrix bundle provides the hardware tiled entry. A second cap limits estimated work to 64 Mi matrix products or map output elements per batch. Neither cap limits language graph size or guarantees wall-clock latency. Larger stages, software-only matrix bundles, checked typed programs, interleaved reductions and host decisions retain their existing acknowledgement boundaries. A final graph reduction remains a separate submission.

Encoded intermediate tensors remain private and retained until real acknowledgement. Parameter and indirect resource buffers remain reserved too. Operand use counts are released in source order after completion. Before adding a stage, the runtime checks room for its output and parameters; if the earlier outputs need reclaiming, it submits the current command first. Concurrent budget claims still use the ordinary allocator. No shader, public runtime API, arithmetic tree or task scheduler change is included.

**Tradeoff:** cancellation is checked between encoded stages and at the external-lease submission decision; an uncommitted cancelled batch can be discarded. Once committed, settlement waits for the entire batch, potentially four short stages rather than one. The GPU is not preempted. Batching also retains more live intermediate storage. Both effects are deliberate and tested; cancellation latency and peak memory are not claimed unchanged.

## Measurement contract

- Baseline: `d1d1a2336dda97c057029c411ac422c130968df0`. The frozen archive SHA-256 matches the prior skinny-projection qualification: `e9b7ade63195a8f8e420ac77d984682d72defc3a66c877ff7373ef5132ec4576`.
- Same current compiler, shaders, C++ benchmark driver and task archive on both sides; only the Metal runtime archive differs. Source snapshots, hashes, commands and artifact hashes are in the records.
- Input is M×N, weights N×N. Each layer multiplies by the **same resident weight matrix**, optionally followed by ReLU; the final tensor is reduced with the strict adjacent sum. These synthetic recurrent/projection chains isolate repeated stage handoffs. Different weights per layer, upload-inclusive execution and real-model throughput were not measured here.
- Every GPU matrix computes all output cells. The untimed CPU reference exploits the known 13-column period of the fixture only to reduce reference-generation work; it evaluates each distinct cell using separately rounded float32 products and the full adjacent sum. Each timed call checks the scalar bits before cleanup. Independent APFloat per-cell tests cover non-dyadic/special values separately.
- Timers include fresh tasks, graph capture, allocations, all submissions, scalar observation and cleanup. Initial uploads, pipeline loading and reference generation are excluded. No persistent result cache or reused intermediate workspace.
- Six mirrored ABBA/BAAB blocks, two fresh processes per version per block, two warmup batches and three measured batches per process. Iteration counts are equal within each pair and calibrated to 20 ms. All samples, calibration and losing blocks are retained.
- Performance and profiling runs are separate. No competing agent builds/tests/profilers ran during timing. Desktop activity, CPU affinity, clocks and temperature were not isolated. Physical qualification: M2 Max, macOS 27.2; deployment minimum remains 26. Other GPUs/OS versions are unqualified by these measurements.
- Benchmark budget is max(64 MiB, 12×N² + 32×M×N bytes), including host captures and uploads. Larger fixtures raise this harness budget; the production default remains 64 MiB.

## Complete resident results

Absolute times are medians of paired block means. Percentages are medians of each block’s paired time reduction, so they need not equal the ratio of the displayed absolute medians. Positive percentages mean less elapsed time.

| M×N | Layers | ReLU | Submissions old → new | Run 1 µs old → new | Run 1 reduction | Confirmation reduction |
|---|---:|:---:|---:|---:|---:|---:|
| 1×256 | 2 | yes | 5 → 2 | 942.30 → 452.29 | +51.08% | +53.55% |
| 1×512 | 4 | yes | 9 → 3 | 1446.12 → 580.45 | +60.33% | +61.29% |
| 1×2048 | 2 | yes | 5 → 2 | 913.35 → 450.43 | +50.99% | +50.55% |
| 1×2048 | 4 | yes | 9 → 3 | 1815.23 → 755.45 | +57.53% | +56.70% |
| 1×2048 | 8 | yes | 17 → 5 | 3348.53 → 1314.46 | +61.02% | +60.65% |
| 2×2048 | 4 | yes | 9 → 3 | 1959.92 → 932.59 | +53.18% | +51.24% |
| 1×4096 | 4 | yes | 9 → 3 | 2616.55 → 1508.77 | +42.51% | +42.16% |
| 1×4096 | 8 | yes | 17 → 5 | 5259.74 → 2902.45 | +43.75% | +43.84% |
| 1×2048 | 2 | no | 3 → 2 | 630.52 → 442.20 | +29.18% | +28.82% |
| 1×2048 | 4 | no | 5 → 2 | 1073.43 → 551.23 | +46.79% | +50.98% |
| 1×2048 | 8 | no | 9 → 3 | 1897.04 → 894.79 | +52.08% | +54.90% |
| 128×128 | 4 | yes | 9 → 3 | 1843.62 → 823.91 | +55.13% | +53.70% |
| 512×512 | 2 | no | 3 → 3 | 1062.02 → 1062.35 | +0.85% | +2.60% |
| 1×2048 | 1 | no | 2 → 2 | 408.05 → 419.70 | -2.16% | +0.71% |

**Controls remain disclosed.** The one-layer 1×2048 case performs two submissions in both versions: −2.16% in run 1 (blocks −28.82%…+4.06%) and +0.71% in confirmation (−10.24%…+4.33%). The two-layer 512×512 case exceeds the work allowance and keeps three submissions: +0.85% and +2.60%, with a −2.58% losing block in run 1. These observations do not establish that every small loss is noise, nor a universal regression-free result.

## Separate profile

Four alternating blocks per case/version, three iterations and three measured rounds per process. Values below are medians of process medians in microseconds. Command GPU durations and CPU encoding intervals are diagnostics; the remaining wall time includes queue/driver/completion/scheduler effects and is not attributed wholly to any one component.

| Case | Wall old → new | GPU duration old → new | Encoding old → new | Compute dispatches old → new |
|---|---:|---:|---:|---:|
| 1×2048, 4 layers + ReLU | 1790.76 → 895.21 | 208.43 → 298.87 | 99.61 → 65.70 | 10 → 10 |
| 1×2048, 8 layers + ReLU | 3444.01 → 1470.00 | 424.10 → 387.46 | 181.72 → 114.99 | 18 → 18 |
| 1×4096, 8 layers + ReLU | 4848.62 → 2881.53 | 1723.59 → 1710.94 | 217.48 → 161.18 | 18 → 18 |
| 128×128, 4 layers + ReLU | 2372.78 → 1087.45 | 470.75 → 388.48 | 110.42 → 74.71 | 10 → 10 |
| 512×512, 2 layers | 1261.36 → 1355.49 | 553.51 → 674.05 | 42.72 → 44.41 | 5 → 5 |
| 1×2048, 1 layers | 468.09 → 474.28 | 103.59 → 104.04 | 23.52 → 22.60 | 3 → 3 |

The arithmetic dispatch count is identical for every profiled case. For example, eight 1×4096 projections with ReLU keep eighteen compute dispatches, while submissions drop 17→5; profiled GPU duration stays near 1.72 ms and wall time drops from about 4.85 ms to 2.88 ms. Other GPU-duration observations fluctuate, including controls; the profile is not used as acceptance timing.

## Memory and correctness

The 128×128 four-layer/ReLU lifetime payload peak rises from 393,228 to 589,856 bytes (about 192 KiB). The 1×2048 four-layer/ReLU peak is 50,348,032 bytes in both versions because initial weight uploads dominate that lifetime peak; this does not imply identical hot graph storage. The 512×512 ineligible control stays at 6,291,468 bytes. These counters cover managed tensor/staging payloads, not RSS, driver allocations or all host metadata. The frame adds fixed bookkeeping for retained parameters/resources.

49 focused CTest gates pass:

- The new hardware batching gate runs optimized, debug, ASan/UBSan and TSan with Metal API and shader validation. Its core loop checks 350 APFloat cases and 6,790 output references per mode, over 1–9 layers, with and without ReLU, empty inputs, non-dyadic data, subnormals, signed zero, infinities and NaNs. Additional rectangular, shared-intermediate and work-cap tests check complete tensors.
- 95 direct-graph allocation checkpoints are injected per mode, plus a full indirect-graph allocation sweep. Held completion tests cover cancellation, deadlines and device failure at each of three batch boundaries; retained staging counts and no later submissions are checked. A separate 31-input argument-buffer graph covers retained indirect resources and cancellation.
- Pre-admission cancellation, sixteen independent graphs on four workers, immutable inputs, core destruction with a pending batch, empty intermediates, and complete cleanup pass. Every byte budget from 60 through 180 succeeds using suitable batch sizes. Two 2×4096 by 4096×4096 products exactly fill the work allowance; a four-product graph must submit two commands and all 8,192 output cells equal the independent power-of-two result.
- Compiled Kalandra source produces the expected nonzero result in all four runtime modes.
- 44 existing graph, matrix/reduction, feedback and scalar/tensor control gates pass in four modes. Typed program and matrix-control suites pass in optimized and ASan/UBSan modes: 67+20 source cases per mode across all ten numeric types, including checked faults and selected-path shape obligations.

Host sanitizers do not instrument GPU instructions. Physical GPU validation and numerical references are separate checks. The tests do not promise a wall-clock cancellation bound.

One development fixture failed before runtime testing: its generator assumed thirty additions would become a single 31-input graph map, but the compiler emitted thirty binary maps. The fixture was corrected to compile a standalone 31-input map and bind it into a runtime graph. The failed log is retained; production code did not change in response.

## Evidence and reproduction

- [First paired run](graph-batching-paired-2026-10-26.json) and [confirmation](graph-batching-confirmation-2026-10-26.json).
- [Separate profiles](graph-batching-profile-2026-10-26.json), [recomputed audit](graph-batching-audit-2026-10-26.json) and [qualification hashes](graph-batching-qualification-2026-10-26.json).
- [Final hardware gate](graph-batching-final-tests-2026-10-26.log), [48 regression gates](graph-batching-regression-2026-10-26.log), and [retained fixture failure](graph-batching-expanded-tests-2026-10-26.log).

The audit checks 672 measured processes, 112 profiled processes, 56 calibration processes and 168 paired blocks, including source-snapshot hashes, per-call checksums, matching iteration counts, ordering, submission/completion counts and unchanged compute-dispatch counts in the profile.

```sh
python3 native/benchmarks/graph_batching.py \
  --baseline-metal /path/to/d1d1a233/libkalandra-metal-optimized.a \
  --blocks 6 --output benchmarks/results/graph-reproduction.json
python3 native/benchmarks/graph_batching_audit.py \
  benchmarks/results/graph-reproduction.json
ctest --test-dir build/native -R "^native-metal-graph-batching$" --output-on-failure
```

Use `--reuse-build` for a second comparison against the verified saved artifacts, and `--profile-only --blocks 4` for separate diagnostics. Choose fresh output names.
