# GPU reduction and matrix forwarding helpers

Reduction and matrix entry functions can use immutable local bindings and call ordinary forwarding helpers. Those helpers can come from other modules:

```kalandra
fn product(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  left = input
  right = weights
  result = matmul(left, right)
  result

fn project(input: Tensor[float32, 2, 3], weights: Tensor[float32, 3, 2]) -> Tensor[float32, 2, 2]:
  product(input, weights)

fn total(values: Tensor[float32, ?, ?]) -> float32:
  result = reduceSum(values)
  result
```

After explicitly transferring the inputs, `spawn project(input, weights) on devices.gpu()` performs one matrix calculation. `spawn total(result) on devices.gpu()` performs one reduction. Forwarding calls do not introduce extra GPU tasks or download data to run a helper on CPU.

## Operand identity and shapes

The compiler follows each operand through bindings and calls rather than assuming it is the entry's first or second argument. A wrapper may reverse its argument order, pass the same tensor twice, or forward its result through an identity function. For example, a function taking `(weights, input)` can call `project(input, weights)` and still produce `input × weights` with the correct output shape.

Fixed and mixed shape annotations retain the [existing host-side preflight](native-gpu-fixed-shapes.md). Parameter, helper and result constraints are checked before admitting the numerical kernel. Imports preserve private helper lookup and source contracts.

## Current boundary

Single-primitive entries resolve to one supported `reduceSum` or `matmul`.
[Matrix-to-reduction composition](native-gpu-matmul-reduction.md) additionally admits
one matrix product consumed by one reduction, including optional matrix input maps. A direct reduction forwards a rank-one/rank-two float32 tensor. Matrix entries resolve their rank-two float32 operands by identity, including repeated inputs and additional tensor/scalar parameters. Forwarding helpers can pass those tensors or the completed scalar/matrix result through bindings and calls, including checked shape nodes.

The compiler inspects every statement, including unused bindings. Extra unused reductions or matrix operations are not discarded to make an entry fit the supported graph. [Elementwise reduction inputs](native-gpu-pre-reduction.md), such as `reduceSum(values * 2)`, now use two resident GPU stages under one scheduled task and may mix tensor/scalar parameters. [Scalar arithmetic after a reduction](native-gpu-post-reduction.md), such as `reduceSum(values) + 1`, now executes once on the GPU before scalar readback. [Elementwise matrix results](native-gpu-post-matmul.md) now apply tensor/scalar arithmetic after one multiplication, with projected direct/Tier 2 bindings. [Matrix operand arithmetic](native-gpu-pre-matmul.md) now supports independent resident maps of either/both inputs under the same task, with optional post arithmetic. [One matrix followed by one reduction](native-gpu-matmul-reduction.md) can now share a scalar task. [Matrix-result arithmetic before reduction](native-gpu-matmul-adjust-reduction.md) now also stays within that task. [Scalar arithmetic after the pair](native-gpu-matmul-scalar-reduction.md) also stays within that task. [Matrix product graphs](native-gpu-matrix-graphs.md) now additionally support several products, including shared intermediates, under one tensor task. [One final reduction and scalar arithmetic](native-gpu-graph-reduction.md) can now follow those graphs. [Tensor arithmetic within those graphs](native-gpu-graph-arithmetic.md) is now supported. [Multiple reductions](native-gpu-multiple-reductions.md) now feed one final scalar expression. [Reduction feedback](native-gpu-reduction-feedback.md) now feeds tensor maps and later products inside scalar-returning graphs. [Tensor results after feedback](native-gpu-tensor-feedback.md) now remain resident too, including single-reduction normalization.

Recursive forwarding, arbitrary control flow and unsupported operations remain diagnostic. Analysis has explicit work and call-depth limits; reaching a limit rejects the program before publishing artifacts. This does not add callable-value dispatch, new element types, implicit transfers, device-side task coordination or a numerical-policy change.

## Lowering and qualification

A bounded symbolic pass tracks input identities and the supported primitive or
matrix/reduction pair through the checked CPU helper graph. It preserves source argument evaluation order in the supervisor and selects the corresponding retained buffers for the existing runtime launch. The shared shape preconditions, strict arithmetic lowering, shader interfaces and runtime ownership are reused.

The [three-file fixture](../../native/metal-source/forwarding/main.k) exercises aliases, imported/private helpers, reordered matrix parameters, repeated operands with a different unused input, scalar result forwarding and cached task observations. Qualification also covers empty and negative-zero singleton reductions, dynamic shape failures, relocated bundles, unsupported extra computations and bounded expansion. Wrapped kernels are compared byte-for-byte with their direct equivalents.

See the [qualification record](../../native/metal-proof/evidence/gpu-forwarding-2026-10-20.json) for modes, counts and source/artifact hashes.
