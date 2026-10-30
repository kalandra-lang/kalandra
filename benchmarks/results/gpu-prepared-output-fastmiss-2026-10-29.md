# Qualified bounded prepared-output reuse — 29–30 October 2026

The runtime reuses completed, fully overwritten Float32 backing of validated pure prepared progression. A combined 16 MiB / 64-entry idle pool stays inside the existing 64 MiB default payload budget. Uploads have priority over private retention; scratch, transforms, flags, packed types and parameters keep their existing paths. No arithmetic, shader, checkpoint or cancellation-contract change. Native C++ clients must rebuild for the extended Options/Stats layout.

An atomic empty-private-pool snapshot skips pointless locking/scanning. A positive upload-pressure snapshot may conservatively discard private backing without locking; admission still rechecks under the mutex. Races can lose an optional cache hit, but cannot reuse pending backing or overspend the existing atomic budget. Logical allocation permits are unchanged.

## Qualification

201 GPU runtime/support tests pass, including prepared-program integration in debug, optimized, ASan/UBSan and TSan modes. Coverage includes aliases, opposite old contents (NaNs/negative zero), ranks, exact budgets, all allocation failures, held ACK, cancellation/deadlines/device failure, concurrent cores, policy changes, byte/entry caps and teardown. This is not a full compiler/CLI-suite rerun.

Fresh qualification passes 1,944 changing-input full-output comparisons against frozen accepted production across nine shapes, three cache caps, both switching orders and every runner/upload mode. Eighteen independent high-precision oracle checks pass. Signed zero is preserved. Qualification completes before timing.

## Uninstrumented frozen comparisons

Frozen baseline is accepted `dd940e07`; both sides use the same shaders, compiler-generated program, task archive, source algorithm and complete per-call lifecycle. No timing counters are enabled. Mode 0 keeps operands resident; mode 2 uploads every operand on each invocation (host tensors are prepared once equally on both sides). Three adjacent ABBA/BAAB process blocks, two warm-up and three measured rounds, matching iterations, adaptive one-second target, 8,192-call cap. Setup is excluded equally; all per-invocation work remains timed. AA controls all four cases with the identical enabled candidate. Uncached compares frozen production against the new candidate with private reuse disabled and uploads retained.

The predeclared precision goal is absolute AA median ≤2%, every block within ±5%. Failed controls limit corresponding claims. No sample is removed. Separate study medians must not be subtracted as causal estimates.

| Study | Rows × width / mode | Time reduction | Block range | AA precision |
|---|---|---:|---:|---|
| aa | 1 × 512 / 0 | +2.48% | -0.90% … +7.51% | fail |
| aa | 64 × 512 / 0 | -6.74% | -7.15% … +0.82% | fail |
| aa | 512 × 512 / 0 | -0.08% | -0.77% … +0.47% | pass |
| aa | 512 × 512 / 2 | -1.79% | -4.82% … +3.86% | pass |
| production | 1 × 512 / 0 | +4.87% | -7.99% … +9.66% | — |
| production | 64 × 512 / 0 | +6.87% | +3.85% … +13.88% | — |
| production | 512 × 512 / 0 | +11.44% | +10.81% … +12.48% | — |
| production | 512 × 512 / 2 | +3.06% | -2.27% … +12.17% | — |
| repeat | 1 × 512 / 0 | +8.67% | -14.06% … +11.55% | — |
| repeat | 64 × 512 / 0 | +9.31% | +1.46% … +15.15% | — |
| repeat | 512 × 512 / 0 | +11.32% | +10.96% … +11.95% | — |
| repeat | 512 × 512 / 2 | +6.04% | +1.56% … +11.24% | — |
| uncached | 1 × 512 / 0 | -0.08% | -5.47% … +3.34% | — |
| uncached | 64 × 512 / 0 | +0.77% | +0.15% … +4.62% | — |
| uncached | 512 × 512 / 0 | -1.00% | -2.12% … +2.79% | — |
| uncached | 512 × 512 / 2 | +5.07% | +2.11% … +7.98% | — |

## Interpretation

The accepted result is targeted: resident512×512 takes 11.44% less elapsed time against frozen production, independently repeated at 11.32%. Every block improves 10.81–12.48%; the identical-build control is -0.08% and stays within ±0.78%. The cache-disabled comparison is -1.00%. These are hot-invocation measurements of the same GPU program, not a universal language or numerical-library speed claim.

The upload-heavy guard changes from the preceding candidate’s negative medians to +3.06% / +6.04%, retaining seven upload hits and zero misses. Its block range is -2.27% to +12.17%; a passing AA control still spans -4.82% to +3.86%. The cache-disabled cohort also improves 5.07%, so these separate cohorts do not isolate private-cache benefit or establish that fast misses alone caused the timing difference. All outcomes remain retained.

Smaller resident controls fail, despite positive candidate medians. No reliable small-call total-time gain or zero-overhead guarantee is claimed. Earlier reported-DVFS correlation does not establish sole causality for these observations. Next profile GPU command/kernel time and dataflow on the final runtime; no numerical or cancellation relaxation is implied.

## Resources and provenance

Maximum observed idle retention: 15,728,640 bytes; retained payload: 46,141,440 bytes, below the existing 64 MiB default. Metadata and driver-internal memory remain outside this payload accounting. Private disable/trim and a zero-capacity option remain available.

A separate short counter diagnostic verifies seven upload hits / zero misses in the upload-heavy case. Its timings are not acceptance evidence. Submissions and logical allocation-attempt counts are positive and unchanged within every matched workload. Dispatch counters are disabled in uninstrumented runs; the separate instrumented counter diagnostic verifies positive, unchanged dispatch counts on both cache settings. All acceptance runs are sequential, without overlapping builds, tests or profiling. External CPU load is recorded and does not prove GPU idleness.

The [first policy](gpu-prepared-output-initial-2026-10-29.md) and [upload-priority follow-up](gpu-prepared-output-priority-2026-10-29.md) retain earlier gains, failed controls and regressions. Versioned reconstruction patches restore both superseded sources in isolated copies. The [same-runtime diagnostic](gpu-private-inprocess-2026-10-29.md) retains replay cleanup, reported-DVFS evidence and prototype observations; none replace final-runtime results. No production Accelerate/MPSGraph dependency. Source/artifact hashes identify the candidate qualified before committing; the checkout revision alone names its parent.
