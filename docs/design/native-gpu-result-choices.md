# Choices over computed GPU matrix results

A function may choose how to use a matrix that it has already computed:

```kalandra
fn project(a: Tensor[float32, 2, 3], b: Tensor[float32, 3, 2], bias: Tensor[float32, 2, 2], factor: float32) -> Tensor[float32, 2, 2]:
  product = matmul(a, b)
  if factor > 0:
    product * factor
  else:
    bias
```

The product executes before the choice, following source evaluation order. One
resident elementwise result stage reads the product and any other contributing
tensors, evaluates the pure arms and selects their result bits. The example
therefore still computes the product when it chooses `bias`; it does not conditionally
skip matrix multiplication. There is no implicit host download.

Both arms may use the computed product, or one may use another proven equal-shaped
input. Nested choices, transitive helpers, result arithmetic and mapped matrix
operands compose with the same path. A subsequent `reduceSum` and final scalar
arithmetic also stay resident until the scalar task result is read back.

## Shape and purity boundaries

All contributing tensors in a choice must have equal complete shapes, established
by fixed dimensions or existing unconditional checked contracts. A dynamic
`product + bias` evaluated before the choice can establish that equality; moving
it into a speculative arm cannot introduce a new conditional check. Unknown
shapes without proof remain rejected. A shape mismatch on an unselected input
still fails the unconditional contract before GPU submission.

The compiler continues to reject matrix multiplication, reductions, tensor
construction, dispatch, fallible operations and shape checks inside branch arms.
Both pure arms may execute before selection. This step extends the existing
resident result map; it does not add general conditional primitive scheduling.

## Representation and qualification

Primitive forwarding preserves the identity of the single completed matrix and
marks the choice as result arithmetic. The existing result-map shader, projected
input table and runtime own all buffers through completion. Multi-read conditional
maps retain Tier 2 bindings even if optimization erases a read. Single-read maps
retain their existing interface. No runtime API, numerical kernel or memory
ownership rule changes.

The [fixture](../../native/metal-source/result-choices.k) and
[source/CLI checks](../../native/metal-source/VerifyResultChoices.cmake) cover
true/false and nested selections, constant predicates, identical arms, helpers,
mapped operands, a terminal sum with scalar scaling, dynamic shape proofs,
equal-count/different-shape failures, repeated observation and relocation. Pure
arm violations and missing shape proofs fail transactionally. Programs run with
one, two and four workers in debug, optimized, address/undefined-behavior-sanitized
and thread-sanitized modes. The installed Release compiler also builds and
relocates the example.

See the [qualification record](../../native/metal-proof/evidence/gpu-result-choices-2026-10-21.json).
These checks establish correctness for this subset, not a speed improvement.
