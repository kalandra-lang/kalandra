# Runtime tensor shapes from flat arrays

The native CPU constructor accepts a flat numeric array and an explicit shape whose dimensions can be runtime integer expressions:

```kalandra
fn matrix(values: [float32], rows: int, columns: int) -> Tensor[float32, ?, ?]:
  tensor(values, [rows, columns])
```

`matrix([1, 2, 3, 4], 2, 2)` produces the matrix `[[1, 2], [3, 4]]`. Elements keep their original numeric type, raw representation and row-major order. The input array remains available. The resulting tensor supports ordinary tensor arithmetic, reductions and matrix multiplication.

A flat literal can also use runtime dimensions: `tensor([1, 2, 3, 4], [rows, columns])`. Existing literal construction with constant shapes and `tensor(values)` without a shape retain their behavior.

## Rank and dimensions

The shape must be a list written at the constructor call, such as `[rows, columns]`. Its number of entries fixes the result rank at compile time; entries may be runtime expressions. Passing a separately stored `[int]` shape array with an unknown length is not implemented.

Dimensions accept completed CPU `int32`, `uint32`, `int64` or `uint64` scalars, including the `int` and `uint` aliases. A list can mix those integer types. Floating-point values, Bool, arrays, task handles and unhandled fallible values are rejected. Each extent must fit the nonnegative signed 64-bit indexing range. `-1` is not an inferred-size marker.

The product of the dimensions must equal the input array length. For example, `[2, 3]` requires six elements. An empty shape `[]` constructs a rank-zero tensor and requires exactly one element. Any zero dimension requires an empty input array: `[2, 0]` constructs an empty rank-two tensor.

A fixed result annotation constrains the dimensions; it does not override them:

```kalandra
fn four(values: [int], rows: int, columns: int) -> Tensor[int, 2, 2]:
  tensor(values, [rows, columns])
```

This requires both dimensions to be two and the array length to be four. Known contradictions are compile-time errors. Unknown dimensions and counts are checked before allocating or copying the result.

## Checks and evaluation

The input and dimension expressions evaluate once, in source order. Ordinary checked integer arithmetic applies inside each dimension expression and retains its original fault location. The constructor then checks dimension ranges, fixed extent constraints, allocation-byte products, row-major strides and element counts before allocating output storage.

Invalid dimensions, fixed extents or element counts produce a checked Shape fault. Byte-count or stride overflow produces a checked Allocation fault. A zero element count does not waive the stride checks: `[0, huge, huge]` can have an unrepresentable row-major stride even though it has no elements. Cancellation and allocation failure publish no partial tensor, and intermediate owners settle normally.

The implementation extends the existing `ArrayToTensor` operation with explicit dimension operands. Dimensions are carried through continuation kernels and imported callback graphs. CPU MLIR lowering makes an ordered copy into managed tensor storage and derives each destination index from the flat input index. This reuses the numeric engines, checked invocation ABI and ownership runtime.

## Workers

```kalandra
fn background(values: [float32], rows: int, columns: int) -> Tensor[float32, ?, ?] ! TaskError:
  try parallel:
    work = spawn tensor(values, [rows, columns])
    try await work
```

Construction in ordinary worker helpers and anonymous map callbacks also works. Dimension captures retain their concrete integer types. Repeated awaits reuse the completed result; returned tensors remain valid after their workers finish. Worker faults use the existing TaskError recovery rules. Ordinary argument expressions run before dispatch; put the complete pipeline in a helper when that pipeline should execute in the worker.

## Remaining work

This constructor takes flat numeric arrays. [CPU tensor-to-tensor `reshape`](tensor-reshape.md) is now implemented separately. Inferred dimensions, runtime-variable ranks and GPU/mixed-target array construction remain separate work. Distributed native execution is not implemented.

## Qualification

On Apple M2 Max, 77 focused checks passed across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. The reshape suites ran 11,600 raw shape/ownership cases with 2,118,144 exact lane checks and 1,272 worker harness runs. Coverage includes all six numeric element types, all four integer dimension types in frontend/lowering, ranks zero through three, mixed-width dimensions, large zero-element shapes, checked byte/stride overflow, active cancellation, dimension fault ordering, matrix multiplication and retained worker outputs. Earlier constructor, numeric, tensor-inference, frontend, source/storage and selected GPU regressions passed. All 640 tracked numerical artifacts and the numeric/runtime sources remain unchanged. This is focused qualification, not a full-suite claim; the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-runtime-reshape-2026-10-18.json) records scope and hashes.
