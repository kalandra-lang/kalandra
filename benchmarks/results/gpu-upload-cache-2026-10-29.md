# First bounded shared-upload cache candidate — 29 October 2026

**Not promoted:** longer-batch confirmation found a 7.92% resident 64×256 regression. The atomic-ledger follow-up is documented separately. This first candidate is compared against the frozen pre-cache production executable (`c542d414`). Apple M2 Max, macOS 27. The generated transformer shaders, task core and arithmetic are unchanged. This replaces the earlier diagnostic allocator with a per-runtime cache whose idle storage is charged against the existing payload budget.

## Qualification and resource contract

206 runtime/support CTests passed across debug, optimized, address/undefined-sanitized and thread-sanitized builds. Two complete all-type matrix CLI suites also passed under ThreadSanitizer. The original 339-test selection was deliberately interrupted after those two CLI suites: the remaining repeated frontend CLI compilation was excluded because compiler/shader generation is unchanged. This is not a claim that the full compiler suite ran. The retained logs and selection record distinguish completed from interrupted work.

New tests cover changing data, immutable aliases, exact budgets and idle eviction, byte/entry limits, warm allocation-failure injection, partial-copy cancellation/deadlines, held GPU completion, concurrent callers and trimming, plus disabled/private-storage controls. Separately, 432 complete decoded-float32 output comparisons cover four shapes, three upload modes, three runner policies, three cache limits and four alternating-input calls. Eight untouched-production outputs pass the independent high-precision forward-error oracle. Metal and shader validation are enabled for these numerical checks and disabled for timings.

The default cache is limited to 16 MiB and 64 exact-size/device entries, inside `maxBytes` (64 MiB by default). Only CPU-populated shared upload backing is eligible. Every logical byte and sub-word padding is rewritten before publication. Private outputs, scratch, staging and parameter arenas are excluded. Last-owner retirement requires zero admitted operations; concurrent/potentially pending resources are discarded. Ordinary owners pin resources through GPU acknowledgement. Idle entries are evicted to satisfy new requests; runtime destruction closes and trims the cache. No new cancellation checkpoint, numerical relaxation or pending-buffer reuse is introduced.

Existing live-owner statistics retain their meaning; new retained/cache statistics expose idle memory. This deliberately trades bounded idle retention for allocation reuse. `maxCachedUploadBytes=0` disables it; `trimUploadCache()` releases idle backing. Concurrent statistic reads are not a transaction. Driver bookkeeping and control-object allocations remain outside the payload-byte budget, as before.

## Frozen paired measurements

Seven ABBA/BAAB blocks for the original and independent repeat, five for identical-build and cache-off/on controls, and three comparing the old production executable with the new runtime with caching disabled. Two warm-up and three measured rounds per fresh process; adaptive 150 ms target, at most 512 calls. Setup is excluded; upload modes include per-invocation copies. All samples and adverse results remain retained. Positive percentages mean less elapsed time. Median paired percentages need not equal the ratio of independently pooled timings.

| Batch × width | Upload mode | Repeat old → new µs | First reduction | Repeat reduction | Identical-build control | Same-build off → on | Old → new disabled |
|---|---|---:|---:|---:|---:|---:|---:|
| 1 × 512 | resident | 454.2 → 421.8 | +1.89% | +11.28% | -3.55% | +13.45% | -28.20% |
| 1 × 512 | input | 455.2 → 491.6 | +0.70% | -10.74% | +3.26% | -1.70% | -1.12% |
| 1 × 512 | all weights | 1888.0 → 654.3 | +64.77% | +64.62% | -5.88% | +66.08% | -2.21% |
| 16 × 512 | resident | 592.5 → 635.0 | +1.25% | -5.36% | -31.97% | +25.57% | -32.55% |
| 16 × 512 | input | 618.5 → 617.8 | +3.42% | -0.78% | -3.83% | -7.57% | +0.74% |
| 16 × 512 | all weights | 2142.6 → 818.5 | +60.67% | +61.80% | +2.50% | +48.26% | +6.71% |
| 64 × 256 | resident | 739.3 → 692.7 | -13.90% | +4.88% | -5.87% | -0.89% | +1.04% |
| 64 × 256 | input | 681.9 → 670.4 | -9.16% | +0.41% | +14.33% | -29.99% | +2.49% |
| 64 × 256 | all weights | 993.9 → 730.5 | +26.71% | +27.22% | +20.73% | +27.95% | +9.94% |
| 64 × 512 | resident | 828.9 → 831.7 | -5.25% | -0.34% | +4.42% | +0.89% | +10.58% |
| 64 × 512 | input | 827.9 → 1205.7 | +0.41% | -45.63% | -0.02% | -0.13% | +31.10% |
| 64 × 512 | all weights | 2370.8 → 1053.6 | +52.02% | +55.83% | -2.36% | +51.95% | -14.55% |

