# Tensor results after resident reduction feedback

A GPU function can now return the tensor it computes from a reduced scalar:

```kalandra
fn normalize(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  values / reduceSum(values)
```

The caller explicitly transfers the input and selects the GPU:

```kalandra
input = try await transfer(values, gpu)
output = try await spawn normalize(input) on gpu
```

`output` is an immutable GPU tensor. It can be passed to another function spawned
on the same GPU, observed again through the original task, or explicitly
transferred to `devices.cpu()`. There is no automatic download. This arithmetic
example follows float32 division rules; it does not special-case a zero sum.

## Implementation

This extends the [resident reduction feedback graph](native-gpu-reduction-feedback.md)
with a tensor result mode. One or more reductions may feed tensor/scalar
arithmetic and matrix products through checked helper functions. Single-reduction
normalization works for vectors and matrices. The final tensor stage is the
result; every reduction and numerical stage must contribute to it.

The compiler records the result mode and preserves the result's rank and shape
when generating task captures and later transfers. A separate buffer-result task
entry avoids confusing tensor ownership with a scalar task's four-byte result.
There is no final scalar join shader, scalar staging allocation or result
readback command. Existing resident reduction buffers and compact/Tier 2 scalar
broadcast bindings are reused.

Preflight validates roots, broadcast ordering, complete tensor shapes, kernel
interfaces, device residency and owned capture budgets before dispatch. It also
retains the final tensor as a result consumer. After the final command
acknowledges, the task releases the graph workspace and owns the returned buffer.
Empty maps follow the same iterative completion path without a compute command.
Cancellation, deadlines, allocation failures and failed GPU completions preserve
the existing cleanup rules.

The accepted balanced-adjacent reduction tree, strict float32 operations and
separately rounded matrix products are unchanged. No stage fusion, reassociation,
CPU arithmetic fallback or new performance claim is introduced.

## Qualification and remaining scope

The [fixture](../../native/metal-source/tensor-feedback.k),
[device tests](../../native/metal-source/TensorFeedbackTests.cpp),
[source/CLI checks](../../native/metal-source/VerifyTensorFeedback.cmake) and
[qualification record](../../native/metal-proof/evidence/gpu-tensor-feedback-2026-10-21.json)
cover vectors, matrices, repeated feedback, matrix products, large broadcasts,
GPU forwarding, cached observations, failures and relocated installed programs.

The supported GPU numerical subset remains straight-line rank-one/rank-two
float32 computation. [Pure scalar choices and comparisons](native-gpu-scalar-control.md)
are now supported within those computations. Wider numeric types, broader ranks
and tensor-stage control flow remain N2 work. Scalar last-use reclamation, buffer reuse, scheduling and fusion
remain optimization work.
