# Upload-protecting cache candidate — 29–30 October 2026

Production runtime extends the accepted per-runtime upload cache with completed, fully overwritten Float32 prepared outputs. One combined 16 MiB / 64-entry idle cap remains inside the 64 MiB default payload budget. Last-owner retirement, zero admissions, fresh metadata and distinct shared/private keys preserve immutable ownership. No numerical, cancellation, checkpoint or shader change. Native C++ clients must rebuild for the extended Options/Stats layout.

## Correctness and scope

201 GPU runtime/support tests pass, including prepared-program integration in debug, optimized, ASan/UBSan and TSan modes. The dedicated fixture checks aliases, opposite prior bits (including NaNs/negative zero), changing ranks, storage-kind separation, exact budget pressure, all allocation-failure points, held acknowledgement, cancellation/deadlines, device failure, concurrency, policy changes, entry caps and teardown. This is not a full compiler/CLI suite rerun.

1,944 full-output comparisons pass against frozen accepted production across nine shapes, all three runner/upload modes, both switch orders and zero / 4 KiB / 16 MiB cache limits. Eighteen independent high-precision oracle checks pass. Signed zero survives decoding. Numerical comparisons precede timing and never overlap it.

## Same-runtime paired intervals

A single runtime, queue, prepared program and resident inputs span ABBA/BAAB intervals. Each slot trims idle backing and warms two complete intervals outside timing; all per-call execution remains timed. Five blocks, 1,024 calls per measured slot, two initial warm-up slots. GPU/encoding counters are enabled equally on both sides. AA disables private caching on both sides; toggle and repeat switch it on only for the current side. The original precision requirement is absolute AA wall median ≤2% and every block within ±5%; a failure limits the corresponding gain claim.

| Scope / study | Rows × width / upload mode | Wall reduction | Block range | GPU interval reduction | Encoding reduction | AA precision |
|---|---|---:|---:|---:|---:|---|
| largest / aa | 512 × 512 / 0 | -0.84% | -2.89% … -0.49% | -1.50% | +0.31% | pass |
| largest / toggle | 512 × 512 / 0 | +13.08% | +13.02% … +14.96% | +4.59% | +36.15% | — |
| largest / repeat | 512 × 512 / 0 | +14.00% | +12.92% … +16.93% | +5.47% | +35.07% | — |
| guards / aa | 1 × 512 / 0 | -5.54% | -36.78% … +19.64% | -14.00% | +0.38% | fail |
| guards / aa | 64 × 512 / 0 | -19.14% | -42.75% … +11.12% | -29.24% | -8.50% | fail |
| guards / aa | 512 × 512 / 2 | -3.77% | -5.95% … +3.67% | -4.63% | -2.71% | fail |
| guards / toggle | 1 × 512 / 0 | -0.49% | -30.85% … +28.94% | -4.45% | +31.31% | — |
| guards / toggle | 64 × 512 / 0 | +7.30% | -0.92% … +14.94% | -2.28% | +39.78% | — |
| guards / toggle | 512 × 512 / 2 | -0.07% | -3.82% … +2.83% | -0.50% | +3.60% | — |
| guards / repeat | 1 × 512 / 0 | +4.81% | -21.60% … +23.94% | -1.14% | +31.99% | — |
| guards / repeat | 64 × 512 / 0 | +9.30% | +7.91% … +17.95% | +1.64% | +38.37% | — |
| guards / repeat | 512 × 512 / 2 | -2.79% | -6.32% … +3.76% | -3.83% | -3.68% | — |

Positive reduction means less elapsed time. GPU command timestamps include stalls and scheduling; they are not exclusive arithmetic. Encoding scopes overlap and are not additive with GPU time. Instrumented within-runtime evidence is distinct from the uninstrumented frozen-production comparisons below.

## Uninstrumented frozen production comparison

Baseline is frozen accepted `dd940e07`; candidate uses the same shaders, compiler-generated program and task archive. No timing counters are enabled. Fresh-process adjacent ABBA/BAAB blocks use matching iterations, two warm-up rounds and three measured rounds per process, targeting one second per batch (8,192-call cap). Every per-invocation allocation, transfer, execution and cleanup stays inside timing. AA uses the identical enabled candidate on both sides. Uncached compares frozen production with private reuse disabled, preserving upload caching.

| Study | Rows × width / mode | Time reduction | Block range |
|---|---|---:|---:|
| aa | 512 × 512 / 0 | +0.18% | +0.11% … +1.11% |
| aa | 512 × 512 / 2 | -1.34% | -3.67% … +1.74% |
| production | 1 × 512 / 0 | +11.48% | +2.53% … +14.95% |
| production | 64 × 512 / 0 | +8.92% | +1.91% … +15.24% |
| production | 512 × 512 / 0 | +11.97% | +11.91% … +13.06% |
| production | 512 × 512 / 2 | -4.08% | -5.12% … +1.99% |
| repeat | 1 × 512 / 0 | +6.16% | +1.96% … +12.05% |
| repeat | 64 × 512 / 0 | +9.23% | +8.90% … +14.20% |
| repeat | 512 × 512 / 0 | +12.48% | +12.02% … +13.10% |
| repeat | 512 × 512 / 2 | -2.68% | -2.68% … +1.99% |
| uncached | 1 × 512 / 0 | +0.05% | -4.06% … +3.02% |
| uncached | 64 × 512 / 0 | +4.49% | +0.29% … +16.37% |
| uncached | 512 × 512 / 0 | +2.55% | +0.08% … +2.71% |
| uncached | 512 × 512 / 2 | +0.12% | -1.24% … +1.23% |

