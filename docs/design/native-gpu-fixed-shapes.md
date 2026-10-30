# Fixed tensor shapes on Apple GPU

GPU numerical functions accept fixed extents, dynamic extents, or a mixture of both. They use the same tensor type syntax and shape rules as CPU functions:

```kalandra
fn scale(values: Tensor[float32, 3]) -> Tensor[float32, 3]:
  values * 2

fn project(input: Tensor[float32, ?, 3], weights: Tensor[float32, 3, ?]) -> Tensor[float32, ?, ?]:
  matmul(input, weights)
```

Call them with `spawn scale(input) on devices.gpu()` or `spawn project(input, weights) on devices.gpu()`, after explicitly transferring the inputs. Function imports and re-exports preserve these contracts. Shapes describe logical dimensions; a matching flat element count does not make `[2, 3]` and `[3, 2]` interchangeable.

## Checking and execution

A known mismatch is a compile-time diagnostic. When an input's size is unknown until execution, the generated host continuation checks its rank and required dimensions before loading or admitting the kernel. A mismatch reports the existing Shape task fault; normal `TaskError` handling and group settlement apply. Unused parameters must satisfy their declared shapes too, even when their buffers are omitted from the shader interface.

Shape requirements from transitive numerical helpers, explicitly typed local bindings and fixed result annotations also become entry preconditions. For example:

```kalandra
fn checked(values: Tensor[float32, ?]) -> Tensor[float32, 3]:
  three: Tensor[float32, 3] = values
  three * 2

fn calculate(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  checked(values) + 1
```

A GPU call to `calculate` must receive three elements. The host validates that requirement before dispatching; the shader does not silently erase the local binding's shape check. Fixed result annotations on supported matrix functions similarly constrain the corresponding input dimensions before execution.

This uses the existing straight-line GPU numerical subset and the shared frontend's shape-precondition analysis. [Reduction/matrix forwarding helpers](native-gpu-forwarding.md) now preserve aliases and calls around one operation. This does not add arbitrary shape-dependent control flow, tensor construction inside shaders, or general reduction/matrix expression composition. Unsupported bodies retain diagnostics. Existing float32, rank-one/rank-two, residency and numerical policies remain unchanged.

## Lowering and qualification

The Metal backend checks fixed parameter dimensions alongside inferred equality/constant requirements. Its lane lowering consumes checked shape nodes after the host has validated those preconditions. The matrix entry analysis can see the numerical operation beneath a result shape check and through forwarding helpers. No new numerical kernel or shader ABI is introduced: equivalent fixed/dynamic annotations produce identical raw GPU MLIR, strict-lowered MLIR and Metal shader sources in the qualification fixture.

The [source fixture](../../native/metal-source/fixed-shapes/main.k) combines imported vector helpers, mixed-shape matrix multiplication, reduction, empty tensors, checked local bindings, result annotations and unused inputs. Separate failure programs exercise each contract without letting an earlier failure hide a missing later guard.

See the [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-fixed-shapes-2026-10-20.json) for platform, sanitizer modes, execution counts, regressions and source hashes.
