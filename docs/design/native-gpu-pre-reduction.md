# Elementwise expressions before GPU reduction

A numerical GPU helper can now return one strict `reduceSum` whose input is a
straight-line float32 elementwise expression, including named/imported helpers:

```kalandra
fn total(values: Tensor[float32, ?], shift: float32) -> float32:
  reduceSum(values + shift)

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    values: Tensor[float32, 3] = tensor([1, 2, 3])
    resident = try await transfer(values, gpu)
    try await spawn total(resident, 1) on gpu
```

This returns `9`. Rank-two tensors work too, with row-major reduction order.
Parameters may mix tensors and float32 scalars. The elementwise subset retains
strict `+`, `-`, `*`, `/`, unary signs, aliases, shape checks and transitive named
calls. Imports and re-exports preserve declaring-file scope. All tensor arguments
must already reside on the selected GPU. Complete shape obligations—including
those of unused parameters—are checked before launch.

## Two GPU stages, one task

This is source-level composition, not a fused reduction algorithm. The compiler
publishes an elementwise shader and a supplemental `kernelN-reduce` shader, with
an explicit `reduction_stage` association in the manifest. Existing direct and
Tier 2 argument-buffer layouts apply to the elementwise stage.

The scheduled task owns the argument pack, both kernel handles and the temporary
device buffer. `spawn` returns its handle without waiting for either stage. On
acknowledgement of the elementwise command, the same task resumes and submits the
existing balanced-adjacent reduction. Only the final scalar is read back. The
intermediate tensor stays on the GPU, and the original inputs stay immutable.

Every elementwise operation rounds independently as float32. The mapped values
are then reduced using the established adjacent-pair tree; odd tails pass through
unchanged at each level. Empty inputs yield positive zero. Singleton inputs are
mapped once before returning their value. No map expression is reapplied to
scratch buffers, and no reassociation or fused multiply-add is introduced.

Preparation and command failures publish no scalar. Cancellation/deadlines use
the existing external-completion leases: storage remains alive until the active
command acknowledges completion, and cancellation prevents the next stage from
starting. Each stage uses the existing memory/in-flight budgets. This introduces
no unbounded task tree or CPU numerical fallback.

## Boundaries

The bounded primitive analysis still requires exactly one reduction in an entry
and visits every statement. [Scalar arithmetic after reduction](native-gpu-post-reduction.md) now adds an
optional final GPU stage. [Matrix operand arithmetic](native-gpu-pre-matmul.md)
has its own resident input stages. Multiple independent reductions,
recursive calls and data-dependent GPU branches remain unsupported. Supported pure arithmetic in unused bindings may be removed
just as in ordinary elementwise shaders, while its shape obligations remain.
This does not add other GPU numeric types, tensor ranks, managed host values or
custom shaders.

## Qualification

[`PreReductionTests.cpp`](../../native/metal-source/PreReductionTests.cpp) compares
216 vector/matrix cases with separate GPU dispatches and an LLVM APFloat
reference. Inputs cover empty/singleton/odd lengths, signed zeros, subnormals,
infinities, NaNs and arbitrary bit encodings. It checks repeated scalar reads,
every Metal preparation allocation checkpoint, and cancellation/deadline/device
failure while either stage is held pending. Scheduler destruction waits for pending acknowledgement. A single in-flight slot
suffices for both stages; a memory-budget failure preparing the second stage
reclaims the mapped storage. Invalid/missing second-stage kernels publish no
scalar. Resource accounting returns to its baseline after group release.

[`VerifyPreReduction.cmake`](../../native/metal-source/VerifyPreReduction.cmake)
checks imported/re-exported helpers with mixed arguments, exact graph/separate
results, cached awaits, relocation and equal-element-count shape mismatches.
Source-only checks admit post-reduction composition and preserve existing output
on rejected multiple-reduction and branch programs. Both compiler modes run those checks;
GPU/runtime checks run Debug, optimized, ASan/UBSan and TSan hosts. The existing
reduction CLI additionally covers rank-two and empty/singleton paths.

The [qualification record](../../native/metal-proof/evidence/gpu-pre-reduction-2026-10-21.json)
records hardware, toolchain and selected checks. This is correctness/lifetime
qualification, not a performance improvement claim.
