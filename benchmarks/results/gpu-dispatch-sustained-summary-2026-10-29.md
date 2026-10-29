# Sustained encoder diagnostic

This repeats the exact frozen counter executable after the short-burst diagnostic. Five fresh processes per shape run 256 warmup calls and three batches of 128 measured calls. Shape order is shuffled. There are 12,800 calls in total; all per-batch resource, shape, dispatch and submission checks pass. The unchanged executable retains the original eight validated full-output checks; timed calls do not read every output cell.

These are instrumented diagnostic times, not acceptance or a comparison with MPSGraph. Both the runtime profiler and encoder counters are enabled. Each interval can include scheduling and synchronization, and interval sums can overlap. Complete stdout/stderr is retained in compressed ignored experiment storage with hashes; the tracked evidence contains all per-batch distributions.

| Tokens × width | Command GPU median µs | Process range µs | FFN share of summed intervals |
|---|---:|---:|---:|
| 1 × 64 | 405.65 | 400.60–427.33 | 26.75–26.87% |
| 1 × 512 | 412.42 | 406.31–431.50 | 27.00–27.33% |
| 16 × 512 | 760.02 | 736.50–803.90 | 41.37–41.71% |
| 64 × 512 | 1360.73 | 1351.88–1369.62 | 43.33–43.47% |

## Largest-shape encoder intervals

| Instruction | Operation | Median µs | Process range µs |
|---:|---|---:|---:|
| 32 | FFN contraction + residual | 345.94 | 342.44–346.42 |
| 29 | FFN expansion + ReLU | 336.35 | 335.48–336.94 |
| 22 | Probabilities × V | 175.88 | 175.27–177.65 |
| 15 | V projection | 162.65 | 161.90–163.67 |
| 13 | K projection | 143.63 | 138.58–148.85 |
| 24 | Output projection + residual | 103.67 | 102.88–104.92 |
| 11 | Q projection | 102.17 | 100.67–103.42 |
| 18 | QKᵀ + scale | 66.58 | 57.81–68.96 |
| 16 | K transpose | 49.60 | 47.56–57.96 |
| 20 | Softmax | 30.40 | 30.25–30.58 |
| 9 | Attention RMSNorm | 28.00 | 27.69–28.29 |
| 27 | FFN RMSNorm | 25.58 | 25.48–25.83 |

## Decision and limitations

The large-shape command interval now varies by about 1.3% across process medians, instead of roughly threefold in the short-burst experiment. FFN expansion and contraction remain the largest combined target at 43.33–43.47% of summed median intervals. Sustaining activity reduces this observed variation; it does not prove whether frequency, startup state, competing activity or instrumentation caused the original drift.

Next test FFN tile/data reuse while keeping per-cell arithmetic order. A kernel candidate still needs separate unprofiled paired/control/replication timings, independent numerical checks and whole-block/upload regressions. This diagnostic does not justify reassociation or a numerical-contract change, and does not show that the FFN owns 43% of application wall time.

Two collector assertions failed before the completed sweep: one forgot that observation rows include two warmups; the other incorrectly expected profiling counters to be zero in this deliberately profiled executable. The first raw capture and both failure logs remain retained. The corrected collector validates warmups, exact dispatch counts and nonzero profiler values; it reused already captured valid samples rather than dropping slow outcomes. No runtime or shader was changed.

[Evidence](gpu-dispatch-sustained-2026-10-29.json) · [Unprofiled acceptance](gpu-closed-entry-2026-10-29.md) · [Short-burst diagnostic](gpu-dispatch-counter-summary-2026-10-29.md)
