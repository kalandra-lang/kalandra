# Choices in GPU matrix operands

A matrix operand can select between several immutable tensors before multiplication:

```kalandra
fn project(a: Tensor[float32, 2, 3], b: Tensor[float32, 2, 3], weights: Tensor[float32, 3, 2], factor: float32) -> Tensor[float32, 2, 2]:
  selected = if factor > 0:
    a
  else:
    b
  matmul(selected, weights)
```

Both operands may use independent choices and pure elementwise arithmetic,
including nested choices and transitive helpers. The left and right operand
maps read separate projections of the original immutable argument pack. Their
shapes need not match each other: rectangular multiplication still requires
only the normal inner-dimension agreement. The map results stay resident until
multiplication completes. Optional result arithmetic, a terminal `reduceSum`,
and final scalar arithmetic retain their existing execution paths.
[Choices over the computed matrix result](native-gpu-result-choices.md) now also
use the resident result-map path.

Every source within a choice must have the same complete shape. Fixed dimensions
or existing unconditional checked equality contracts establish that proof.
Independent unknown dimensions remain rejected, as do branch-local reductions,
matrix products, shape checks, dispatch and fallible operations. Both pure arms
may be evaluated before selecting the result; this does not introduce conditional
GPU dispatch. Shape failures remain task failures before GPU work is submitted.

## Bindings and ownership

Multi-input conditional operand maps use retained Tier 2 argument-buffer bindings,
even when their input count fits direct Metal slots. This keeps resource indices
stable when constant predicates or identical arms eliminate shader reads during
optimization. Shader emission, host loading and manifest metadata use the same
rule, including multi-input result maps. Single-input maps retain their previous
layout. Operand counts follow existing capture and device budgets; this adds no
fixed arity ceiling.

The existing projected operand-map runtime owns captures and intermediate buffers.
No new runtime API or payload copying is needed. Repeated task observation retains
the same immutable result. Existing cancellation, deadlines and failure cleanup
remain shared with the matrix pipeline.

## Qualification

The [fixture](../../native/metal-source/operand-choices.k) uses three distinct
left inputs and two distinct right inputs with rectangular shapes. The
[source and CLI checks](../../native/metal-source/VerifyOperandChoices.cmake)
cover nested true/middle/false selections, constant predicates, identical arms,
left-only and right-only maps, result scaling, a separately projected bias, terminal reduction/scalar scaling,
checked dynamic shapes, equal-count unequal-shape failures, repeated observation
and relocated programs. Unproven shapes and branch-local numerical primitives
must fail transactionally without replacing existing artifacts.

Programs are checked in debug, optimized, address/undefined-behavior-sanitized and
thread-sanitized modes, with one, two and four workers. Installed-package checks
also compile and relocate the fixture. This is correctness qualification, not a
performance result. See the [evidence record](../../native/metal-proof/evidence/gpu-operand-choices-2026-10-21.json).
