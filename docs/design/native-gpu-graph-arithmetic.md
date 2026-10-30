# Arithmetic within GPU matrix graphs

Rank-two float32 matrix graphs can now contain tensor arithmetic before, between
and after products:

```kalandra
fn project(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?],
           bias: Tensor[float32, ?, ?], projection: Tensor[float32, ?, ?],
           scale: float32) -> Tensor[float32, ?, ?]:
  hidden = matmul(input, weights)
  shifted = -(hidden + bias) / scale
  matmul(shifted, projection)
```

After explicit transfers, place this helper with `spawn project(...) on gpu`.
One tensor task owns the full sequence and returns a resident tensor. A final
`reduceSum` and optional scalar arithmetic can instead return a scalar through
[the graph reduction path](native-gpu-graph-reduction.md).

Supported operations are strict float32 `+`, `-`, `*`, `/` and unary signs, with
tensor/tensor or scalar/tensor operands in either order. Scalar parameters,
literals, scalar expressions and named/imported helpers compose with these
operations. Tensor operands must have equal complete shapes; this does not add
broadcasting. Shared intermediate tensors and product branches can feed later
arithmetic without recomputing the producer.

## Stages and ownership

The compiler records an ordered graph of matrix and arithmetic stages. Each
arithmetic shader reads only its one or two distinct tensor operands. Original
scalar parameters are copied into immutable stage captures. The logical graph
can contain many tensors without binding all of them to each shader.

The runtime validates all residence, shape and stage-kernel contracts before the
first dispatch. A single task selects the next matrix or arithmetic kernel after
acknowledgement, keeps intermediate tensors resident and releases retained
operands after their last consumer. Stage input handles select graph values
without mutating the scalar capture packs. Cached and retained final tensors
keep their existing independent ownership.

Every tensor operation currently has a separate resident stage. Intermediate
results are stored as float32. Matrix multiplication keeps its separately rounded
products and balanced-adjacent contraction tree; arithmetic retains source operand
order and strict rounding. No reassociation, multiply-add contraction or implicit
CPU transfer is introduced. Adjacent-stage fusion is later optimization work.

Empty outputs advance iteratively without submitting a command. Cancellation,
deadlines, allocation/device failures and resource budgets remain checked at
stage boundaries. Metadata and per-map capture payloads count against the argument
budget; resident and staging storage count against the device-storage budget.

## Current boundary

This extends graphs with at least two matrix products. Every tensor stage must
contribute to the returned tensor or scalar expression.
[Multiple reductions](native-gpu-multiple-reductions.md) can now feed that final
scalar expression. Extra unused stages, branches, broader GPU numeric types and
higher tensor ranks remain unsupported. Existing single-product arithmetic paths
remain available.

Helper analysis, scalar IR expansion and aggregate graph-shader expansion have
explicit work limits. An over-budget graph produces a diagnostic before output
artifacts are published.

See the [source example](../../native/metal-source/matrix-graph-arithmetic.k),
[device tests](../../native/metal-source/MatrixGraphArithmeticTests.cpp) and
[qualification record](../../native/metal-proof/evidence/gpu-graph-arithmetic-2026-10-21.json).
