# Scalar entry experiments and final qualification

Baseline: `6b27a812` on `native` (29/32 fair ordinary CPU cases within 10% of the faster C++/Rust median). The final candidate completed CPU/Metal qualification; experimental checkpoints below are retained. The user confirmed Docker is idle; measurements below ran without competing builds, tests or profilers.

## Architecture

The ordinary standalone entry now has a scalar-return fail-stop companion, with no status pointer or sentinel classification on successful calls. It transfers arithmetic failures to the same terminal reporting/cleanup boundary used by the old standalone main. Recoverable ordinary entries, checked task entries and cancellation behavior remain available and unchanged. The companion inherits the closed, resource-free scalar proof. Windows continues to use the existing ABI.

A two-register value/error companion supplies the intermediate proof and permits independent ABI comparisons. Valid NaNs, signed zeros, all ten numeric types, Bool, first-fault locations and cleanup are checked. Whole-record memset clears are included in fault-code tracking.

Exact positive dyadic expressions use bounded integer fixed point only when every source operation and intermediate is proven exact for all 65,536 guarded inputs. AArch64 emits its hardware scaled SCVTF; other targets retain strict floating IR. No fast-math flags are introduced. Wide or negative values retain the original implementation.

A separate guarded integer-loop specialization proves the nonnegative Euclidean remainder invariant and exposes its initial bound. The bound mask is an identity on every backedge. Negative divisors and unsupported forms retain the original code; overflow/fault paths are compared independently. Clone sizes and stack budgets are limited.

## Retained experiments

All raw results remain in this directory; frozen compilers and programs are under the ignored `benchmarks/.build/scalar-entry-2026-10-27/` directory.

- Countdown-only dispatch: regressed zero-round entries; rejected.
- Pair ABI and scalar ranges: modest GCD gain, no closure of the three misses; two idle runs remained 29/32.
- Finite coefficient-select dispatch: introduced zero/one regressions; rejected.
- Scalar-return direct ABI: removed successful-call status traffic; finite-path branch layout still limited short calls.
- Integer fixed point without explicit scaled instruction: LLVM merged unequal scales into a shared multiply; zero-round regression. Rejected.
- Hardware fixed conversion: 4,723,584 generated floating cases passed. Still three original misses because short success paths took an extra dispatch branch.
- Fall-through finite cases plus bounded GCD (`scalar-narrow-probe` and `scalar-narrow-repeat`): both 31/32. All three original misses closed; polynomial/16 regressed to approximately 11.8–11.9% behind. This is retained as a losing case, not omitted.

## Accepted and qualified candidate

The general scalar-range implementation now keeps unrestricted zero/one source bodies. When both are pure and LLVM deems every instruction safe to speculate, it computes and selects the original result bits without another dispatch. Fault publication, memory effects and unsafe operations reject this form. Longer guarded loops peel and exactly fold the first step. Tiny pure recurrences unroll four ways in source order, with no extra accumulators or FP reassociation, before verification/frame budgeting. The separate finite-case header was removed.

The first unrolled probe met the target in 32/32 cases, with lower medians than both references in 28. Frozen final and repeat matrices both retain 32/32 within 10%, with 27 and 26 lower medians respectively. The published baseline remains 29/32. Nine-block paired measurements, the independent Fraction/modular/divisor audit and artifact checks are complete. All raw exploratory and final data remain, including every losing case.

The full 1,279-target build passed. Current qualification selects 429 CPU tests and 50 real-device Metal tests, in separate CTest roots. Both selections subsequently passed without failures or skips; no performance measurements ran alongside them. Frozen compiler/runtime match the build outputs; source hashes are in the ignored qualification/inputs.json.

Numerical coverage now includes 4,724,480 loop/IEEE/first-fault cases (48 records, including a zero-case-success/next-step-overflow recurrence), 6,294,528 integer-domain cases (32 records), 262,144 mathematical unit comparisons, 1,744 typed ABI cases and 576 terminal fault/CLI comparisons. The full CTest run rechecks these.

One older recoverable closure-control sample at 4,096 rounds is variable: process medians were 16.3, 26.1 and 24.1 microseconds, against an earlier 16.2 microsecond median. The current and baseline hot-loop instructions and addresses match exactly; the code audit is retained. Do not hide this result or claim it is resolved. Once all regression jobs finish, run an adjacent baseline/current control comparison and another full control matrix on the quiet host. No timing overlaps tests/builds.


## Final qualification

429/429 CPU and 50/50 Metal tests passed, with no skips. All frozen inputs remain unchanged. The quiet matched legacy control baseline and repeat both qualify in 8/8 cases. Closure/4096 returned to 14.765 microseconds baseline and 14.921 current; the nine-block paired comparison did not reproduce a regression. Both control rerun and source/task paired audits passed. The earlier variable run and unchanged-code audit remain retained. The historical control required rebuilding because its protocol.py signature differed; the quiet comparison uses identical current harness inputs.

Canonical results and proof details are in scalar-entry-final-2026-10-27.md; numerical, qualification and log/JUnit records are saved beside it. Qualified for publication on native/PR260; issue closers #13, #233 and #135 are preserved.
