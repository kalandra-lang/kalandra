# GPU elementwise expressions after matrix multiplication

A numerical GPU helper can now compose strict arithmetic with one matrix result:

```kalandra
fn project(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?],
           bias: Tensor[float32, ?, ?], scale: float32) -> Tensor[float32, ?, ?]:
  product = matmul(input, weights)
  (product + bias) * scale
```

Transfer the tensor arguments explicitly, then spawn this helper on the GPU.
The bias must have the complete result shape: adding a vector to every row is
not implicit broadcasting. Scalar operands are lifted as in existing tensor
arithmetic, so `matmul(input, weights) + 1` also works. The elementwise expression
supports strict `+`, `-`, `*`, `/` and unary signs, with named/imported helpers,
aliases and re-exports.

One product binding can be used repeatedly, for example `product + product`,
without performing another multiplication. Operand identity survives reordered
parameters and repeated input buffers. A function can take one tensor and use it
for both operands, as in `matmul(values, values) + bias`, when its shape is square.

## Two resident stages under one task

The compiler proves exactly one strict matrix primitive. The multiplication
shader and its qualified tile alternatives are unchanged. A supplemental
`kernelN-post` shader evaluates the elementwise result expression, and the
manifest records its logical tensor/scalar counts, projected reads and binding
layout.

Logical post-stage slot zero holds the completed matrix. Other slots refer to
original tensor arguments. Only contributing tensors appear in the physical GPU
binding table; matrix operands with different shapes can remain unread by the
post stage. Direct bindings and Tier 2 argument buffers reuse the existing
layout selection, including a tested entry with 30 additional tensor operands.
This introduces no new fixed language parameter-count limit.

`spawn` returns one tensor task without waiting on its caller. The task retains
its immutable captures while multiplication runs. After acknowledgement, its
frame selects the matrix result as slot zero and submits the post stage using
the existing elementwise runtime. No argument pack or source tensor is mutated.
The intermediate remains resident; only an explicit later transfer downloads
the finished tensor. Each elementwise operation rounds separately after the
established separately-rounded products and balanced-adjacent contraction tree.
No arithmetic is fused into that tree or silently run on CPU.

All inferred and declared shape contracts are checked by the generated
supervisor before dispatch, including complete bias shape and unused parameter
obligations. Runtime validation also checks the post interface and selected
buffers. Empty output shapes retain their dimensions and validate the post
arguments without submitting GPU work. A zero inner dimension with nonempty
output still applies the post expression to the resulting positive-zero cells.

Both commands use the existing memory, argument and in-flight budgets. One
in-flight slot suffices for the sequence. Cancellation/deadlines stop subsequent
submission and retain active resources until acknowledgement. Failure in the
second stage never publishes the unadjusted matrix. The final immutable tensor
can outlive the task group and original inputs; the intermediate and capture
pack are released independently.

## Boundaries and qualification

[Arithmetic on multiplication operands](native-gpu-pre-matmul.md), such as
`matmul(input + 1, weights)`, now uses resident input stages within the same task. Multiple
independent matrix/reduction primitives, data-dependent GPU branches, recursive
helpers, other numeric types and higher ranks remain separate lowering work.
Supported pure arithmetic in unused bindings may be eliminated, while its shape
obligations remain checked; extra numerical primitives are not discarded.

[`MatmulPostTests.cpp`](../../native/metal-source/MatmulPostTests.cpp) compares
96 cases against CPU execution, separate GPU calls and an independent LLVM
APFloat reference. It covers empty/zero-inner shapes, odd contraction lengths,
signed zeros, subnormals, infinities, NaNs and arbitrary bit encodings. Allocation
failure sweeps, held-command cancellation/deadline/device failures, pending
scheduler destruction, invalid interfaces, a second-stage memory-budget failure
and retained results all require accounting to return to baseline.

[`VerifyPostMatmul.cmake`](../../native/metal-source/VerifyPostMatmul.cmake)
checks imports/re-exports, mixed argument order, tensor/scalar operands, a reused
product, one repeated tensor input, direct/Tier 2 bindings, full-shape faults,
relocation and byte-identical underlying multiplication shaders. CLI programs
run with 1/2/4 workers in Debug, optimized, ASan/UBSan and TSan modes. Source-only
checks admit operand transformations and preserve existing outputs on rejected extra
matrix operations and branches. The installed Release compiler additionally
builds a relocated application carrying its post shader after compiler removal.

The [qualification record](../../native/metal-proof/evidence/gpu-post-matmul-2026-10-21.json)
records hardware, modes and selected checks. This is correctness and lifetime
qualification; it makes no performance improvement claim.
