# Native CPU tensor reshaping

`reshape` changes a tensor's dimensions while preserving its numeric type and logical row-major element order:

```kalandra
fn flatten(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?]:
  dimensions = shape(values)
  reshape(values, [dimensions[0] * dimensions[1]])

fn main():
  values = tensor([[1, 2], [3, 4]])
  flattened = reshape(values, [4])
  flattened[2]
```

The program returns `3`. `values` remains a matrix with its original elements. The result is a rank-one tensor, not an array. This implements the operation in the [accepted tensor design](tensors.md); [flat-array construction](reshape.md) remains `tensor(array, dimensions)`.

## Shape and type rules

Both input and result are completed CPU tensors of the same element type. All six implemented types work: `int32`, `uint32`, `int64`, `uint64`, `float32` and `float64`. Reshape never converts precision or integer signedness. Use explicit numeric conversion separately when needed.

The second argument must be a shape list at the call site, such as `[rows, columns]`. The list length fixes the output rank at compile time. Each dimension can be a runtime expression of type `int32`, `uint32`, `int64` or `uint64`; axes can mix those types. Dimensions must be nonnegative and fit the signed 64-bit indexing range. `-1` is not inferred-size syntax. Float, Bool, task handles and unhandled fallible values are rejected as dimensions.

The output element count must equal the input element count. Known contradictory counts are compile-time errors. Other count requirements are checked at runtime before allocating or copying the result. An empty shape `[]` produces a rank-zero tensor and requires exactly one element:

```kalandra
fn main():
  values = tensor([[7]])
  scalar = reshape(values, [])
  scalar[]
```

This returns `7`. Conversely, `reshape(tensor(7), [1, 1])` produces a one-by-one matrix. A zero dimension requires an input with no elements; an empty `[0, 3]` matrix may become `[3, 0]` or a zero-length vector.

A fixed result annotation constrains the requested dimensions rather than overriding them:

```kalandra
fn square(values: Tensor[int, ?, ?], rows: int, columns: int) -> Tensor[int, 2, 2]:
  reshape(values, [rows, columns])
```

This requires both dimensions to be two and the input to contain four elements. A mismatched annotation does not silently resize or discard data.

`reshape(array, dimensions)` is rejected; use the array-to-tensor constructor. `tensor(existingTensor)` remains unsupported. Reshape calls take exactly two arguments and follow normal function name resolution and pipeline insertion: `values -> reshape([rows, columns])` is equivalent to the ordinary call.

## Ordering, errors and memory

The tensor expression evaluates first, then dimension expressions from left to right, once each. Their arithmetic and expected-error failures retain their original locations. All dimensions evaluate before their ranges and output shape are validated. An invalid runtime row does not suppress a later column expression's Arithmetic fault.

Dimensions, fixed result extents and element counts are checked before allocation. Violations produce a checked Shape fault. Byte products and output row-major strides must also be representable; overflow produces an Allocation fault. Empty tensors still need representable output strides: `[0, huge, huge]` can overflow a suffix stride even with zero total elements.

The initial CPU implementation makes an ordered copy into managed output storage. It reads logical row-major coordinates from the source, respecting its descriptor's strides and offset, and writes the corresponding coordinates in the output. Non-contiguous, transposed-layout and negative-stride borrowed inputs follow the same logical order. The original backing remains unchanged. Every element keeps its exact representation, including signed zeros, subnormals and NaN payloads.

Returned tensors remain valid after their source function or worker finishes. Cancellation and allocation failure publish no partial result and settle intermediate owners. Shared storage for compatible layouts is deferred to the [roadmap optimization stage](../roadmap.md#optimization), after missing native functionality. Immutability permits that optimization, provided the result retains its backing for its full lifetime; incompatible layouts or borrowed storage may still require an ordered copy. The current implementation does not promise a zero-copy view or introduce manual memory management.

## Workers and callbacks

Reshape works in ordinary helper graphs, synchronous numeric map callbacks with tensor captures, and direct CPU worker calls:

```kalandra
fn main() ! TaskError:
  values = tensor([[1, 2], [3, 4]])
  flattened = try parallel:
    work = spawn reshape(values, [4])
    first = try await work
    again = try await work
    again
  flattened[3]
```

This returns `4`. Repeated awaits reuse the completed immutable result. Workers preserve numeric identities, retained output ownership and existing TaskError recovery for Shape, Allocation and Arithmetic faults. Dimension expressions can themselves await completed integer work. Argument expressions normally run before dispatch; put their computation in a helper when it should occur inside the worker.

## Lowering and scope

Typed IR uses `TensorReshape` with a tensor input and ordered dimension operands. Array construction keeps `ArrayToTensor`. Both share dimension validation and output-copy lowering. CPU MLIR checks source/output element counts, validates dimensions/bytes/strides, unflattens a logical element index into source and target coordinates, and copies exact elements with existing checked invocation/ownership machinery. Continuation kernels and imported callback/helper graphs remap the input and all dimensions. Capability validation rejects malformed IDs, rank/type/category changes and resident GPU inputs before lowering.

GPU/mixed-target reshape, stored shape arrays as constructor arguments, inferred dimensions, runtime-variable rank, slicing and zero-copy views remain separate work. There is no implicit device download; transfer a resident tensor to CPU before using this operation. Broader native values and modules remain on the roadmap, and distributed execution requires its agreed design discussion.

## Qualification

On Apple M2 Max, all 85 focused checks passed across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. The tensor reshape suites ran 47,240 raw copy/shape/ownership cases with 3,169,152 exact lane checks and 1,272 worker harness runs. Coverage includes all six numeric types, signed/unsigned and mixed-width dimensions, source ranks zero/one/two/three/eight, result ranks zero through three, fixed output extents, row/column-major and negative-stride input, empty shapes, exact FP bits in eight environments, allocation failure, source independence, retained results and active million-cell cancellation. Worker callbacks, cached awaits, fault recovery, matrix consumers, operand fault order and earlier array-based reshape/frontend/source/storage/numeric/selected GPU regressions also passed. All 640 tracked numerical artifacts and 20 numerical/checked-ABI/runtime/storage sources remain unchanged. This is focused qualification, not a full-suite claim; the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-tensor-reshape-2026-10-18.json) records scope and hashes.
