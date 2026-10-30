# GPU scalar arithmetic after reduction

A GPU numerical helper can now compute with the result of one `reduceSum`:

```kalandra
fn adjustedTotal(values: Tensor[float32, ?], bias: float32) -> float32:
  reduceSum(values) + bias

fn normalizedTotal(values: Tensor[float32, ?], shift: float32, factor: float32) -> float32:
  total = reduceSum((values + shift) * factor)
  -(total + shift) / factor
```

After explicitly transferring the tensor to a GPU, either helper can be spawned
there. The result remains one scalar task. Rank-one and rank-two float32 inputs,
scalar parameters, immutable bindings, transitive named helpers, imports and
re-exports share the existing numerical subset. Scalar `+`, `-`, `*`, `/` and
unary signs use the same strict float32 lowering as elementwise expressions.

One reduced binding can be used repeatedly, for example `total * total + bias`.
This performs one reduction. Two independent `reduceSum` expressions, even with
the same input, still exceed the current one-primitive entry boundary.

## GPU execution

The compiler proves exactly one reduction through the checked helper graph. It
separates the reduction input from the scalar result expression and emits a
supplemental `kernelN-post` shader, recorded as `epilogue_stage` in the manifest.
The scalar stage reads one already-reduced device value plus the original
float32 scalar parameters. It does not read the original tensor lanes.

The runtime retains the fixed balanced-adjacent, row-major reduction tree. After
its final level, another GPU dispatch evaluates the scalar expression once.
Both the tree and this final dispatch are encoded into the same command buffer;
only the finished scalar is copied back to the host. A preceding elementwise
input stage, when needed, keeps its intermediate tensor resident and hands off
to that command under the same scheduled task. `spawn` does not wait for either
command on the caller's behalf.

Every source arithmetic operation rounds separately. No reassociation, implicit
fused multiply-add, per-lane application of final scalar arithmetic or CPU
fallback is introduced. Empty reductions supply positive zero to the final
shader, so `reduceSum(empty) + bias` returns `bias`. Empty device storage has no
Metal allocation; a separately owned four-byte identity buffer supplies this
case. Singleton inputs retain their bits before final arithmetic.

All scratch outputs, scalar/count parameter blocks and the empty identity seed
are budgeted and prepared before opening encoders. An optional input map and the
reduction/final command acknowledge completion independently; cancellation
prevents the next command from starting and keeps pending storage alive until
acknowledgement. Failure publishes no intermediate scalar. Existing argument,
memory and in-flight budgets apply; one in-flight slot suffices.

## Boundaries and qualification

Full argument residence and shape contracts are checked before launch, including
unused parameter obligations. Unsupported operations in any statement remain
diagnostic. [Elementwise matrix results](native-gpu-post-matmul.md) are also
supported, including [matrix operand arithmetic](native-gpu-pre-matmul.md).
Multiple independent reductions, data-dependent
GPU branches, recursive helpers, other GPU numeric types and higher ranks remain
separate work. This is not a general GPU closure or foreign-shader interface.

The shared [`PreReductionTests.cpp`](../../native/metal-source/PreReductionTests.cpp)
harness runs the new `post-reduction` fixture in four host modes. It compares
288 vector/matrix cases with separate GPU stages and an independent LLVM APFloat
reference. Inputs include empty/singleton/odd lengths, signed zeros, subnormals,
infinities, NaNs and arbitrary encodings; scalar cases include IEEE division by
zero. It sweeps preparation failures for ordinary and empty inputs, holds each
command pending for cancellation/deadline/device-failure checks, verifies cached
observations, scheduler destruction and second-command budget failure, and
requires resource accounting to return to baseline.

The shared [source/CLI checks](../../native/metal-source/VerifyPreReduction.cmake)
also verify imported/re-exported mixed arguments, comparison with a CPU scalar
helper after separate GPU calls, a direct reduction without an input map, empty
results, repeated use of one reduced binding, shape faults and relocated bundles.
The installed Release bundle qualifies a moved executable with both supplemental
shaders after removing the compiler. See the
[qualification record](../../native/metal-proof/evidence/gpu-post-reduction-2026-10-21.json)
for selected checks and hardware. No performance improvement is claimed.
