# Typed projection epilogues and private tensor reuse

This records the initial fusion work. Compiler-costed hardware float32 plans now
use [work-bounded progression](gpu-work-bounded-progression.md); other paths retain
the four-stage quantum described below.

Continue on `native` from the prepared-execution runtime at `ca7b5705`.
The compiler, runtime archives, generated program header and GPU executable
were frozen before edits in `benchmarks/.build/gpu-fusion-2026-10-28/baseline`.

1. Correct the stale LSP prefix-completion qualification expectation and rerun
   debug/sanitized protocol tests. Pushed as `a97e91b3`.
2. Extend the existing canonical matrix-epilogue lift to compiler-proven,
   non-faulting, single-input float32 numerical maps. Preserve uint bit carriers
   at the map boundary and separate float32 products/adjacent-pair reductions.
   Unsupported source/control/indexing keeps the original stages.
3. Fuse only adjacent matrix/map instructions with a single consumer and no
   intervening shape/control operation. Bind fresh scalar words. Keep both
   parameter buffers charged and count both logical operations toward the
   four-stage cancellation quantum. A fused oversized dispatch stays isolated.
4. Track invocation-produced buffer provenance. Reuse backing only in a
   non-faulting pointwise map when all remaining reads of that value occur in
   the current map and no other live value aliases it. Inputs, input aliases,
   escaping results, transforms and checked arithmetic are excluded. Reused
   owners remain charged through ACK and origins propagate through aliases.
5. Qualify IEEE bit patterns, live aliases, immutable inputs, fault injection,
   held cancellation/deadline/device failure, tight budgets and all runtime
   modes. Verify every full-block output before isolated paired timing.
6. Record pipeline setup and plan preparation separately, compare all 27
   workload/residency cases against the frozen runtime, profile separately,
   retain losing/uncertain cases and rerun relocated distribution qualification.
   Report the result and push to the existing native PR.

Production arithmetic and cancellation contracts remain unchanged. The native
execution API does not add language ownership annotations or user-visible
mutation. Vendor numerical APIs remain benchmark-only controls.

## Completion

All six steps are complete locally. Profiling verifies 14 dispatches instead of
16, 4/7 submissions instead of 4/8, and 112/115 hot allocation/admission
checkpoints instead of 127/131 for the 1/64-row 512-wide blocks.

Across five paired ABBA/BAAB blocks per case, 22 of 27 medians improve and five
are 0.2–1.2% slower with mixed block signs. The largest resident case uses 9.9%
less API time and the all-upload case uses 5.3% less. The resident large block is
4.90× faster than CPU in the separate library-control comparison. Small cases
still favor CPU and MPSGraph remains faster. Program/pipeline setup adds about
0.42 ms; no universal warm or cold speedup is claimed.

Four-mode lifetime, alias, IEEE-bit, fault-injection and cancellation gates,
typed matrix/APFloat and row-math gates, canonical codegen checks and relocated
Metal distribution execution passed. Frozen comparison dumps match every bit;
full-model reference checks cover 476,455 cells. CPU LLVM is byte-identical.

[Report, chart, losing cases and raw evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/gpu-fusion-execution-2026-10-28.md).
The next architectural experiment is GPU-side graph progression with explicit
device-visible cancellation/fault gates. Residual epilogues across validated
shape guards remain another candidate; neither is implemented in this step.