The resident cases cannot benefit directly from upload-cache hits. Their results measure the full runtime change, including ledger synchronization and timing variation. The same-build off/on comparison separates cache reuse from the rest of that change; the identical-build control estimates observed measurement variation. Do not subtract percentages from independent cohorts as a causal estimate.

### Between-block variation

Full minimum–maximum paired reductions, without removing outliers:

| Batch × width | Mode | First range | Repeat range | Off/on range |
|---|---|---:|---:|---:|
| 1 × 512 | resident | -33.14% … +24.20% | -8.43% … +28.62% | -27.05% … +26.08% |
| 1 × 512 | input | -35.01% … +20.10% | -25.95% … +22.11% | -11.21% … +8.74% |
| 1 × 512 | all weights | +57.12% … +66.05% | +57.64% … +66.74% | +63.01% … +66.43% |
| 16 × 512 | resident | -2.08% … +29.21% | -35.74% … +4.06% | +1.59% … +27.37% |
| 16 × 512 | input | -37.11% … +23.25% | -34.90% … +27.45% | -36.14% … +30.24% |
| 16 × 512 | all weights | +49.30% … +63.51% | +47.84% … +63.37% | +37.29% … +63.58% |
| 64 × 256 | resident | -42.91% … +27.88% | -37.70% … +25.69% | -4.78% … +24.64% |
| 64 × 256 | input | -56.78% … +21.91% | -37.76% … +28.39% | -39.56% … +29.01% |
| 64 × 256 | all weights | +2.21% … +43.61% | +0.56% … +42.27% | -11.03% … +43.84% |
| 64 × 512 | resident | -50.51% … +23.98% | -50.63% … +29.46% | -53.12% … +29.57% |
| 64 × 512 | input | -50.24% … +35.69% | -48.13% … +19.03% | -45.93% … +23.08% |
| 64 × 512 | all weights | +33.71% … +56.24% | +42.69% … +57.06% | +24.73% … +55.54% |

## Longer-batch confirmation

The original cohorts remain above, including the 45.63% batch64/input slowdown and the very large identical-build swings. These prompted a separate, predeclared five-case follow-up: three paired blocks, 1,000 ms adaptive target, maximum 8,192 calls, unchanged frozen binaries and hot loop. Identical-build control runs before the production comparison. No original samples are removed.

| Batch × width | Mode | Old → new µs | Paired reduction | Full block range | Identical-build control |
|---|---|---:|---:|---:|---:|
| 1 × 512 | input | 485.9 → 476.5 | +2.68% | -10.94% … +11.47% | +2.96% |
| 16 × 512 | resident | 654.6 → 635.8 | +3.79% | -4.84% … +4.51% | +0.72% |
| 64 × 256 | resident | 660.2 → 712.5 | -7.92% | -20.14% … -1.29% | -0.21% |
| 64 × 256 | input | 706.9 → 682.8 | +7.59% | +1.27% … +7.69% | +1.65% |
| 64 × 512 | input | 905.6 → 898.7 | -0.77% | -1.40% … +0.76% | -0.23% |

Maximum observed idle cache: 12,713,984 bytes. Maximum observed retained payload: 27,447,296 bytes, below the 67,108,864-byte budget. These are observed peaks for this workload set, not a new budget guarantee beyond the enforced cap.

## Load and scope

No builds, tests or profilers overlap acceptance. CPU process sampling is retained and does not prove GPU idleness. The task’s Xcode capture windows were closed before this milestone.

| Study | External CPU median | External CPU maximum |
|---|---:|---:|
| paired | 35.9% | 264.7% |
| aa | 47.3% | 261.7% |
| repeat | 39.6% | 362.7% |
| toggle | 37.4% | 205.8% |
| uncached | 35.8% | 248.4% |
| settled-aa | 49.1% | 219.1% |
| settled-repeat | 48.0% | 239.9% |

100% means one CPU core. Results cover these transformer workloads on one M2 Max; they do not establish performance on other GPUs, concurrent service workloads or genuinely cold driver caches. Production uses no Accelerate/MPSGraph numerical implementation.

Run `python3 native/benchmarks/fair/upload_cache_report.py` to audit the retained local frozen artifacts, recompute every paired ratio and regenerate this report. Raw JSON records contain commands, source/artifact hashes, all calibration/warm-up/measured observations and cache statistics. Large binaries and generated shader bundles remain ignored under `benchmarks/.build`.
