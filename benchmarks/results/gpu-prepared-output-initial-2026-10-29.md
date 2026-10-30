# Prepared-output reuse: rejected first eviction policy — 29 October 2026

Production runtime extends the accepted per-runtime upload cache with completed, fully overwritten Float32 prepared outputs. One combined 16 MiB / 64-entry idle cap remains inside the 64 MiB default payload budget. Last-owner retirement, zero admissions, fresh metadata and distinct shared/private keys preserve immutable ownership. No numerical, cancellation, checkpoint or shader change. Native C++ clients must rebuild for the extended Options/Stats layout.

## Correctness and scope

201 GPU runtime/support tests pass, including prepared-program integration in debug, optimized, ASan/UBSan and TSan modes. The dedicated fixture checks aliases, opposite prior bits (including NaNs/negative zero), changing ranks, storage-kind separation, exact budget pressure, all allocation-failure points, held acknowledgement, cancellation/deadlines, device failure, concurrency, policy changes, entry caps and teardown. This is not a full compiler/CLI suite rerun.

1,944 full-output comparisons pass against frozen accepted production across nine shapes, all three runner/upload modes, both switch orders and zero / 4 KiB / 16 MiB cache limits. Eighteen independent high-precision oracle checks pass. Signed zero survives decoding. Numerical comparisons precede timing and never overlap it.

## Same-runtime paired intervals

A single runtime, queue, prepared program and resident inputs span ABBA/BAAB intervals. Each slot trims idle backing and warms two complete intervals outside timing; all per-call execution remains timed. Five blocks, 1,024 calls per measured slot, two initial warm-up slots. GPU/encoding counters are enabled equally on both sides. AA disables private caching on both sides; toggle and repeat switch it on only for the current side. The original precision requirement is absolute AA wall median ≤2% and every block within ±5%; a failure limits the corresponding gain claim.

| Scope / study | Rows × width / upload mode | Wall reduction | Block range | GPU interval reduction | Encoding reduction | AA precision |
|---|---|---:|---:|---:|---:|---|
| largest / aa | 512 × 512 / 0 | +0.94% | -3.82% … +3.42% | +0.74% | +0.60% | pass |
| largest / toggle | 512 × 512 / 0 | +13.48% | +12.19% … +15.94% | +5.47% | +33.51% | — |
| largest / repeat | 512 × 512 / 0 | +13.93% | +11.09% … +15.25% | +4.79% | +37.85% | — |
| guards / aa | 1 × 512 / 0 | +0.28% | -28.41% … +16.63% | -1.69% | -2.17% | fail |
| guards / aa | 64 × 512 / 0 | +4.25% | -29.65% … +28.40% | +5.95% | +0.97% | fail |
| guards / aa | 512 × 512 / 2 | +1.61% | -4.84% … +8.62% | +2.17% | +1.71% | fail |
| guards / toggle | 1 × 512 / 0 | -2.24% | -25.72% … +25.69% | -11.06% | +31.42% | — |
| guards / toggle | 64 × 512 / 0 | +12.99% | -8.88% … +21.85% | +3.62% | +36.63% | — |
| guards / toggle | 512 × 512 / 2 | -2.09% | -5.77% … -1.69% | -0.29% | +17.08% | — |
| guards / repeat | 1 × 512 / 0 | -13.06% | -23.30% … +28.75% | -34.83% | +27.85% | — |
| guards / repeat | 64 × 512 / 0 | -1.23% | -17.32% … +6.35% | -18.33% | +36.40% | — |
| guards / repeat | 512 × 512 / 2 | -3.29% | -7.59% … -0.57% | -2.39% | +17.63% | — |

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
| guards / toggle | 512 / 2 | True | 6.00 / 1.00 | 4.00 / 4.00 | 8,388,608 / 8,388,608 |
| guards / repeat | 1 / 0 | False | 0.00 / 0.00 | 0.00 / 0.00 | 0 / 0 |
| guards / repeat | 1 / 0 | True | 0.00 / 0.00 | 8.00 / 0.00 | 0 / 20,484 |
| guards / repeat | 64 / 0 | False | 0.00 / 0.00 | 0.00 / 0.00 | 0 / 0 |
| guards / repeat | 64 / 0 | True | 0.00 / 0.00 | 8.00 / 0.00 | 0 / 1,327,104 |
| guards / repeat | 512 / 2 | False | 7.00 / 0.00 | 0.00 / 0.00 | 13,631,488 / 0 |
| guards / repeat | 512 / 2 | True | 6.00 / 1.00 | 4.00 / 4.00 | 8,388,608 / 8,388,608 |

## Rejected first policy

The large resident control passes (0.94% median drift, every block within ±3.83%). Enabled reuse reduces elapsed time 13.48%, independently repeated at 13.93%. Small-workload controls remain noisy and do not support small-call claims.

The upload-heavy case regresses 2.09% in the first cohort and 3.29% in replication. Every block in both cohorts is slower. Class counters expose one upload miss per call (six hits), replacing the accepted seven upload hits. Four private hits do not compensate. The unchanged 16 MiB combined cap is bounded but the eviction policy lets private retention displace an upload working set. This policy is rejected; the next candidate protects existing uploads and prefers private eviction. All controls, numerical qualification, tests and negative samples remain retained. A [versioned reconstruction patch](../../native/benchmarks/prepared-output-initial-policy.patch) restores this rejected source in an isolated copy; frozen local sources remain the original identity reference.

The uninstrumented comparator was prepared but never measured: its waiting coordinator was stopped before launch after this regression was identified. Do not infer any frozen-production performance from its manifest.

Correctness passes 201 runtime/support tests and 1,944 full comparisons plus 18 oracles; this performance rejection does not imply numerical failure. No numerical, checkpoint, ownership or cancellation contract changes.
