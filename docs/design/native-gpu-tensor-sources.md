# Choosing between tensor sources on Apple GPU

Pure elementwise choices can now read two different tensors when their complete
shapes are statically equal:

```kalandra
fn choose(a: Tensor[float32, 3], b: Tensor[float32, 3], factor: float32) -> Tensor[float32, 3]:
  if factor > 0:
    a * factor
  else:
    b / factor
```

Transfer both inputs explicitly, then spawn `choose` on the GPU. One elementwise
map reads both sources and writes one output tensor. Both arms may be evaluated
before selecting the result bits. No temporary tensor is allocated for either
arm, and no host readback is needed to choose an arm. The output remains resident.

A predicate can also consume a resident reduction:

```kalandra
fn normalize(a: Tensor[float32, 3], b: Tensor[float32, 3], threshold: float32) -> Tensor[float32, 3]:
  total = reduceSum(a)
  if total > threshold:
    a / total
  else:
    b * threshold
```

The compiler schedules the reduction and one map under the same task. A terminal
`reduceSum` can consume a two-source choice too, without returning the selected
tensor to the host first. Precomputed fixed-shape intermediates, nested choices,
rank-two tensors and empty fixed shapes have separate checks.

## Shape and execution boundary

Equal fixed dimensions are one accepted proof. [Checked dynamic-shape contracts](native-gpu-dynamic-choices.md)
now provide another, including transitive requirements from unconditional tensor
arithmetic and helpers. Equal flat element counts are insufficient: `[2, 3]` and
`[3, 2]` are different shapes. Independent `Tensor[float32, ?]` parameters without
an equality proof remain outside this subset, even if a particular caller supplies
equally sized tensors.
Existing fixed parameter contracts are checked before kernel admission, including
parameters an optimizer later removes. This adds no check to an untaken arm.

[Same-source choices](native-gpu-tensor-control.md) retain dynamic-shape support.
All branch operations must remain pure and non-failing. Branch-local reductions,
matrix products, dispatch, allocation and shape checks are still rejected.
This is not general graph branching, and does not support selecting differently
shaped tensors or scheduling only one arm's numerical primitives.

[Multi-source maps](native-gpu-nary-choices.md) now carry explicit owned operand
lists, removing the fixed two-source ceiling. Shape proofs and compiler/device
resource budgets remain in force.
[Single-matmul operand maps](native-gpu-operand-choices.md) now also support
different-source choices with independently projected bindings. Do not infer support for every
matrix/primitive composition from these examples.

## Bindings, ownership and evidence

Graph analysis carries a set of tensor sources and reduced scalar dependencies
for each lane expression, then creates one map with its source operands. Shape
proof is checked before publishing artifacts. Conditional entries with several
live tensor reads use the existing Tier 2 argument-buffer interface, preserving
declared binding indices when constant predicates remove an arm during shader
optimization. Single-source ordinary entries retain their previous layout.

Both captures remain owned through completion; graph last-use reclamation,
cancellation, deadlines, allocation failure and device failure use the existing
runtime paths. This step changes compiler admission and lowering, not runtime APIs.

See the [source fixture](../../native/metal-source/tensor-sources.k),
[device checks](../../native/metal-source/TensorSourceTests.cpp),
[source/CLI checks](../../native/metal-source/VerifyTensorSources.cmake) and
[qualification record](../../native/metal-proof/evidence/gpu-tensor-sources-2026-10-21.json).
These qualify correctness; no speed claim is made.
