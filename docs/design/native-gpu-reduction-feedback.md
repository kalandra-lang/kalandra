# Resident reduction feedback

A GPU function can now use a reduction result in later tensor arithmetic, then
return a final scalar. [Tensor results](native-gpu-tensor-feedback.md) are also supported now. The intermediate scalar stays on the device:

```kalandra
fn variance(values: Tensor[float32, ?], count: float32) -> float32:
  total = reduceSum(values)
  centered = values - total / count
  squares = centered * centered
  reduceSum(squares) / count
```

Spawn this function on `devices.gpu()` with an explicitly transferred input.
This is ordinary function and tensor syntax; no separate kernel definition is
required. The caller supplies `count`; division follows float32 rules, including
zero denominators.

## Execution and ownership

The bounded straight-line graph proof records which reduced scalars each tensor
map consumes. Source-ordered reductions run as soon as their tensor input is
available, interleaved with tensor stages. One managed scalar task owns the graph;
there is no child task per stage and no CPU arithmetic fallback.

Each reduction writes an immutable one-element GPU buffer. Tensor maps load its
value from index zero for every output element, while ordinary tensor inputs are
indexed by the output lane. Scalar expressions such as `total / count` execute
on the GPU as part of that map. Tensor shapes must still agree in every axis;
a device scalar is explicitly distinguished from a one-element tensor operand.

Bindings are compact and ordered: contributing tensors first, then the map's
resident scalars. Large maps use the existing Tier 2 argument-buffer path. The
final scalar shader projects away reductions used only by earlier tensor work;
only its four-byte result is read back.

Preflight validates graph references, broadcast ordering, kernel interfaces,
shapes, residency and owned capture budgets before dispatch. Tensor storage is
released after its last consumer; reduced scalar buffers currently stay owned
until graph completion. Allocation failures, cancellation, deadlines and failed
GPU completions unwind through the existing acknowledged task lifecycle.

Arithmetic preserves the accepted balanced-adjacent, row-major reduction tree,
separately rounded products and strict float32 operations. Stages are not fused
or reassociated. Empty reductions produce positive zero; empty maps skip dispatch
without recursive stage traversal.

## Qualified scope and remaining work

This increment supports rank-one/rank-two float32 tensor inputs, scalar
arithmetic, checked transitive helpers, repeated reduction feedback and matrix
products before or after feedback. The scalar-result mode requires at least two
contributing reductions and every numerical stage must contribute. [Tensor-result mode](native-gpu-tensor-feedback.md)
also admits a single reduction, such as `values / reduceSum(values)`. [Pure scalar choices and comparisons](native-gpu-scalar-control.md) can now use
those reduced values too. Tensor-producing branches, wider numeric types and
broader ranks remain outside this subset.

The [fixture](../../native/metal-source/reduction-feedback.k),
[device tests](../../native/metal-source/ReductionFeedbackTests.cpp),
[source/CLI checks](../../native/metal-source/VerifyReductionFeedback.cmake) and
[qualification record](../../native/metal-proof/evidence/gpu-reduction-feedback-2026-10-21.json)
cover the implementation. This is correctness and lifecycle qualification, not a
new performance claim. Scheduling, scalar last-use reclamation, storage reuse and
fusion remain optimization work.
