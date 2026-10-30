# Scalar choices inside GPU helpers

GPU helpers now support float32 comparisons and pure scalar `if` expressions,
including choices made from resident reduction results:

```kalandra
fn normalize(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  total = reduceSum(values)
  denominator: float32 = if total == 0:
    1
  else:
    total
  values / denominator
```

Spawn this function on the GPU with an explicitly transferred input. The sum,
choice of denominator and tensor division stay on the device. This example
chooses a denominator of one for a zero sum; that is application behavior written
in the function, not an implicit change to division semantics.

## Accepted scope

`==`, `!=`, `<`, `<=`, `>` and `>=` compare scalar float32 values. Internal Boolean
values, `and`, `or` and `not` can combine those conditions inside float32
helper bodies. Function parameters/results and public scalar captures retain
the existing float32 boundary. This does not introduce Boolean helper signatures,
Boolean tensor storage or Boolean parameters at the host/GPU boundary.

An `if` expression may return float32 or an internal Boolean, with nested choices
and local bindings. Every operation in either arm must be a pure, non-failing
scalar calculation. Tensor operations, shape checks, reductions, matrix products,
allocation, task operations and other effects are not allowed inside arms. Such
work can run before the choice and supply scalar values to it. Tensor-valued
branches have a separately qualified [same-source elementwise subset](native-gpu-tensor-control.md).

Pure scalar choices work in ordinary tensor helpers, tensor-result feedback
graphs and scalar joins over several reductions. A reduction used only by a
condition still contributes a resident scalar binding. Existing graph constraints
and the separate single-reduction scalar-entry boundary remain in force.

## Strict lowering

Comparisons use float32 bit encodings and integer operations, matching the CPU
backend's ordered comparison rules. Equal signed zeros compare equal; NaNs make
`!=` true and all other comparisons false. Subnormals remain distinct from zero.
No native floating comparison or conversion changes those rules on the GPU.

The compiler proves both arms pure before lowering the choice to a selection.
It may evaluate both scalar arms, then select the result bits. This preserves
signed zeros, subnormals and NaN payloads through selection and introduces no
branch-dependent allocation or dispatch. Strict arithmetic retains its existing
rounding and reduction order. Conditional graphs use the existing Tier 2
argument-buffer interface, including for small signatures. It retains declared
resources when optimization removes a predicate or an identical arm, avoiding
shader/runtime binding mismatches. Single-source ordinary tensor helpers keep their existing
binding layouts; [two-source choices](native-gpu-tensor-sources.md) use the retained-resource interface. The purity walk is cached and bounded; recursive helpers retain
their existing rejection.

See the [source fixture](../../native/metal-source/scalar-control.k),
[device checks](../../native/metal-source/ScalarControlTests.cpp),
[source/CLI checks](../../native/metal-source/VerifyScalarControl.cmake) and
[qualification record](../../native/metal-proof/evidence/gpu-scalar-control-2026-10-21.json).
This is correctness qualification, not a performance claim.

Wider GPU numeric types, Boolean capture/storage interfaces, tensor-stage control
flow and broader ranks remain follow-up work. Scalar choices do not imply that
the complete CPU control-flow language is available on the GPU.
