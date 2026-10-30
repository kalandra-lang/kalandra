# Reducing a GPU matrix graph

A float32 GPU helper can reduce the result of several matrix products and compute
with that scalar before returning:

```kalandra
fn score(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?],
         projection: Tensor[float32, ?, ?], count: float32) -> float32:
  hidden = matmul(input, weights)
  projected = matmul(hidden, projection)
  total = reduceSum(projected)
  total / count
```

Transfer the tensor arguments to the selected device, then use
`try await spawn score(input, weights, projection, count) on gpu`.
One scalar task owns all products, the final reduction and any scalar arithmetic.
Intermediate matrices remain on the GPU; only the final four-byte scalar is read
back. Repeated awaits observe the cached scalar outcome.

The product graph supports named/imported helpers, aliases, repeated inputs and
shared intermediates under the [matrix graph rules](native-gpu-matrix-graphs.md).
One row-major `reduceSum` consumes its final matrix. Strict scalar `+`, `-`, `*`,
`/`, unary signs, literals and named helpers can follow it. A reduced binding can
be reused, as in `total + total`, without rerunning the reduction. Returning the
reduction directly needs no scalar shader.

## Numerical and lifetime rules

Every product uses the existing separately rounded float32 products and
balanced-adjacent contraction tree. Each intermediate matrix is materialized
without reassociation. The final matrix is reduced by the existing row-major
balanced-adjacent tree. Scalar operations run once after reduction on the GPU,
with the same IEEE behavior, including division by zero.

An empty final matrix reduces to positive zero. Optional scalar arithmetic still
executes on that value; it is not bypassed. Empty product stages use the iterative
handoff, including long empty chains.

The complete product graph is validated before dispatch. Retained operands are
released after their final acknowledged consumer. The last product hands its
resident output directly to the reduction stage. The scalar shader runs in the
final reduction command before readback. Capture and device budgets, allocation
failures, cancellation, deadlines and device failures retain their checked task
behavior. Partial results are not published.

## Current boundary

This path handles a product graph with one final reduction.
[Multiple reductions](native-gpu-multiple-reductions.md) now also support earlier
intermediate matrices and independent rank-one/rank-two inputs contributing to
one final scalar expression. Unused extra stages, branches and reduced scalars
feeding later tensor work remain diagnostics. Wider types/ranks and device
control flow remain separate work.

See the [source example](../../native/metal-source/matrix-graph-reduction.k),
[qualification tests](../../native/metal-source/MatrixGraphReductionTests.cpp) and
[qualification record](../../native/metal-proof/evidence/gpu-graph-reduction-2026-10-21.json).
