# Prepared typed GPU execution

Work continues on `native` from `27522768`. Baseline compiler, runtime, driver,
generated instruction header and executable are frozen before edits.

1. Separate immutable validated instructions, edges, scalar schema and initial
   use counts from per-invocation values, captures, counters and program position.
   Prepared plans retain kernels and their registry; concurrent invocations never
   share mutable parameters or temporary storage.
2. Reuse immutable map argument schemas. Bind runtime scalar words per invocation
   and revalidate widths. Dynamic shapes and device residency remain checked.
3. Encode proven non-faulting stages in chunks of at most four logical stages and
   64 Mi estimated work. Keep branch/readback and unproved arithmetic boundaries.
   Host RMS epsilon must be valid before its stage can be considered non-faulting;
   device epsilon remains an acknowledgement boundary.
4. Retain parameters, scratch, argument resources and all deferred last-use owners
   until the chunk acknowledges. On a later preparation failure, acknowledge
   already encoded work before reporting the deferred error. Tight budgets keep
   the individual-stage path.
5. Integrate prepared plans into generated host frames, with per-call scalar
   bindings and device-aware invalidation. Test generated host code, changing captures,
   concurrent calls, shape/arithmetic faults, allocation injection, cancellation,
   deadlines, device failure and teardown in all runtime modes.
6. Validate complete outputs, then measure the same frozen full-block shaders
   against baseline in resident and transfer-inclusive modes. Profile separately;
   record losing cases and the remaining MPSGraph gap.

No floating arithmetic order, first-fault contract, input immutability or task
cancellation guarantee is weakened. Vendor APIs remain benchmark-only.

## Completion

All six steps are complete locally. The source compiler emits device-aware
prepared plans, refreshing captures per invocation. The same prepared API
supports every existing numeric format; batching currently admits proven
non-faulting float32 operations. Other formats and checked operations retain
individual acknowledgement boundaries.

Qualification passed in optimized, debug, ASan/UBSan and TSan modes, including
127 injected hot-path allocation checkpoints, changing captures, concurrent
invocations, immutable inputs, held cancellation/deadline/device failures,
deferred host-fault precedence and tight workspace budgets. The all-ten-type
source/runtime gates and relocated Metal distribution passed.

Five paired ABBA/BAAB blocks for each of 27 shape/residency cases all improved;
case-median time reductions span 18–66%. Nine baseline/candidate output dumps
are bit-identical and generated shader source is unchanged. The full block also
passed independent every-cell CPU/Metal/MPSGraph checks across 13 shapes.

[Report, chart, cold preparation cost, remaining vendor gap and raw evidence](../../benchmarks/results/gpu-prepared-execution-2026-10-28.md).
Cancellation remains cooperative: an already submitted chunk settles at its
ACK, with retained resources, rather than interrupting a running kernel.
