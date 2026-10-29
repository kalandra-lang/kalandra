# GPU host costs, SIMD sharing and backing-reuse probe — 29 October 2026

Production remains at `efbe33f`. This study rejects the operand-sharing kernels and tests an explicitly different, diagnostic-only buffer-retention policy. No language, cancellation, arithmetic, ownership or production memory-budget contract changes are promoted.

## Whole-production host attribution

Apple M2 Max; copied current runtime, actual generated transformer bundle. Three fresh processes per shape/upload mode, two warm-ups plus three rounds of 100 calls. Each row is the median of process medians, in microseconds per call. These are instrumented diagnostics, not paired acceptance. Twelve full-output comparisons against the untouched production driver passed.

| Batch × width | Upload mode | Wall | GPU | Allocation | Destruction | Submit → GPU start | GPU end → wake | Encoding |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 1 × 512 | resident | 398.3 | 153.0 | 26.5 | 13.3 | 99.3 | 90.5 | 38.2 |
| 1 × 512 | input | 402.0 | 153.8 | 26.9 | 13.3 | 101.7 | 81.6 | 35.6 |
| 1 × 512 | all weights | 1756.2 | 180.3 | 66.6 | 23.8 | 479.8 | 88.6 | 39.7 |
| 16 × 512 | resident | 552.7 | 255.5 | 47.9 | 18.5 | 128.8 | 82.4 | 59.8 |
| 16 × 512 | input | 568.4 | 258.7 | 53.7 | 21.5 | 125.4 | 83.1 | 56.3 |
| 16 × 512 | all weights | 1928.0 | 300.8 | 88.6 | 30.1 | 486.8 | 89.7 | 62.4 |
| 64 × 256 | resident | 616.9 | 298.3 | 49.6 | 20.4 | 145.7 | 86.7 | 62.2 |
| 64 × 256 | input | 636.0 | 297.2 | 55.1 | 21.6 | 146.7 | 84.0 | 59.3 |
| 64 × 256 | all weights | 942.6 | 300.1 | 85.6 | 30.8 | 179.2 | 88.7 | 60.4 |
| 64 × 512 | resident | 783.0 | 412.3 | 61.2 | 22.5 | 176.9 | 92.6 | 75.0 |
| 64 × 512 | input | 789.2 | 419.4 | 57.1 | 22.2 | 172.6 | 85.6 | 60.2 |
| 64 × 512 | all weights | 2098.9 | 456.3 | 93.6 | 31.6 | 489.9 | 91.4 | 66.3 |

Allocation/destruction, encoding, progression and submit scopes overlap; **do not add them**. Submit/start/end/wake use the common Mach clock domain; the maximum arithmetic partition residual is 101 ns over a complete 100-call round. Submit-to-start includes driver/scheduling/residency effects and is not proof of any single cause. Profile overhead and prevailing system load affect absolute values. The host profile preceded the lane-sharing captures.

## Operand-sharing controls

All versions preserve separately rounded products and the adjacent reduction tree. Complete tiles use explicit SIMD-group/lane attributes and require execution width 32; ragged cases retain the original 1D path. `mapping` changes only lane mapping. `a`/`b` share one operand, `both` shares both with indexed shuffles, and `fixed` uses constant broadcasts/XOR shuffles to test indexed-shuffle compiler expansion. Each version passes 84 numerical executions (420 total): every GPU output bit, input and guard is compared; independent CPU trees cover all cells up to 32,768 outputs and dispersed cells/corners above that. IEEE patterns check compatibility with the original kernel, not a new arithmetic contract.

