# Ordinary CPU entry and strict scalar loop qualification

Baseline: `6b27a812` on `native`. Measurements use the same source-level algorithms and validation driver on an Apple M2 Max/macOS 27.2 host. These are ordinary standalone calls; task cancellation is not removed to obtain the comparison. Windows retains its previous ordinary ABI. These results are not a universal speed claim or physical macOS 26 qualification.

Both frozen final runs meet the 10% threshold in **32/32 cases**, compared with **29/32** for the frozen baseline. Kalandra has the lower median than both references in **27/32** and **26/32** final cases. Every case and rejected experiment is retained.

## Change

Eligible closed, resource-free scalar entries return their scalar directly. There is no successful-call Invocation, status pointer or sentinel classification. Unhandled arithmetic faults transfer to the same terminal standalone reporting/cleanup boundary. The recoverable ordinary and checked task exports remain available; task cancellation, ownership and physical-stack behavior retain their contracts.

Zero/one-round source bodies remain unrestricted. When both are pure and LLVM proves them safe to speculate, their original result bits are selected without another dispatch. Longer loops can use a guarded 16-bit domain only when all folded dyadic operations and intermediates are exact. AArch64 uses its scaled hardware conversion. Tiny pure recurrences unroll four ways in source order, without extra accumulators, reassociation or fast-math flags. A separate nonnegative remainder invariant removes redundant integer overflow checks. Unsupported or wide inputs retain the original graph. Clone and frame budgets are checked.

## Full matrix

Times are nanoseconds per validated call. Relative time compares Kalandra with the faster C++/Rust median **within that run**; negative values mean less time. The repeat column uses its own reference measurements.

| Program | Rounds | Kalandra ns | C++ ns | Rust ns | Relative time | Repeat relative time | Paired time reduction vs baseline |
|---|---:|---:|---:|---:|---:|---:|---:|
| affine | 0 | 0.902 | 1.182 | 1.201 | -23.7% | -25.8% | +25.1% |
| affine | 1 | 0.897 | 0.894 | 0.891 | +0.7% | -1.2% | +25.6% |
| affine | 16 | 4.339 | 7.938 | 7.960 | -45.3% | -44.1% | +46.5% |
| affine | 256 | 358.601 | 370.898 | 364.684 | -1.7% | +6.2% | +0.5% |
| polynomial | 0 | 0.937 | 1.204 | 1.209 | -22.1% | -22.9% | +25.6% |
| polynomial | 1 | 0.939 | 0.889 | 0.900 | +5.6% | +5.2% | +22.3% |
| polynomial | 16 | 4.790 | 7.968 | 7.876 | -39.2% | -39.5% | +39.6% |
| polynomial | 256 | 360.487 | 360.526 | 391.416 | -0.0% | +6.0% | -2.2% |
| euclid | 0 | 1.201 | 1.194 | 1.204 | +0.6% | +2.2% | -0.2% |
| euclid | 1 | 2.975 | 3.293 | 3.925 | -9.7% | -8.8% | +21.2% |
| euclid | 16 | 161.598 | 152.472 | 153.297 | +6.0% | +8.2% | +2.3% |
| euclid | 256 | 3377.801 | 3104.718 | 3124.632 | +8.8% | +2.8% | +3.1% |
| array8 | 0 | 6.905 | 22.804 | 22.270 | -69.0% | -70.0% | -1.1% |
| array8 | 1 | 11.836 | 51.843 | 46.899 | -74.8% | -74.6% | +0.9% |
| array8 | 16 | 125.822 | 461.751 | 400.544 | -68.6% | -68.5% | +0.1% |
| array8 | 256 | 1983.974 | 7802.002 | 7057.318 | -71.9% | -72.5% | -4.0% |
| array16 | 0 | 13.870 | 25.610 | 23.726 | -41.5% | -43.0% | +0.5% |
| array16 | 1 | 28.357 | 62.586 | 58.681 | -51.7% | -53.6% | -1.1% |
| array16 | 16 | 259.421 | 577.247 | 566.950 | -54.2% | -53.8% | +0.1% |
| array16 | 256 | 3845.912 | 9450.968 | 8900.309 | -56.8% | -57.4% | -0.3% |
| array32 | 0 | 9.276 | 28.453 | 25.574 | -63.7% | -63.0% | -0.2% |
| array32 | 1 | 34.511 | 72.005 | 73.248 | -52.1% | -51.7% | +0.0% |
| array32 | 16 | 471.225 | 739.072 | 708.119 | -33.5% | -27.5% | +1.6% |
| array32 | 256 | 7432.949 | 11723.243 | 11575.533 | -35.8% | -33.3% | -1.3% |
| array64 | 0 | 20.606 | 39.659 | 31.265 | -34.1% | -34.6% | -0.5% |
| array64 | 1 | 80.440 | 108.638 | 92.262 | -12.8% | -12.2% | -1.2% |
| array64 | 16 | 935.880 | 1118.319 | 1011.628 | -7.5% | -8.5% | -1.4% |
| array64 | 256 | 14555.814 | 18042.365 | 17478.980 | -16.7% | -17.5% | -3.0% |
| array128 | 0 | 48.306 | 62.331 | 52.966 | -8.8% | -9.4% | +5.6% |
| array128 | 1 | 154.541 | 185.105 | 165.551 | -6.7% | -7.6% | -3.0% |
| array128 | 16 | 1770.852 | 2026.959 | 1923.651 | -7.9% | -7.7% | +4.1% |
| array128 | 256 | 27414.856 | 34950.680 | 32153.558 | -14.7% | -15.2% | -4.0% |

