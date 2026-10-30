# GPU choices with checked dynamic shapes

Two-source elementwise choices can use dynamic dimensions when the function's
existing unconditional shape requirements prove that both sources match:

```kalandra
fn choose(a: Tensor[float32, ?], b: Tensor[float32, ?], factor: float32) -> Tensor[float32, ?]:
  combined = a + b
  if factor > 0:
    combined * factor
  else:
    b / factor
```

The addition already requires equal complete shapes. The compiler checks that
contract before admitting the GPU task and reuses it to prove that reading both
arms is safe. The choice stays fused into one elementwise calculation, with one
output tensor. This reuses the requirements of the calculation the function
performs; it introduces no shape-checking syntax or new runtime API.

A reduction can drive the predicate while its result stays on the device:

```kalandra
fn normalize(a: Tensor[float32, ?], b: Tensor[float32, ?], threshold: float32) -> Tensor[float32, ?]:
  combined = a + b
  total = reduceSum(combined)
  if total > threshold:
    combined / total
  else:
    b * threshold
```

This runs the input map, balanced sum and selected output map under one task.
Intermediates remain resident; explicit transfer still controls host readback.

## Proof rules

Each shader helper invocation that needs a proof builds a bounded
extent-equivalence table from its checked unconditional contracts. Constants and nonzero dynamic extent identities
are distinct keys. Transitive equalities and equalities to fixed dimensions are
supported. Every dimension is checked independently, so equal element counts do
not make `[1, 3]` and `[3, 1]` interchangeable.

After proving a choice safe, the compiler connects its fresh merged result
extents to the proven arm extents. Nested choices can reuse that fact. Tensor
arithmetic inside arms also requires proven operand shapes, including operations
whose result keeps the first operand's shape. This prevents an unproven second
input from being hidden behind a compatible-looking result type.

These facts are local to one helper invocation. Calling a helper with the same
buffer twice cannot establish a contract for a later call with two independent
buffers. Existing transitive contracts from unconditionally called numerical
helpers remain available. Shape requirements created only inside an arm are
excluded, and helper calls inside arms retain the existing purity restrictions.

The proof walk uses iterative path compression and union by rank, with a shader
analysis budget. No source declaration or public type system changes are needed.

## Runtime behavior and limits

Unknown dimensions are validated by the existing host continuation before kernel
admission. A mismatch reports the existing Shape task fault, including when a
shader optimizer removes arithmetic that originally imposed the contract.
Known mismatches keep their existing compile-time diagnostics. Shapes do not gain
new checks merely because a choice was added.

Independent `?` dimensions without an equality proof remain rejected. This step
does not select between differently shaped outputs, execute conditional numerical
primitives or admit branch-local shape checks. [Multi-source fused maps](native-gpu-nary-choices.md)
now use explicit operand lists beyond the earlier two-source representation. GPU numeric widths, ranks, strict arithmetic, binding layouts and ownership
rules retain their existing boundaries. [Different-source matrix operand choices](native-gpu-operand-choices.md) now
reuse these shape proofs in the separate single-matmul path.

See the [fixture](../../native/metal-source/dynamic-choices.k),
[device checks](../../native/metal-source/DynamicChoiceTests.cpp),
[source/CLI checks](../../native/metal-source/VerifyDynamicChoices.cmake),
[capability matrix](native-gpu-capabilities.md) and
[qualification record](../../native/metal-proof/evidence/gpu-dynamic-choices-2026-10-21.json).
This is correctness qualification, not a performance claim.
