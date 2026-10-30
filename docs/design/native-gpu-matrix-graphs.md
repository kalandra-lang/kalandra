# GPU matrix product graphs

A placed float32 helper can perform several matrix multiplications without
returning intermediate matrices to the host:

```kalandra
fn project(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?],
           projection: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  hidden = matmul(input, weights)
  matmul(hidden, projection)
```

Transfer the three arguments to the selected GPU, then use
`spawn project(input, weights, projection) on gpu` and `try await` its result.
The returned tensor remains on that GPU until explicitly transferred. Repeated
awaits observe the same immutable result.

Named and imported helpers, aliases, bindings, nested calls and checked shape
forwarding work within this scope. Products can share inputs and intermediate
results: `square = matmul(hidden, hidden)` evaluates `hidden` once. Every product
must contribute to the returned matrix. An unused additional product remains a
diagnostic rather than disappearing during admission.

## Execution and ownership

The compiler records a topologically ordered graph whose operands identify
original tensor parameters or earlier products. One scheduled tensor task owns
that graph, copies its input handles and reuses one compiled matrix kernel for
all products. Each product uses the existing strict matrix implementation and
eligible tiled alternatives; expressions are not reassociated or fused across
products. Intermediate results are rounded and stored as float32 matrices.

All input residence and shape obligations, including later products, are
validated before the first dispatch. The runtime tracks each operand's remaining
uses and releases its retained handle after the last consumer acknowledges.
Intermediates stay on the device. The final tensor can outlive the task and its
group through retained ownership.

Graph workspace is charged to the argument budget. Tensor storage, parameter
buffers and command admission retain their existing budgets. Cancellation,
deadlines and device failures settle through the same acknowledged task path;
no partially completed graph is published. Empty products advance iteratively,
without a GPU command or recursive runtime calls.

## Current boundary

This addition covers rank-two float32 product graphs.
[A final reduction and scalar arithmetic](native-gpu-graph-reduction.md) can now
consume the last matrix under one scalar task. [Tensor arithmetic](native-gpu-graph-arithmetic.md) can now appear before, between
and after products. [Multiple reductions](native-gpu-multiple-reductions.md) can now feed a final
scalar expression. Branches, reduction-to-tensor feedback and device-side task
coordination remain future work. Existing arithmetic around a single matrix
product and its optional reduction continues to use the separately qualified
paths. Additional numeric types, tensor ranks and custom kernels have their own
scope.

The compiler bounds proof expansion by work and helper depth; the runtime bounds
graph representation and workspace by checked sizes and configured budgets.
There is no source-level fixed product-count limit.

See the [source example](../../native/metal-source/matrix-graph.k),
[runtime tests](../../native/metal-source/MatrixGraphTests.cpp) and
[qualification record](../../native/metal-proof/evidence/gpu-matrix-graphs-2026-10-21.json).
