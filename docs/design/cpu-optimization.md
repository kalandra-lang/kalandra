# CPU tensor optimization

**Qualification correction:** This suite does not establish Rust/C++ compiler-performance parity. It compares built-in tensor implementations with handwritten kernels that use different fusion, allocation and matrix strategies. The requested general performance target remains unestablished. See [benchmark methodology](benchmark-methodology.md).

The 2026-10-24 optimization work targets the existing language-comparison kernels against the faster of the Rust and C++ baselines. The acceptance target is Kalandra CPU time at most 1.10 times that baseline, for every measured size and both float32/float64 profiles. This target describes the recorded mathematical operations on the qualified host; the benchmark protocol and comparator implementations remain unchanged.

## General compiler changes

Simple innermost, zero-origin, unit-step counted loops execute in chunks of at most 256 iterations. Cancellation polling is placed before each chunk, outside its inner arithmetic/copy loop. This lets LLVM vectorize the inner loop while retaining bounded cooperative cancellation. Loops with calls, irregular induction, extra loop-carried state, or volatile/atomic accesses retain the previous polling path. Headers with memory accesses are excluded so chunk boundaries cannot duplicate observable accesses. Entry checks, checked faults, call budgets and invocation-owned cleanup remain active.

Hardware floating maps immediately consumed by a balanced sum can fuse into the first adjacent-pair level. Products and additions remain separate operations, each pair retains its original grouping, and odd leaves carry unchanged. This removes materialized map tensors and the full-sized reduction copy; a private half-sized scratch allocation holds the first-level sums. Inputs remain immutable. New fusion admits pure numeric symbol/literal leaves and already evaluated values, avoiding reordering effectful calls and checked conversions. Software arithmetic keeps its established lowering and exact-bit qualification.

One-dimensional fused reductions check input strides once. Unit-stride inputs take a read-only contiguous path; other layouts use the general tensor indexing path. Nonzero offsets and positive/negative strides retain their descriptor semantics. The outermost reconstructed coordinate no longer performs a redundant remainder/division in general row-major reductions.

The existing tiled SIMD matrix implementation now handles hardware float64 as well as float32. Vector elements, scratch sizes and byte-overflow checks use the actual element width. Integer and explicit software matrix arithmetic retain the scalar checked path. Matrix products keep separate multiplication/addition and the balanced contraction tree; no fast math, reassociation or FMA contraction is enabled.

These transformations operate on general compiler IR and numerical expressions. They contain no benchmark function-name or measured-size dispatch.

## Correctness qualification

`native-cpu-optimization` compiles five independent source kernels in both floating types and all four CPU build variants. Its C++ harness compares results against a recursive specification of the balanced tree. It checks empty and singleton inputs, odd lengths, 255/256/257 and 511/512/513 boundaries, larger tails, nonzero offsets, positive and negative strides, input immutability, exceptional floating values, allocation failures, shape faults, mid-loop cancellation and complete cleanup. Matrix controls cover row/column tiles and tails, empty axes, and contraction lengths on both sides of the 256-element blocking boundary. Higher-rank controls cover rank three, rank eight, empty interior dimensions and long expression chains.

The harness also exports matrix results directly and checks every output cell, including shape and descriptor strides, rather than relying on a final checksum. Both float widths run reproducible seeded finite bit patterns spanning exponent gaps, cancellation and halfway/adjacent representable values. Isolated exceptional operand pairs prevent an unrelated NaN in a reduction from hiding a finite-value error. A multiply/add contraction witness distinguishes separately rounded arithmetic from FMA. These checks run in optimized, unoptimized, address/undefined-sanitized and thread-sanitized builds through the existing CTest registration.

Native floating NaN payload differences are permitted by hardware policy; all other reference results are compared bitwise. Existing strict software conformance and checked integer tests remain in the broader regression selection. Performance thresholds are assessed from the recorded full benchmark runs, not placed in timing-sensitive CI tests.

## Recorded result