## Cache class behavior

Counters below are per measured call. Idle byte counts are post-interval snapshots, not peak live working sets. Both classes share the same cap.

| Scope / study | Rows / mode | Private enabled | Shared hits / misses | Private hits / misses | Shared / private idle bytes |
|---|---|---|---:|---:|---:|
| largest / aa | 512 / 0 | False | 0.00 / 0.00 | 0.00 / 0.00 | 0 / 0 |
| largest / toggle | 512 / 0 | False | 0.00 / 0.00 | 0.00 / 0.00 | 0 / 0 |
| largest / toggle | 512 / 0 | True | 0.00 / 0.00 | 8.00 / 0.00 | 0 / 11,534,336 |
| largest / repeat | 512 / 0 | False | 0.00 / 0.00 | 0.00 / 0.00 | 0 / 0 |
| largest / repeat | 512 / 0 | True | 0.00 / 0.00 | 8.00 / 0.00 | 0 / 11,534,336 |
| guards / aa | 1 / 0 | False | 0.00 / 0.00 | 0.00 / 0.00 | 0 / 0 |
| guards / aa | 64 / 0 | False | 0.00 / 0.00 | 0.00 / 0.00 | 0 / 0 |
| guards / aa | 512 / 2 | False | 7.00 / 0.00 | 0.00 / 0.00 | 13,631,488 / 0 |
| guards / toggle | 1 / 0 | False | 0.00 / 0.00 | 0.00 / 0.00 | 0 / 0 |
| guards / toggle | 1 / 0 | True | 0.00 / 0.00 | 8.00 / 0.00 | 0 / 20,484 |
| guards / toggle | 64 / 0 | False | 0.00 / 0.00 | 0.00 / 0.00 | 0 / 0 |
| guards / toggle | 64 / 0 | True | 0.00 / 0.00 | 8.00 / 0.00 | 0 / 1,327,104 |
| guards / toggle | 512 / 2 | False | 7.00 / 0.00 | 0.00 / 0.00 | 13,631,488 / 0 |
| guards / toggle | 512 / 2 | True | 7.00 / 0.00 | 0.00 / 8.00 | 13,631,488 / 0 |
| guards / repeat | 1 / 0 | False | 0.00 / 0.00 | 0.00 / 0.00 | 0 / 0 |
| guards / repeat | 1 / 0 | True | 0.00 / 0.00 | 8.00 / 0.00 | 0 / 20,484 |
| guards / repeat | 64 / 0 | False | 0.00 / 0.00 | 0.00 / 0.00 | 0 / 0 |
| guards / repeat | 64 / 0 | True | 0.00 / 0.00 | 8.00 / 0.00 | 0 / 1,327,104 |
| guards / repeat | 512 / 2 | False | 7.00 / 0.00 | 0.00 / 0.00 | 13,631,488 / 0 |
| guards / repeat | 512 / 2 | True | 7.00 / 0.00 | 0.00 / 8.00 | 13,631,488 / 0 |

## Resource tradeoff and limits

Maximum observed idle retention: 15,728,640 bytes; retained payload: 46,141,440 bytes, below the 64 MiB default. Driver/metadata memory is outside this pre-existing payload accounting. Live allocations evict idle entries when needed. Disable private retention or trim explicitly when idle memory matters.

The [rejected first production policy](gpu-prepared-output-initial-2026-10-29.md) retains the upload working-set regression. The revised pool never admits private backing by displacing cached uploads; upload admission and live-budget pressure prefer private eviction. The earlier [same-runtime diagnostic](gpu-private-inprocess-2026-10-29.md) retains failed small-workload controls, replay-cleanup evidence, the reported-DVFS correlation and prototype gains. Those numbers are not substituted for this production candidate. All unfavorable blocks remain. CPU load snapshots cannot prove GPU idleness; measurements run sequentially without local builds, tests, profiling or plotting. Dispatch, submission and logical allocation-attempt counts are unchanged within each matched workload. No vendor numerical library is used in production. Qualification precedes the implementation commit: each record’s revision is the checkout HEAD, while source/artifact hashes and worktree status identify the actual uncommitted candidate that ran. The auditor checks measured source identities, using preserved snapshots for superseded candidates.

## Interpretation and next experiment

Resident512×512 improves 11.97% and 12.48% against frozen accepted production, with passing large-case controls. The corresponding same-runtime reductions are 13.08% and 14.00%. Failed smaller-workload controls prevent a broad small-call claim.

Upload protection restores seven upload hits and zero misses, but this workload records no private hits and eight private misses per call. Its frozen-production cohorts still show 4.08% and 2.68% median slowdowns; the cache-disabled candidate is approximately flat (+0.12%). These are separate cohorts and must not be subtracted as a causal estimate. The profiled repeat also tracks GPU interval variation, so bookkeeping is not established as the sole cause.

Next remove unnecessary locking/scanning when the private pool is empty, and release private backing without taking the cache lock when a conservative pressure snapshot already proves it cannot be retained. This is a new candidate, requiring fresh qualification and performance evidence; these numbers are retained rather than substituted for the final runtime.
