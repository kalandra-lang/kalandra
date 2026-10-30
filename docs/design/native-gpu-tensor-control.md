# Elementwise tensor choices on GPU

A GPU helper can choose between two elementwise calculations over the same tensor:

```kalandra
fn normalize(values: Tensor[float32, ?], threshold: float32) -> Tensor[float32, ?]:
  total = reduceSum(values)
  if total > threshold:
    values / total
  else:
    values * threshold
```

After an explicit transfer and GPU spawn, the sum stays resident. One map reads
that sum and the input tensor, selects each result and writes one output tensor.
There is no host readback to decide which arm to use, and no intermediate tensor
allocation for either arm. The returned tensor stays on the device until an
explicit transfer or a later GPU call consumes it.

## Accepted scope

Both arms must use the same logical tensor source, with float32 elements and rank
one or two. Unary signs, elementwise `+`, `-`, `*`, `/`, scalar lifting, nested
choices, local bindings and dynamic-shape pure helpers are supported. A source
can be an input tensor or an already-computed resident intermediate. A scalar
predicate may use resident reductions computed before the choice. Comparisons
retain the [strict scalar rules](native-gpu-scalar-control.md).

This is a fused elementwise choice. Both pure arms may be evaluated for each
lane before selecting the result bits. IEEE division by zero remains non-failing
arithmetic. Untaken arms cannot allocate tensors, perform reductions or matrix
products, dispatch tasks, fail, or execute conditional shape checks. Helpers
called inside arms cannot impose fixed tensor dimensions or shape preconditions.
This restriction prevents checks from an untaken arm being executed speculatively.

[Choices between two different sources](native-gpu-tensor-sources.md) now have a
separate equal-fixed-shape subset. [Proven dynamic-shape choices](native-gpu-dynamic-choices.md) now reuse checked
unconditional extent contracts. Unknown independent shapes still require separate work. Compute a common intermediate before the choice, or use separate
placed calls where appropriate. Reductions and matrix products can consume an
accepted choice: the qualified cases include a terminal `reduceSum`, an operand
map before one `matmul`, and a reduction-driven choice followed by another sum.
Existing numerical graph limits still apply; this does not admit every possible
combination of the CPU language on GPU.

## Lowering and qualification

Graph analysis tracks the common source and contributing reduced scalars for each
lane expression. Unused branch-local calculations do not create stages or live
reduction bindings. An outer tensor choice contributes one map; nested arithmetic
and choices stay inside it. Conditional graphs retain the existing Tier 2 resource
interface so optimization of constant predicates or identical arms cannot change
the runtime's binding indices. Ordinary elementwise entries retain their existing
interface.

The [source fixture](../../native/metal-source/tensor-control.k),
[device checks](../../native/metal-source/TensorControlTests.cpp),
[source and CLI checks](../../native/metal-source/VerifyTensorControl.cmake) and
[qualification record](../../native/metal-proof/evidence/gpu-tensor-control-2026-10-21.json)
cover numerical edge cases, nested/helper forms, identical arms, empty tensors,
rank-two tensors, composition, ownership and failure cleanup. This is correctness
qualification, not a speed claim.

Unproven different-source shapes, conditional primitive stages, wider GPU
numeric types and broader ranks remain follow-up work. General graph branching
needs its own shape, scheduling and lifetime rules.
