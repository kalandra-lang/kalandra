# Checked immutable tensor indexing

Read a scalar element by supplying one integer index for each tensor dimension:

```kalandra
fn sample(values: Tensor[float32, ?, ?], row: int, column: uint32) -> float32:
  values[row, column]
```

`sample(tensor([[1, 2], [3, 4]]), 1, 0)` returns `3` with element type `float32`. The input tensor remains available and unchanged. Function calls and indexing bind more tightly than arithmetic: `make()[row, column] * 2` reads the element before multiplying.

This implements the scalar lookup specified in the [accepted tensor design](tensors.md#views-indexing-and-empty-dimensions). It builds on [checked numeric array reads](array-indexing.md), using the same integer extension, size validation, Bounds fault and immutable storage mechanisms.

## Rank and types

The number of indices must equal the tensor's compile-time rank. `matrix[row]` is a rank error for a matrix; it does not slice out a row. A rank-one tensor takes one index. A rank-zero tensor has exactly one element and takes no indices:

```kalandra
fn main():
  scalar = tensor(7)
  scalar[]
```

All six implemented CPU numeric element types work: `int32`, `uint32`, `int64`, `uint64`, `float32` and `float64`. Each index must be a completed CPU integer scalar (`int32`, `uint32`, `int64` or `uint64`, including the `int` and `uint` aliases). Different axes can use different integer types. Floating-point values, Bool, arrays, task handles and unhandled fallible values are not indices.

The result keeps the tensor's element type and exact representation. There is no implicit precision change, numeric conversion or loss of floating-point signed zeros, subnormals or NaN payloads.

## Bounds and evaluation

Each axis requires `0 <= index < extent`. Negative indices do not count from the end; high unsigned indices do not wrap or truncate. Provable violations, including reads from a known empty dimension, are compile-time errors. Unknown violations produce a checked Bounds fault at the lookup's source location before any element is read.

The tensor expression evaluates first, followed by each index expression from left to right, once each. All operands evaluate before shape/byte validation and bounds checks. For example, if an unknown row is negative and the column expression fails arithmetic, the column's original Arithmetic fault is reported. A bounds check does not suppress evaluation of later operands. Rank/type errors and statically invalid bounds remain compile-time diagnostics.

An empty tensor has no readable element. Rank zero is different: it contains one element, so `value[]` reads it without an axis bounds check. Checked invocation cancellation and byte-range validation still apply.

## Functions and workers

Reads work in ordinary functions, captured tensor expressions in numeric map callbacks, independently scheduled helpers, and after awaiting a tensor result. Returned tensors keep their existing retained ownership; reading an element does not consume them. Integer index expressions may themselves await completed work:

```kalandra
fn row() -> int:
  1

fn column() -> uint32:
  0

fn main() ! TaskError:
  values = tensor([[1, 2], [3, 4]])
  try parallel:
    a = spawn row()
    b = spawn column()
    values[try await a, try await b]
```

This returns `3`. Within a worker, a failing lookup follows the existing `TaskError.Fault` path with `FaultKind.Bounds`. Recovering from it does not publish an element from an invalid address. Repeated successful awaits retain identical scalar results.

## Lowering and placement

The parser retains the input and ordered list of indices in `IndexExpr`. Numeric array reads keep their existing `ArrayIndex`; tensor reads use `TensorIndex`. Semantic checks preserve rank, element identity, CPU residence and index types. Typed-IR validation rejects malformed operand references and category/rank/type changes before lowering.

Array and tensor lookups share CPU MLIR extraction lowering. It evaluates all operands, validates extents and byte products, extends each index with its original signedness, then compares it unsigned against its dimension before converting to a storage index. Only after every guard does `tensor.extract` read the element. Continuation kernels and imported callback/helper graphs remap every index operand. Existing checked ABI exits handle faults and cancellation.

There is no hidden device download. Transfer a device-resident tensor to CPU before reading it from host code. GPU kernel indexing and slicing remain separate work; this milestone does not enable lookup operations in a GPU kernel or expose raw pointers.

## Remaining work

Slicing, sub-tensor views, immutable element replacement and custom GPU indexing remain unimplemented. Indexed assignment is rejected: `matrix[row, column] = value` does not mutate a tensor. Broader native values and modules remain on the roadmap. Native distributed execution requires its separate design discussion.

## Qualification

On Apple M2 Max, all 81 focused checks passed across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. The tensor suites exercised 816,192 raw lookup cases, including 143,616 checked Bounds faults, and 1,272 worker harness runs. Coverage includes all six numeric element types, signed/unsigned and mixed-width indices, ranks zero/one/two/three/eight, exact floating-point bits in eight FP environments, positive/negative and column-major borrowed strides, empty axes, invalid metadata and cancellation. Worker checks cover captures, nested helpers, cached results, row/column Bounds recovery, allocation failures, awaited indices and tensors read after their groups close. Array indexing, frontend/source/storage/numeric/control flow and selected GPU regressions also passed. All 640 tracked numerical artifacts and 20 numeric/checked-ABI/runtime/storage sources are unchanged. This is focused qualification, not a full-suite claim; the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-tensor-indexing-2026-10-18.json) records scope and hashes.
