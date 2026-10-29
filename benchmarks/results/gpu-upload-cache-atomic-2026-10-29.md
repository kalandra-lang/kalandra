# Atomic budget accounting and bounded upload reuse — 29 October 2026

The [first cache candidate](gpu-upload-cache-2026-10-29.md) passed correctness but was not promoted: longer batches found a 7.92% resident 64×256 regression. It locked every payload reservation and release. This revision uses an atomic live-plus-idle reservation ledger; only cache mutation and eviction take the mutex. The original results, including the initial 45.63% input regression that did not reproduce in longer batches, remain retained.

## Contract and correctness

The cache still retains at most 16 MiB and 64 exact-size/device shared-upload buffers inside the existing 64 MiB default payload budget. Every logical byte and padding is overwritten before publication. Last-owner retirement requires no admitted work; pending/concurrent resources are discarded. Private outputs, scratch, staging and parameter arenas remain excluded. Runtime destruction closes/trims the cache, and zero capacity disables it. Numerical operations, checkpoints, failure-injection order and cancellation/fault precedence are unchanged.

The atomic ledger reserves total capacity before publishing live bytes, releases live ownership before total capacity, and leaves the total unchanged during live/idle transfers. Cache eviction releases actual backing before returning its reservation. Racing allocations cannot spend the same space. A new test makes eight callers compete for exactly two live allocations across 32 rounds, including eviction of an existing idle buffer. It passes in debug, optimized, ASan/UBSan and TSan builds.

200 runtime/support CTests passed after this revision, including all four upload suites, concurrency/failure/budget checks, encoding overlap, and module/fixed-shape/forwarding fixtures. Six long source-compilation integration fixtures and the long numeric CLI suites were not rerun after this private accounting change; their earlier passing results are retained in the first report. This is not a full compiler-suite claim. Fresh numerical qualification passes 432 changing-input full-output comparisons across cache limits, upload modes, runner policies and four shapes, plus eight independent high-precision oracle checks. Metal/shader validation is enabled for correctness and disabled for timing.

## Longer-batch production comparison

Frozen pre-cache production (`c542d414`) versus the actual revised runtime, using unchanged generated shaders and original timed driver loop. Apple M2 Max/macOS 27. Three adjacent ABBA/BAAB blocks per case; two warm-up and three measured rounds; adaptive 1,000 ms target, maximum 8,192 calls. The identical-build control covers the five previously unstable cases. All setup is excluded, all per-call work and all samples are retained. Positive percentages mean lower elapsed time. Absolute medians and median paired ratios summarize different distributions: percentages are calculated within adjacent pairs before aggregation. These small block counts and a single machine limit precision and generalization.

| Batch × width | Upload mode | Old → new µs | Paired reduction | Full block range | Identical-build control |
|---|---|---:|---:|---:|---:|
| 1 × 512 | resident | 459.1 → 453.9 | +1.17% | +1.14% … +8.78% | not measured |
| 1 × 512 | input | 450.6 → 436.2 | +3.75% | -10.12% … +8.54% | +3.15% |
| 1 × 512 | all weights | 1872.3 → 688.2 | +63.12% | +59.28% … +64.11% | not measured |
| 16 × 512 | resident | 610.9 → 660.6 | -8.29% | -10.66% … +4.25% | +0.25% |
| 16 × 512 | input | 708.3 → 675.5 | +5.88% | +4.63% … +6.98% | not measured |
| 16 × 512 | all weights | 2065.5 → 823.2 | +59.24% | +56.30% … +61.60% | not measured |
| 64 × 256 | resident | 711.4 → 714.3 | -0.12% | -5.88% … +4.52% | +7.88% |
| 64 × 256 | input | 725.3 → 724.6 | +0.10% | -6.97% … +8.01% | -0.96% |
| 64 × 256 | all weights | 1048.8 → 776.0 | +22.91% | +21.46% … +27.21% | not measured |
| 64 × 512 | resident | 847.7 → 841.7 | +3.47% | +0.42% … +7.42% | not measured |
| 64 × 512 | input | 969.0 → 867.4 | +7.34% | +1.23% … +17.55% | +0.84% |
| 64 × 512 | all weights | 2310.6 → 1102.3 | +52.52% | +48.02% … +52.62% | not measured |

## Focused resident confirmation

Resident 16×512 was the only median slowdown larger than 5% in the twelve-case atomic run. It therefore received a separately declared seven-block identical-build control and seven-block production repeat, with the same one-second target and frozen binaries. The original -8.29% result above is retained.

| Study | Baseline → current µs | Paired reduction | Full block range |
|---|---:|---:|---:|
| aa | 635.6 → 635.7 | -0.81% | -7.51% … +11.82% |
| repeat | 615.4 → 645.1 | -0.97% | -17.71% … +9.06% |

The original -8.29% median slowdown does not reproduce in the focused repeat (-0.97%); the identical-build control is -0.81%. This supports accepting the bounded upload optimization, but does not prove a zero resident cost or attribute every earlier swing to the removed mutex. Individual blocks remain volatile and all are retained. The accepted benefit is the repeatable 52.52–63.12% reduction in width-512 all-weight-upload time, and 22.91% at width256. No resident speedup is claimed.

Independent cohorts must not be subtracted as a causal estimate. Resident cases have no upload-cache hits. The earlier same-executable off/on study establishes the large width-512 upload-reuse benefit, while this follow-up measures the complete revised runtime against pre-cache production. It does not establish a new GPU arithmetic speedup or close the remaining resident-compute/vendor-library gap.

## Memory and external load

Maximum observed idle cache: 12,713,984 bytes; retained payload: 27,447,296 bytes. Every recorded sample respects the existing 67,108,864-byte budget and the 16 MiB cache limit. These are observed peaks for this workload set. Idle retention is an explicit tradeoff, visible in the new retained/cache statistics; live-owner counters keep their old meaning. `trimUploadCache()` releases idle storage.

No local builds, tests or profiling overlap timing. External CPU sampling does not establish GPU idleness. 100% means one CPU core.

| Study | External CPU median | Maximum |
|---|---:|---:|
| atomic-aa | 45.8% | 247.1% |
| atomic-repeat | 38.8% | 295.4% |
| resident-confirm-aa | 45.4% | 179.3% |
| resident-confirm-repeat | 39.2% | 356.6% |

## Reproduction

`upload_cache_study.py` prepares the frozen candidate and performs numerical qualification. `upload_cache_atomic.py --phase measure --study aa|repeat --build DIR --output NEW.json` applies the declared longer-batch protocol without changing the executables or hot loop. `upload_cache_resident_confirm.py --phase measure --study aa|repeat --build DIR --output NEW.json` applies the focused seven-block resident protocol. `fair/upload_cache_atomic_report.py` audits source/artifact identities, both numerical qualifications, resource caps, retained test logs and every paired observation, then regenerates both reports. Large binaries and source snapshots remain local under ignored `benchmarks/.build`; JSON records and manifests retain their hashes. Production uses no Accelerate/MPSGraph numerical implementation.