The [CPU tensor implementation report](../../benchmarks/results/cpu-parity-2026-10-24.md) records all 12 cases below the 1.10× target. Kalandra CPU measured 0.409–0.755× the faster Rust/C++ implementation (24.5–59.1% faster). Full float32 and float64 profiles each retain 42 cells, 126 fresh processes, 882 measured batches and 252 warmup batches, including both GPU modes. Comparator sources, tool versions and the sampling protocol were checked against the preceding hardware-policy runs and are unchanged. Source/compiler hashes, arithmetic artifacts and all checksums were verified.

The full native build passed. Across the broad run and targeted reruns, all 144 selected native tests passed. This included the initial 32-combination optimizer qualification and higher-rank/long-expression controls. All 39 extracted documentation blocks also passed. Active-cancellation probes synchronize sender startup and spin until the chosen poll point, preventing a scheduler yield from outlasting a fast operation; their cancellation, non-publication and cleanup assertions remain unchanged.

The expanded numerical qualification passed all 40 type/kernel/build combinations (five kernels, two float widths, four build variants), including direct per-cell matrix checks and the seeded/rounding corpora described above. The CPU and physical Metal arithmetic-policy regression tests also passed on the qualification host.

## Follow-up after the fair comparison

The [checked-call optimization](checked-call-optimization.md) inlines the small admission/status paths and caches physical stack metadata for a synchronous native chain. Cancellation-bound calls keep acquire token loads; observer/fault/settlement paths retain ordinary runtime polling. Logical depth and stack reserve remain checked.

Hardware float32 matrix kernels form thirty-two-leaf adjacent-pair subtrees (float64 retains sixteen leaves) in SSA/registers, store only their roots, and reduce those roots in the original tree order. Partial groups use the unpadded reference reduction. Float32 uses two-row/eight-column tiles; float64 uses one-row/eight-column tiles to bound vector register pressure. Scratch size follows the number of roots plus a disjoint tail workspace, with checked byte arithmetic. The obsolete 256-product materialization helper is removed.

A proven unit column stride admits a dense right-operand path. For at least 32 rows/columns and contraction length 128, full eight-column panels are packed once per invocation. Inner products stream those panels instead of walking the full right-hand row pitch. Row strides and offsets remain dynamic, including reversed rows; trailing columns retain ordinary descriptor reads. Packing and its allocations are included in complete-call timings. General column-strided layouts retain their fallback.

The expanded per-cell tests cover 7/8/9, 15/16/17 and 31/32/33 boundaries, packing thresholds, row/column tails, reversed right-hand row strides, every allocation failure in a packed operation and cancellation during packing. Both float widths and all four build modes retain exact comparison. No FMA, reassociation or zero-padding is introduced. The [fair follow-up](../../benchmarks/results/optimization-2026-10-24.md) reports the remaining gaps rather than qualifying compiler parity from tensor kernels.

The [checked scalar tail-call follow-up](../../benchmarks/results/tail-calls-2026-10-24.md) optimizes source recursion and proven-total hardware cast admission. It preserves the numerical policy and existing matrix kernels; the new report makes no new matrix or GPU timing claim.

The [bounded checked-call inlining follow-up](../../benchmarks/results/checked-inlining-2026-10-24.md) expands one level of small scalar calls while preserving logical admission/polls and the combined stack budget. Interleaved baseline/current token controls accompany a fresh five-language source comparison. Static tensor-to-buffer shapes are also preserved explicitly, with new small/packed matrix and fused-reduction controls.

The [recursive context follow-up](../../benchmarks/results/recursive-context-2026-10-24.md) specializes proven scalar call chains and their existing pure tail loops. It retains all logical checks and token polls, verifies actual frames and dispatcher expansion budgets, and preserves direct observed recursion. The same source algorithms and fresh C++/Rust/Go/Python comparison remain the performance boundary.

The [matrix tuning follow-up](../../benchmarks/results/matrix-tuning-2026-10-25.md) increases the float32 register subtree from 16 to 32 leaves. A paired trial found float64 regressions, so float64 retains its existing 16-leaf implementation. The tail workspace and root allocation use the same compiler constant. Exact per-cell tests additionally cover 63/64/65, 95/96/97 and packed 159/160/161 contractions. The paired runner retains all small-size and float64 controls; this does not establish performance parity with reassociated numerical libraries.
