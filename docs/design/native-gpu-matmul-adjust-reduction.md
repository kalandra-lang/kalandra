# GPU arithmetic between matrix multiplication and reduction

A float32 GPU helper can transform a matrix result before reducing it:

```kalandra
fn score(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?],
         bias: Tensor[float32, ?, ?], scale: float32) -> float32:
  product = matmul(input, weights)
  reduceSum((product + bias) * scale)
```

The intermediate expression supports strict `+`, `-`, `*`, `/`, unary signs,
scalar lifting and full-shape tensor operands. Bindings and named/imported
helpers can forward these values, and a product binding can be used repeatedly,
as in `reduceSum(product + product)`, without another multiplication. Bias must
have the complete matrix result shape; there is no implicit axis broadcasting.

The existing matrix input maps compose with this path too:

```kalandra
product = matmul(input + scale, weights - 1)
reduceSum((product + bias) * scale)
```

Transfer inputs explicitly and spawn the helper on the GPU. One scalar task
owns optional left/right input maps, multiplication, a matrix-result adjustment,
and the final reduction. Intermediates stay resident and only the final
four-byte scalar is read back. Stages use separate output storage/commands;
this implementation does not claim fusion or a measured speed improvement.

## Extraction and numerical boundaries

The compiler proves one matrix primitive consumed by one reduction. Its scalar
SSA pass extracts the expression passed to `reduceSum`, treating the completed
matrix as logical tensor slot zero. Other logical slots select original immutable
tensor arguments. Per-stage liveness gives the adjustment its own projected read
table; direct bindings and Tier 2 argument buffers reuse the existing machinery.
The `kernelN-post` shader is the matrix epilogue, before `kernelN-reduce`.

Optional input maps round each operation before multiplication. Separately
rounded products and the balanced-adjacent contraction tree remain unchanged.
Each intermediate arithmetic operation then rounds before the final row-major
balanced-adjacent sum of matrix cells. No operations move across either tree,
and the base multiplication/reduction artifacts are byte-identical to those in
separate placed calls. The compiler never silently runs an unsupported stage
on CPU.

Transitive shape preconditions remain supervisor guards before dispatch,
including complete bias shapes even when the element counts happen to match.
Runtime validation checks the matrix adjustment's interface and actual readable
buffers too. Logical slot zero must consume the completed matrix; original
inputs are not mutated or substituted for that result.

## Ownership, empty shapes and failure

The runtime reuses acknowledged-stage handoffs. After multiplication acknowledges,
its resident result selects adjustment slot zero. The adjustment then becomes the
reduction input. Original argument captures remain available while needed, and
only scalar-layout completion is published. A cached scalar can outlive all
original buffer owners.

Empty outputs validate the adjustment arguments and return the canonical empty
sum without GPU commands for empty stages. A zero inner dimension with nonempty
output still produces zero matrix cells, applies the adjustment, and reduces
those cells. Thus a nonzero bias contributes even when the matrix contraction
is empty. One in-flight slot suffices; memory/capture budgets and allocation
checkpoints apply throughout. Cancellation, deadlines and device/allocation
failures stop subsequent stages and retain active resources until acknowledgement.
A failure never publishes the product or adjusted tensor as the scalar result.

## Boundaries and qualification

[Scalar arithmetic after this composed reduction](native-gpu-matmul-scalar-reduction.md)
now has its own qualified GPU epilogue. Additional matrix/reduction primitives,
recursive helpers and data-dependent GPU branches remain separate
lowering work. Existing CPU stack admission remains unchanged. Broader GPU types,
ranks, custom shaders and NVIDIA/CUDA retain their separate roadmap scope.

The shared [`MatmulReductionTests.cpp`](../../native/metal-source/MatmulReductionTests.cpp)
harness qualifies the adjustment fixture in Debug, optimized, ASan/UBSan and TSan
modes. Each mode compares 192 plain/input-transformed cases with checked CPU
execution, separate GPU calls and LLVM APFloat, including empty/zero-inner shapes,
odd trees, signed zeros, subnormals, infinities, NaNs and arbitrary encodings.
It sweeps 54 allocation checkpoints, checks cancellation/deadline/device failure
in each of five command stages, pending scheduler destruction, invalid adjustment
interfaces, a later-stage budget failure, one in-flight slot and retained scalars.

[`VerifyMatmulAdjustmentReduction.cmake`](../../native/metal-source/VerifyMatmulAdjustmentReduction.cmake)
checks imports/re-exports, tensor/scalar/unary arithmetic, a reused product,
optional input maps, 31 contributing adjustment inputs via Tier 2, equal-count
but unequal bias shapes, relocation and unchanged primitive artifacts. Programs
run with 1/2/4 workers in all four modes. Unsupported graphs preserve existing
outputs. The installed Release compiler builds an application carrying all four
supplemental shaders, then the relocated application runs after compiler removal.

See the [qualification record](../../native/metal-proof/evidence/gpu-matmul-adjust-reduction-2026-10-21.json)
for selected checks and hashes. General numerical graph support remains incomplete.
