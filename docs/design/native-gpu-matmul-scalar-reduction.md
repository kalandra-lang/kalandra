# GPU scalar arithmetic after matrix reduction

A float32 helper can now compute with its reduced matrix result before returning:

```kalandra
fn score(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?],
         bias: Tensor[float32, ?, ?], count: float32) -> float32:
  total = reduceSum(matmul(input, weights) + bias)
  total / count
```

Strict `+`, `-`, `*`, `/`, unary signs, literals and named/imported scalar helpers
are supported after the reduction. A reduced binding can be read repeatedly,
for example `total + total`, without another matrix product or reduction.
The existing optional matrix input maps and matrix-result adjustment compose
with this scalar expression. A plain `reduceSum(matmul(a, b)) / count` needs
neither input maps nor a matrix adjustment stage.

One scalar task owns the full resident sequence. Only the final four-byte scalar
is read back. This does not add scalar-only GPU entry functions, broader numeric
types or arbitrary multi-primitive graphs.

## Separate matrix and scalar expressions

The bounded proof still requires one matrix product consumed by one reduction.
It now tracks the scalar expression after that pair independently from the
matrix-result adjustment before reduction. The scalar SSA emitter treats the
completed reduced value as logical input zero and eliminates dead tensor work
from the scalar shader. Original scalar parameters retain their signature order.

The compiler emits `kernelN-scalar` and records its stage and parameter count in
the manifest. It is separate from `kernelN-post`, which remains the matrix
adjustment. Matrix, adjustment and final scalar argument packs are owned
independently; consuming the matrix pack cannot discard the final scalar values.
Existing signature-sized captures apply, including a qualified 40-scalar entry.
There is no new fixed language parameter-count limit.

The runtime uses the existing reduction epilogue encoder: reduction tree passes
and the final scalar dispatch share the last command buffer before readback.
There is no additional scheduled task or host calculation between them. Optional
input maps and matrix adjustment still use their existing acknowledged command
handoffs, so the full path has at most five commands and one in-flight slot is
sufficient. Matrix/reduction trees and every floating-point rounding boundary
remain unchanged; scalar operations round after the final tree result.

## Empty sums, IEEE arithmetic and ownership

An empty matrix result still has the positive-zero empty reduction seed. When a
scalar expression exists, the final GPU command applies it to that seed. For
example, adding an offset to an empty sum must return the offset. Division by
zero follows float32 IEEE behavior, including infinity and NaN; it is not an
execution fault. A zero contraction length with nonempty output continues to
apply matrix adjustment before reduction and then scalar arithmetic afterwards.

Full tensor shape/residence guards and kernel-interface validation remain in
place. The final shader must have one logical tensor input, its declared scalar
parameter count and the qualified direct binding layout. Memory/capture budgets,
fallible allocation preparation, cancellation/deadline handling and completion
acknowledgement rules are unchanged. A final-stage failure publishes no partial
scalar. Active resources remain owned until acknowledgement, and cached final
scalars can outlive original input owners.

## Qualification and remaining work

The shared [`MatmulReductionTests.cpp`](../../native/metal-source/MatmulReductionTests.cpp)
harness runs the full path in Debug, optimized, ASan/UBSan and TSan modes. Each
mode compares 192 plain/input-transformed cases with checked CPU execution,
separate GPU calls and LLVM APFloat. It includes empty/zero-inner shapes, odd
trees, signed zeros, subnormals, infinities, NaNs, arbitrary bit encodings and
zero scalar divisors. It sweeps 62 allocation checkpoints, checks cancellation,
deadlines and device failures in all five commands, pending scheduler destruction,
invalid scalar captures, memory budgets and retained scalar ownership.

[`VerifyMatmulScalarReduction.cmake`](../../native/metal-source/VerifyMatmulScalarReduction.cmake)
checks imports/re-exports, optional input/adjustment stages, repeated scalar
results, 40 scalar parameters, Tier 2 matrix adjustments with a separate
zero-parameter scalar expression, shape failures and relocation. The final
scalar shader matches the separately placed reduction epilogue byte-for-byte.
Programs run with 1/2/4 workers in all four modes. Unsupported extra primitives
and branches preserve existing outputs. The installed Release compiler builds
an application with all five supplemental shaders; it still runs after
relocation and removal of the compiler installation.

The [qualification record](../../native/metal-proof/evidence/gpu-matmul-scalar-reduction-2026-10-21.json)
records selected checks and hashes. Multiple independent matrix/reduction
primitives, data-dependent device control flow, recursive helpers, broader GPU
types/ranks and custom shader interfaces remain separate work. This milestone
makes no performance improvement claim.
