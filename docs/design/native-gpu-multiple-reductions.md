# Multiple GPU reductions

A float32 GPU helper can combine several reductions without reading intermediate
scalars back to the CPU:

```kalandra
fn difference(a: Tensor[float32, ?], b: Tensor[float32, ?],
              scale: float32) -> float32:
  left = reduceSum(a)
  right = reduceSum(b)
  (left - right) / scale
```

After explicit transfers, use `try await spawn difference(a, b, scale) on gpu`.
The inputs may have different lengths. Rank-two inputs and intermediate matrices
also work:

```kalandra
fn compare(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?],
           projection: Tensor[float32, ?, ?]) -> float32:
  hidden = matmul(input, weights)
  projected = matmul(hidden, projection)
  reduceSum(hidden) - reduceSum(projected)
```

Named/imported helpers, tensor arithmetic before reductions, scalar literals and
parameters, unary signs and strict scalar `+`, `-`, `*`, `/` compose with these
expressions. Reusing a reduced binding, such as `left + left`, reuses its scalar.
Separate reduction calls remain separate graph operations.

## Execution and ownership

One scalar task owns the tensor graph, each reduction and the final scalar
expression. The current runtime executes tensor stages first, then reductions
in their recorded order, then the scalar expression. This does not introduce
concurrent reduction scheduling or change source arithmetic order.

Every reduction uses the existing row-major balanced-adjacent tree and stores
its result in an immutable one-element device buffer. The final expression reads
those device scalars and returns only its four-byte result to the host. Tensor
operands are retained through their last tensor or reduction consumer. No
intermediate scalar is exposed to the CPU.

Each empty input contributes positive zero. Singleton inputs preserve their
stored scalar, including negative zero. Final arithmetic retains IEEE behavior,
including division by zero. Matrix products and tensor arithmetic keep their
existing strict float32 rules without reassociation or contraction.

All shapes, residence and kernel signatures are checked before the first
command. Workspace, retained scalar buffers, captures, device allocations and
command admission retain their checked budgets. Allocation failures,
cancellation, deadlines and device failures settle the whole task without
publishing a partial result.

The final expression uses direct bindings when they fit and the existing
Tier 2 argument-buffer path for larger signatures. Qualification includes forty
contributing reductions. There is no new small fixed reduction-count limit;
representation, compiler-work and runtime-resource bounds still apply. Ordinary
CPU stack admission remains in force for source helpers too.

## Current boundary

This supports rank-one/rank-two float32 inputs and straight-line tensor graphs
whose reductions feed a final scalar expression. Every reduction and tensor
stage must contribute to the returned result. Unused extra reductions, unsupported
branches remain diagnostics. [Reduced scalars feeding later tensor work](native-gpu-reduction-feedback.md) are now supported within scalar-returning graphs.

[Tensor results after feedback](native-gpu-tensor-feedback.md) now remain resident
too. Broader GPU numeric types/ranks and device control flow remain separate work. Scheduling and temporary-storage reuse are later
optimization opportunities.

See the [matrix example](../../native/metal-source/multiple-reductions.k),
[vector example](../../native/metal-source/capabilities/multiple-reductions.k),
[device tests](../../native/metal-source/MultipleReductionTests.cpp) and
[qualification record](../../native/metal-proof/evidence/gpu-multiple-reductions-2026-10-21.json).