Apple documents physical thread-to-SIMD mapping as implementation-defined; the explicit mapping avoids relying on an assumed linear lane assignment. Sources: [threadgroups](https://developer.apple.com/documentation/metal/creating-threads-and-threadgroups), [Metal specification §6.10.2](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf).

Xcode 27.1 compiler fields and exported counters, 64×2048×512. Resource values are shader/compiler evidence; replay durations are not acceptance timings. The baseline 94-register value comes from the preceding aligned-grid capture.

| Mode | Registers / highest used | Spilled bytes | Measured occupancy | Dynamic ALU instructions | Shared-read limiter |
|---|---:|---:|---:|---:|---:|
| mapping | 92 / 92 | 0 | 22.96% | 261,488,640 | 41.03% |
| a | 254 / 254 | 848 | 9.51% | 1,537,566,720 | 0.76% |
| b | 254 / 254 | 848 | 9.64% | 1,542,023,168 | 0.73% |
| both | 174 / 174 | 32 | 13.68% | 2,911,266,816 | 3.07% |
| fixed | 126 / 126 | 32 | 13.04% | 1,586,920,960 | 6.09% |

Reducing the shared-read limiter merely moved the bottleneck: all sharing versions spill, with much more dynamic ALU work. Even the constant-lane control still spills. Mapping alone remains spill-free but is not a demonstrated improvement.

### Retained initial timing series — potentially contaminated

The first series was initially believed to have stopped all replays. Closing the task capture windows later exposed a retained A-sharing Xcode GPU debug session. That session was explicitly stopped, then every task capture window was closed. All original files and load records remain retained, but **none of their timing ratios establish acceptance**. The per-file load note saying all replays were stopped was the belief at collection and is superseded here. These data must not be silently relabeled clean.

| Mode | Blocks | Median paired time reduction across the six shapes | External CPU median / max |
|---|---:|---:|---:|
| mapping | 7 | -15.08% to -4.38% | 176.8% / 247.0% |
| a | 3 | -6772.21% to -1754.45% | 153.2% / 302.5% |
| b | 3 | -7051.97% to -1726.97% | 143.8% / 293.1% |
| both | 3 | -1287.51% to -486.68% | 151.1% / 275.9% |
| fixed | 7 | -553.86% to -313.32% | 243.8% / 385.0% |

### Repeated screens after closing capture windows

No builds/tests/profiling overlap these runs. ABBA/BAAB fresh processes, two warm-up and three measured rounds, adaptive 30 ms target, maximum 256 calls. Negative reductions mean slower. Single-block repeats of severely losing variants are rejection checks, not precise acceptance estimates. CPU sampling does not prove GPU idleness.

| M × K × N | Mapping | A | B | Both | Fixed |
|---|---:|---:|---:|---:|---:|
| 16 × 512 × 512 | -5.22% | -1862.21% | -1794.21% | -539.56% | -304.56% |
| 16 × 512 × 2048 | -8.11% | -5452.84% | -5644.69% | -1070.57% | -481.24% |
| 16 × 2048 × 512 | -8.01% | -2208.74% | -2240.93% | -618.91% | -353.93% |
| 64 × 512 × 512 | -5.95% | -5595.83% | -5761.24% | -1099.80% | -498.73% |
| 64 × 512 × 2048 | -7.94% | -6333.42% | -6602.84% | -1208.83% | -420.01% |
| 64 × 2048 × 512 | -7.23% | -6412.10% | -6740.96% | -1233.39% | -550.34% |

## Bounded backing-reuse diagnostic

The same copied runtime binary runs with the cache disabled/enabled. The process-local cache is capped at 64 entries and 64 MiB, matches device/length/storage mode, and quarantines retired buffers until a driver boundary after a completed invocation proves no commands remain in flight. It never reuses a retired buffer inside the same invocation. The actual production shaders/task core remain unchanged.

**This is not production-equivalent memory semantics.** Cached bytes are outside the current runtime live-byte ledger and the probe assumes one caller. Numerical correctness does not qualify cancellation, failure injection, concurrent invocations, budget eviction, or destruction of a production pool. Those require a deliberate runtime design. There is no production speedup claim here.

Changing inputs alternate patterns 0/1 over four calls, across four shapes, all three upload modes and all three runner policies: 144 decoded-float32 full-output comparisons with cache off/on (288 executions), plus eight untouched-production outputs and independent high-precision forward-error checks. Warm cache hits and the 64 MiB cap are asserted. An initial measurement harness incorrectly demanded hits in the first cold one-call warm-up; it stopped before collecting timings. The assertion was corrected to measured rounds, a new build was frozen, and all qualification cases rerun.

| Batch × width | Upload mode | Disabled µs | Enabled µs | Paired time reduction |
|---|---|---:|---:|---:|
| 1 × 512 | resident | 503.0 | 499.6 | +4.86% |
| 1 × 512 | input | 524.6 | 422.6 | +8.68% |
| 1 × 512 | all weights | 2017.8 | 835.4 | +56.72% |
| 16 × 512 | resident | 637.5 | 632.9 | +14.51% |
| 16 × 512 | input | 659.6 | 617.9 | +15.71% |
| 16 × 512 | all weights | 2214.5 | 1051.8 | +52.89% |
| 64 × 256 | resident | 846.3 | 664.8 | +20.59% |
| 64 × 256 | input | 725.0 | 683.0 | +11.58% |
| 64 × 256 | all weights | 1144.7 | 854.3 | +22.70% |
| 64 × 512 | resident | 1054.8 | 756.2 | +27.46% |
| 64 × 512 | input | 1165.9 | 816.6 | +29.55% |
| 64 × 512 | all weights | 2407.5 | 1056.3 | +58.43% |

### Untouched-production executable controls

Three independent paired blocks per study compare the untouched production executable to the diagnostic with retention disabled (`off`), then enabled (`on`). The off control exposes instrumentation/driver changes; the on control measures the complete experimental policy against actual production. These remain screens under a different memory contract, not production acceptance. Both use the same numerical qualification and frozen bundle.

| Batch × width | Upload mode | Off vs production | On vs production |
|---|---|---:|---:|
| 1 × 512 | resident | +23.42% | +0.89% |
| 1 × 512 | input | -7.63% | -3.25% |
| 1 × 512 | all weights | +0.56% | +67.38% |
| 16 × 512 | resident | -0.60% | +15.48% |
| 16 × 512 | input | +2.31% | +4.85% |
| 16 × 512 | all weights | +2.22% | +54.74% |
| 64 × 256 | resident | +0.73% | +17.93% |
| 64 × 256 | input | +10.73% | +36.21% |
| 64 × 256 | all weights | -9.41% | +48.07% |
| 64 × 512 | resident | +26.43% | +26.59% |
| 64 × 512 | input | +9.71% | +39.73% |
| 64 × 512 | all weights | +0.32% | +64.45% |

The strongest repeatable lead is all-weight upload at width 512: the independent enabled reductions are 54.74%/64.45% for batches 16/64, against disabled controls of 2.22%/0.32%. **Do not attribute the resident batch 64 result to caching:** disabled/enabled controls both improve by about 26%. Batch 16 resident is promising at 15.48%, but further controlled qualification is required. Batch 1 input regresses 3.25%; small resident/input gains are not established. These controls are separate cohorts, so their percentages must not be subtracted as a causal estimate.

External CPU samples (100% means one CPU core; this is not a GPU-idle measurement):

| Study | Median CPU | Maximum CPU |
|---|---:|---:|
| gpu-backing-reuse-paired | 70.9% | 435.7% |
| gpu-backing-reference-off | 73.6% | 243.3% |
| gpu-backing-reference-on | 72.8% | 166.1% |

The main disabled/enabled screen uses seven paired blocks; adaptive 100 ms target, maximum 512 calls. Median paired ratios need not equal the ratio of separately pooled medians. Peak retained cache over these timings: 14,733,312 bytes. Setup is excluded and each fresh process warms its own cache. The untouched-production controls above provide an independent reference, but no identical-build control or production-compatible implementation has established an acceptance threshold for this policy.

## Next architectural decision

Prioritize bounded completion-safe backing reuse over more shuffle micro-optimizations. A production design must charge live plus idle cached storage against the resource limit, evict idle entries before reporting budget exhaustion, keep pending GPU owners quarantined, handle concurrent invocation lifetimes, and define observable cache statistics/teardown and buffer-initialization guarantees. Preserve logical allocation-failure injection and cancellation/fault precedence. Start with deterministic ownership/budget tests and changing-input checks, then frozen paired/control/replication whole-model measurements. Do not adopt the diagnostic global cache as the implementation.

## Reproduction and evidence

`aligned_host_profile.py --build DIR --output FILE` freezes an instrumented copy from the current aligned production artifacts. `lane_share_screen.py --mode mapping|a|b|both --phase prepare|capture|measure --build DIR --output FILE` and `fixed_lane_screen.py --mode fixed` freeze shader controls; preparation output must be `DIR/qualification.json`. `backing_reuse_probe.py --phase prepare|measure --build DIR --output FILE` similarly requires a frozen qualification. Use fresh output/build paths, preserve manifests, and close every task capture window before timing. The existing aligned production frozen bundle is a prerequisite.

Raw `gpu-aligned-host-profile`, `gpu-lane-*`, `gpu-backing-reference-*` and `gpu-backing-reuse-*` JSON/CSV files retain samples, qualifications, command/source/artifact hashes and external-load observations. Large binaries/traces stay ignored under `benchmarks/.build`. Run `python3 native/benchmarks/fair/host_lane_report.py` to validate the local frozen artifacts and regenerate this report. No production tests were rerun because this pass changes diagnostic tooling/evidence only.