Three independent process medians per full run, adaptive warmed batching, exact returned-bit checks and wrapping integer checksums. The paired comparison uses nine adjacent ABBA/BAAB process blocks with identical iteration counts within each pair. No build, profiler or test work overlapped timing. CPU affinity, frequency and desktop activity were not isolated.

## Correctness and qualification

All **429 selected CPU tests and 50 real-device Metal tests passed**, with no failures or skips. The full 1,279-target build passed. Source/compiler/runtime hashes remained frozen throughout qualification.

Four-build matrices (debug, optimized, ASan/UBSan and TSan) verify **4,724,480** loop/IEEE/first-fault cases and **6,294,528** integer-domain cases. The loop matrix includes a successful zero case whose next step can overflow, checking effect/fault rejection. Typed ABI comparisons cover **1,744** cases across all ten numeric types and Bool; **528** direct/recoverable process outcomes and **48** compiler-generated main failures match exit status, output, source location and cleanup. Mathematical units additionally check **262,144** exhaustive values. Cancellation, ownership, stack, alias, replay, allocation-failure and Metal regressions remain covered by the full suite.

[Qualification manifest](scalar-entry-qualification-2026-10-27.json), [numeric records](scalar-entry-numerical-qualification-2026-10-27.json), [CPU log](scalar-entry-cpu-qualification-2026-10-27.log), [CPU JUnit](scalar-entry-cpu-qualification-2026-10-27.xml), [Metal log](scalar-entry-metal-qualification-2026-10-27.log), [Metal JUnit](scalar-entry-metal-qualification-2026-10-27.xml).

An independent Fraction/modular-power/divisor audit verified the baseline, both final matrices, paired arithmetic and frozen artifact hashes. The older recoverable-control matrix also passed its closed-form modular/checksum audit. Its 4,096-round closure row had variable process medians (16.3, 26.1, 24.1 microseconds); the baseline/current hot-loop instructions and addresses match exactly. The result is retained. A freshly rebuilt matched baseline and quiet repeat both qualify in **8/8 legacy control cases**. The 4,096-round closure measures **14.765 µs baseline / 14.921 µs current**, and the nine-block paired source comparison shows no reproduced regression (aggregate current time is 4.0% lower for that case; other source groups range from 2.2% lower to 3.3% higher). These small differences are not claimed as a speedup of the unchanged loop. Source and task-scope paired diagnostics passed independent oracle/statistical/artifact audits; task-scope startup timings are not cross-language comparisons.

## Evidence

- [Frozen baseline](scalar-entry-final-before-2026-10-27.json), [final](scalar-entry-final-2026-10-27.json), [repeat](scalar-entry-final-repeat-2026-10-27.json), [paired](scalar-entry-final-paired-2026-10-27.json), [independent audit](scalar-entry-final-audit-2026-10-27.json).
- [Recoverable control](scalar-entry-final-control-2026-10-27.json), [control oracle audit](scalar-entry-final-control-audit-2026-10-27.json), [unchanged control code audit](scalar-entry-control-code-audit-2026-10-27.json).
- [Matched control baseline](scalar-entry-control-before-2026-10-27.json), [quiet control repeat](scalar-entry-control-repeat-2026-10-27.json), [nine-block control pair](scalar-entry-control-paired-2026-10-27.json), [control repeat audit](scalar-entry-control-repeat-audit-2026-10-27.json), [paired control audit](scalar-entry-control-paired-audit-2026-10-27.json).
- [Retained experiment log](scalar-entry-worklog-2026-10-27.md).
