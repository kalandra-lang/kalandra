# Scalar choices after GPU reductions

A scalar decision can consume a reduction without downloading the intermediate
sum to the CPU:

```kalandra
fn score(values: Tensor[float32, ?], threshold: float32) -> float32:
  total = reduceSum(values)
  if total > threshold:
    total * 2
  else:
    threshold
```

The existing reduction runs first, then one resident scalar stage evaluates the
predicate and pure arms and selects the final float32 result. Only that final
scalar returns to the host. Helpers, nested choices, internal Boolean conditions
and ordinary scalar arithmetic may use the sum. A predicate may also choose
between constants: `if total > threshold: 7 else: 9` still depends on the sum.
Conversely, a condition can use an independent scalar parameter while an arm
returns the sum.

Mapped tensor inputs and rank-one/rank-two sums retain their existing paths.
For `total = reduceSum(matmul(a, b))`, the final choice uses the existing scalar
stage after the resident product and reduction. Matrix operand/result arithmetic
keeps its current support and shape contracts.

## Admission and execution

Primitive forwarding carries reduction provenance through comparisons and scalar
choices, including dependencies appearing only in the predicate. An ordinary
sum requests its result stage; a matrix-product sum requests its final scalar
stage independently of any matrix-result map. The existing shader emitter
implements strict comparisons and bitwise float32 selection. No numerical kernel,
runtime API, reduction order, capture budget or ownership rule changes.

Both arms must be pure scalar calculations. They may be evaluated before
selection. A reduction, matrix product, dispatch or fallible operation inside
an arm remains unsupported. This is a choice after completed numerical stages,
not conditional scheduling of those stages. Scalar task results remain float32;
internal Boolean predicates do not introduce Boolean GPU task results or captures.
Multiple reductions continue through their separately qualified graph path.

## Qualification

The [fixture](../../native/metal-source/reduction-choices.k) and
[source/CLI checks](../../native/metal-source/VerifyReductionChoices.cmake)
cover true/false and nested choices, constant predicates, dependencies only in
predicates or arms, transitive helpers, mapped inputs, empty sums, generated NaN
and infinity, matrix-product sums with and without operand maps, cached task
observation and relocation. Branch-local primitives and Boolean GPU task results
must reject transactionally.

Programs run in debug, optimized, address/undefined-behavior-sanitized and
thread-sanitized modes with one, two and four workers. The installed Release
compiler builds the example, and its relocated output runs after source/compiler
resources are removed. See the [qualification record](../../native/metal-proof/evidence/gpu-reduction-choices-2026-10-21.json).
These are correctness checks, not a performance result.
