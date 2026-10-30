# Kalandra tensor types and shapes proposal

Accepted tensor design — 2026-10-14. This builds on the accepted core, immutable values, numeric rules, typed errors, and structured tasks. Tensors are numerical values whose element type and shape remain visible to the compiler. The project owner approved these rules for the fresh implementation, not features of Kalandra 0.13.

## Accepted decisions

- `Tensor[elementType, dimensions...]` names an immutable dense tensor with a known rank.
- Dimensions are nonnegative integer constants or `?` for an extent known at runtime.
- `tensor(...)` explicitly constructs a tensor; ordinary array literals remain arrays.
- Infer result shapes where possible and check unresolved shape requirements before computation.
- Tensor arithmetic preserves element type. `*` is elementwise; `matmul` names matrix multiplication.
- Scalar arithmetic may apply the same scalar to every element. Tensor-to-tensor broadcasting is explicit.
- Allocation, safe views, buffer reuse, fusion, and layout selection belong to the compiler/runtime.
- A tensor type describes values, not a CPU/GPU location. Placement is the next proposal.

## Type spelling

| Type                    | Meaning                                               |
| ----------------------- | ----------------------------------------------------- |
| `Tensor[float32, 128]`  | A vector of 128 float32 elements                      |
| `Tensor[float32, 2, 3]` | A 2-by-3 float32 matrix                               |
| `Tensor[float32, ?, 3]` | A matrix with runtime row count and exactly 3 columns |
| `Tensor[float32, ?, ?]` | A matrix with both extents determined at runtime      |
| `Tensor[float32]`       | A rank-zero tensor containing one float32 element     |

Rank is the number of dimensions. The two question marks in a type are independent; they do not imply equal dimensions. Rank is known even when every extent is dynamic. Runtime-variable rank is outside the first core.

Tensor is a built-in parameterized type; this notation does not yet define general user-written generics or arbitrary expressions in type parameters. Named symbolic dimensions and shape-polymorphic functions get a later generics proposal. An undeclared dimension name is not automatically a symbolic parameter.

Initial element types are the accepted fixed-width integers, float32/float64, and Bool. The int/uint/float aliases retain their accepted 64-bit meanings. Records, strings, function values, and resource capabilities are not dense tensor elements in the initial core. Float16, bfloat16, and extended integer arithmetic need the corresponding numerical contracts and device-capability work before inclusion.

## Arrays stay simple

```kalandra
values = [1, 2, 3]
```

This remains `[int]`, an immutable array. It is not implicitly a tensor or a request for GPU execution. Ordinary map still invokes a callback sequentially according to the accepted core.

Nested arrays can be ragged. A dense tensor must have a rectangular shape and one element type. Conversion is explicit because shape/layout requirements differ from those of general arrays.

## Construct and multiply matrices

```kalandra
left: Tensor[float32, 2, 3] = tensor([
  [1, 2, 3],
  [4, 5, 6],
])

right: Tensor[float32, 3, 2] = tensor([
  [1, 2],
  [3, 4],
  [5, 6],
])

product = matmul(left, right)
```

The expected tensor type supplies float32 context to the numeric literals. Product has type `Tensor[float32, 2, 2]` and values `[[22, 28], [49, 64]]`. Both inputs remain unchanged and usable.

With no annotation, `tensor([1, 2, 3])` infers `Tensor[int64, 3]`. An annotated float32 tensor can type literals as float32, but cannot silently narrow an already typed float64 array. Explicit element conversion is required for such a source.

A rectangular nested literal has a known shape. A flat runtime array passed to `tensor(values)` produces a rank-one tensor whose length is dynamic unless the compiler proves its extent. A scalar argument produces a rank-zero tensor. Flat numeric runtime arrays can also use [explicit shape lists](reshape.md) with runtime integer dimensions. Arbitrary nested runtime arrays remain separate work. [CPU tensor reshaping](tensor-reshape.md) now changes dimensions/rank while preserving logical row-major elements.

Known ragged input is invalid:

```kalandra
invalid = tensor([[1, 2], [3]])
```

Diagnose which dimension is inconsistent. Do not silently pad, truncate, or produce a tensor of arrays.

## Static and dynamic checking

For rank-two matmul, left has shape `[M, K]`, right `[K, N]`, and output `[M, N]`. Initial matmul is rank-two only; batching, dot products, and higher-rank contraction have separately named operations/contracts later.

```kalandra
fn project(
  samples: Tensor[float32, ?, 3],
  weights: Tensor[float32, 3, 2],
) -> Tensor[float32, ?, 2]:
  matmul(samples, weights)
```

The compiler knows the inner dimensions agree and that the result has two columns. Its runtime row count is the input row count. This relationship is retained internally even though the public spelling uses `?`.

If two known dimensions conflict, compilation fails. If compatibility depends on runtime extents, insert a check before allocation/dispatch of the affected computation. Do not send an invalid shape to unchecked native or device code.

A dynamic shape can satisfy a static parameter/annotation after a runtime shape check, provided element type and rank already agree. The destination receives the checked static facts; the source binding retains its own type. This refinement does not change elements or silently convert precision. A known mismatch is a compile-time error; an unknown rank or incompatible element type is not fixed by a shape check.

Shape failures are predictable validation errors for external data but can also violate an internal operation's precondition. Follow the accepted checked-operation model:

- Ordinary tensor operations with invalid dynamic shape produce a ShapeFault ending the current execution unit.
- Explicit checked constructors/operations return a typed ShapeError for expected invalid input.
- Add Shape to FaultKind so a supervised child can report this fault through TaskError.Fault.

ShapeError variants can include DimensionMismatch, ElementCountMismatch, and InvalidDimension with typed details. Their exact fields are part of the library/contract API before implementation, not an untyped message exception.

## Runtime data construction

Constructor behavior:

```kalandra
matrix = tensor(values, [rows, columns])
```

Assume values is a flat homogeneous numeric array and rows/columns are valid nonnegative integer values. The shape argument has statically known length two, so rank is two; its extents may be dynamic. If values has float32 elements, the general result is `Tensor[float32, ?, ?]`.

Dimensions must be nonnegative, fit the runtime indexing representation, and have a checked element-count product matching the source length. Negative constants and known mismatches are compile-time diagnostics. Dynamic failures are ShapeFault for the ordinary constructor.

For untrusted data, use `checkedTensor(values, shape)`, which returns the corresponding tensor type `! ShapeError`. The compiler knows output rank from the shape argument's static length; an arbitrary runtime-length shape array is not an implicit unranked tensor constructor in this first design.

Type/rank/shape information can be strengthened when the compiler proves constants. The constructor cannot silently reshape, resize, or narrow elements to fit an annotation.

## Operations and result types

| Operation                                      | Initial rule                                                                                  |
| ---------------------------------------------- | --------------------------------------------------------------------------------------------- |
| `a + b`, `a - b`, `a * b`, `a / b` for tensors | Same element type and exact shape; elementwise result                                         |
| Tensor arithmetic with a scalar                | Scalar has the element type, or a literal is typed by that context; apply it to every element |
| `matmul(a, b)`                                 | Same floating element type; rank-two matrix product                                           |
| `mapElements(a, fn(value): ...)`               | Pure, supported element transformation; same shape                                            |
| `reshape(a, shape)`                            | Same element type/count; new logical shape                                                    |
| `transpose(a)`                                 | Rank-two axis swap; same element type                                                         |
| `reduceSum(a)`                                 | Scalar of the element type, under a defined reduction order                                   |
| Tensor comparisons                             | Elementwise Bool tensor of the operand shape                                                  |

The first matmul implementation targets float32/float64. Integer matrix accumulation and mixed-precision operations need their own overflow/accumulation contracts before support is claimed. Bool tensors support representation, selection, and logical/comparison operations; Bool is not an implicitly numeric arithmetic type.

An element callback receives exactly one element, never a hidden index or source tensor. Its input type is inferred. Result element type follows the callback's known type; it cannot vary from element to element. The initial mapElements subset excludes IO, remote calls, arbitrary native callbacks, task creation, and expected-error channels. Unsupported callbacks are diagnosed rather than silently run through an unrelated array interpreter.

Tensor comparisons differ from scalar equality: comparing tensors elementwise returns a Bool tensor, not one Bool for the whole collection. A whole-tensor equality/reduction function can be provided separately. The complete operator table must make this overload clear.

A scalar and rank-zero tensor are distinct types. Explicit constructor/extraction operations connect them; a rank-zero tensor is not silently unwrapped to hide a device synchronization or transfer.

## Explicit CPU element conversions

A numeric type-name call accepts a completed CPU tensor as well as a scalar:

```kalandra
fn approximate(values: Tensor[int64, ?, 3]) -> Tensor[float32, ?, 3]:
  float32(values)
```

The result preserves rank and every extent, including dynamic extent relationships and zero dimensions. Only the element type changes. All 100 pairs among int8, uint8, int16, uint16, int32, uint32, int64, uint64, float32 and float64 are supported; int/uint/float retain their 64-bit aliases. Arguments keep their existing element type; the destination does not supply a literal context to the input. In the current subset, construct a tensor with an explicit source annotation before converting it. An annotation alone never converts an already typed value.

Each element follows the [scalar conversion rules](numbers.md#explicit-cpu-scalar-numeric-conversions), including checked integer ranges, truncation before float-to-integer bounds checking, ties-to-even rounding and changed-width NaN canonicalization. Identity conversions preserve every bit. Rank zero remains a tensor containing one converted element; it does not become a scalar. Empty tensors preserve their shape and perform no element conversions.

Conversion produces an immutable value. Inputs remain usable, including borrowed strided views; the compiler may reuse storage only when doing so preserves those semantics. Both source and destination byte sizes are checked. A failing element raises ArithmeticFault at the conversion call and publishes no partial tensor. Tensor element checks currently run at execution time, including for tensor literals; the scalar constant diagnostics do not imply tensor constant evaluation. Allocation failures and cancellation likewise publish no successful result and release intermediates.

Ordinary helper calls and `spawn float32(values)` use the same checked conversion. Completed worker results retain their backing through repeated awaits and after the source execution unit settles. GPU-resident operands require an explicit transfer to CPU first; GPU casts remain rejected. See [checked CPU tensor conversions](narrow-tensor-conversions.md) for current qualification.

## Broadcasting is explicit

```kalandra
scaled = left * 2
```

The literal 2 takes float32 from left, and scaled has left's shape. This is the only implicit shape lifting in the initial arithmetic model: applying a scalar to all elements.

Two tensors must have the same shape for elementwise operators. A `[3]` tensor does not silently become `[2, 3]`, and a rank-zero tensor is not implicitly a scalar. General broadcasting uses an explicitly named operation with a documented dimension mapping. That API is deferred rather than inheriting a large set of implicit trailing-axis rules.

Matrix multiplication uses matmul, not overloaded `*` or a new punctuation operator. There is no ambiguity between elementwise multiplication and matrix multiplication.

[Native CPU collection queries](collection-queries.md) now expose runtime tensor extents with `shape(values) -> [int]`. The array contains one extent per dimension, including `[]` for rank zero. `shape(values)[axis]` uses ordinary checked array indexing.

## Views, indexing, and empty dimensions

[Native CPU scalar lookup](tensor-indexing.md) is implemented. A rank-two scalar lookup uses `matrix[row, column]`. It supplies exactly one index per dimension and returns an element. A missing/extra index is a rank error. Negative or out-of-range indices fail; there is no implicit wraparound indexing. Provable violations are compile-time errors, dynamic violations are BoundsFault.

Indexing device-resident data may require synchronization/transfer; placement APIs must expose that cost rather than treat it as a free pointer read. Slicing and sub-tensor view syntax are a later API decision.

[Native CPU reshape](tensor-reshape.md) is implemented as an ordered managed copy preserving element type and count. Transpose remains separate work. Reshape and transpose produce immutable logical values. The compiler can share backing storage or copy it when required by physical layout. No user-visible mutation is permitted, and views keep their source storage valid.

Zero-sized dimensions are legal. A tensor shaped `[0, 3]` has zero elements. Rank zero has one element, because the empty dimension product is one. Shape arithmetic is checked before allocation. ReduceSum of an empty tensor returns the additive zero of its element type. Operations with no useful identity, such as mean, need an explicit expected-error or optional-result contract.

## Precision, reductions, and optimization

Elementwise arithmetic follows the accepted scalar numeric rules, including checked integer overflow and declared floating precision. A tensor expression does not automatically opt into fast-math, lower precision, wrapping integer arithmetic, or flush-to-zero behavior.

Hardware float32 `matmul` defaults to the **optimized full-precision** policy.
It preserves float32 inputs and accumulators while permitting fused multiply-add
and implementation-dependent contraction order. The qualified Apple M2 Max
profile uses pinned MIT-licensed MLX Steel SIMD-group kernels; other devices,
unsupported shapes and older bundles retain the existing kernels. This policy
does not permit silently converting float32 operands to float16/TF32, generic
fast-math assumptions, relaxed integer checks or relaxed scalar/post-map arithmetic.
The CPU currently retains its fixed-tree implementation under either policy.

Use `--matmul-policy fixed-tree` when compiling to retain separately rounded
products followed by the fixed balanced sum tree in ascending contraction-index
order. Native Metal callers can also set `Options::fixedTreeMatmul=true` on an
optimized bundle. `--arithmetic software` always selects fixed-tree arithmetic.
The optimized policy does not promise the same output bits as fixed-tree,
cross-device bitwise equality, a fixed accumulation order or universally better
accuracy. Signed zero and intermediate-overflow/NaN behavior can differ. Severe
cancellation may have large relative error under either policy; use an
appropriate numerical algorithm or precision when that matters.

ReduceSum retains its fixed tree over row-major logical element order. Fixed-tree
matmul and ReduceSum carry an odd tree level's final unpaired value to the next
level; an empty reduction returns additive zero. Their order is independent of
thread/block scheduling. Do not initialize a nonempty fixed-tree sum with an
extra zero that changes signed-zero behavior. Integer matmul remains checked at
its specified products/tree operations, under both policies.

The accepted fixed-tree contract extends the scalar rules, not a claim that existing MLIR/IREE or vendor kernels already implement it. If the backend cannot honor it, the implementation must add a matching lowering or diagnose/select an explicitly different policy. Integer reduceSum checks overflow at the specified tree operations, not only in the final mathematical total.

Compiler IR keeps tensor operations and shape facts visible for fusion, layout selection, specialization, and transfer planning. Fusion may remove intermediate allocations, but cannot change observable checked failures or promised numerical results. Do not materialize an intermediate merely because its source has a binding.

## Placement, contracts, and representation

`Tensor[float32, 2, 3]` describes six values and their indexing, not a particular device, stride representation, or allocation policy. CPU/GPU/remote placement is explicit in the next proposal. Shape/element checks and device-capability checks must agree before dispatch.

Remote contracts carry validated element type, rank, extents, and encoded elements under a declared layout/encoding. They do not serialize raw host/device pointers or guess native memory layout. Wire details are a later contract design. Input-size limits and checked dimension products prevent malformed shapes from triggering unsafe allocation.

## First implementation scope and evidence

Start with dense ranked tensors, float32/float64 CPU operations, literal/flat construction, dynamic extent checks, elementwise arithmetic, rank-two matmul, reshape/transpose, and reduceSum. Add checked integer/Bool operations where the scalar semantics can be preserved. Validate one GPU backend against the same language contract before claiming GPU support. Sparse tensors, autodiff, low-precision/mixed accumulation, generalized contraction, and custom kernels are subsequent capabilities.

MLIR's tensor model already represents immutable tensors and permits lowering redundant copies into safe in-place operations. That matches the language direction but does not automatically provide its bounds/failure/numerical guarantees. [MLIR tensor dialect](https://mlir.llvm.org/docs/Dialects/TensorOps/).

IREE demonstrates lowering Linalg-on-tensor programs to executable targets. Use that path to evaluate the vertical slice, preserving any semantic checks the frontend requires. [IREE MLIR/Linalg tutorial](https://iree.dev/community/blog/2024-01-29-iree-mlir-linalg-tutorial/).

## Validation before implementation

1. Infer literal element types, rank, and extents; diagnose ragged/mixed typed input.
2. Matmul of the example matrices produces float32 shape `[2, 2]` and values `[[22, 28], [49, 64]]`.
3. Check known and dynamic inner dimensions before computation/dispatch.
4. Static refinement validates dynamic extents without changing element type or source values.
5. CheckedTensor returns ShapeError for expected invalid runtime data; ordinary violations fault safely.
6. Preserve exact elementwise shape rules and scalar lifting without hidden tensor broadcasting.
7. Enforce bounds, dimension products, zero-size behavior, rank-zero distinction, and immutable view lifetimes.
8. Preserve numeric contracts across fusion, buffer reuse, threading, and backend lowering.
9. Validate deterministic reduction order and any later explicitly relaxed policy separately.
10. Validate CPU/GPU capability and transport schemas without hidden precision changes.

## Accepted design and next step

Tensor notation, static/dynamic extent rules, scalar lifting versus explicit tensor broadcasting, ShapeFault/ShapeError, and the deterministic reduction baseline are accepted.

Review [placement and transfers](placement.md) next. Then write a complete CPU/GPU/remote example before implementing the first MLIR/native vertical slice.

The native constructor now implements the flat-literal/constant-shape part of this design: `tensor([], [0, 3])` creates an empty matrix without changing the rank of a rank-one literal through an annotation. It uses the existing tensor-literal IR and checked CPU lowering; the Metal source subset supports rank one/two constant data. Runtime arrays and computed shape arguments remain unimplemented. See [shared placement qualification](placement-ir.md#explicit-literal-shapes).

## Native tensor element and shape inference

The CPU frontend now implements unannotated scalar and literal-data construction:

```kalandra
fn main():
  values = tensor([[1, 2], [3, 4]])
  reduceSum(values)
```

`values` has type `Tensor[int64, 2, 2]`; the result is `10`. Whole-number scalar expressions default to `int64`, and decimal/exponent expressions default to `float64`, following the scalar rules. A scalar input such as `tensor(7)` produces a rank-zero tensor. `tensor([1, 2, 3, 4], [2, 2])` infers the same element type with the explicit constant shape.

Without an expected tensor type, each leaf expression is checked independently, in source order. All leaves must be completed CPU numeric scalars of exactly the same type. Bindings, function calls and explicit numeric conversions can supply any of the ten implemented CPU element types. For example, two `int32` parameters produce an `int32` tensor without a result annotation:

```kalandra
fn pair(left: int32, right: int32):
  tensor([left, right])
```

There is no common-type promotion across elements: `tensor([1, 2.5])` and `tensor([int32(1), 2])` are rejected in either order. Use an explicit tensor annotation to give numeric literals context, or explicit conversions to align already typed values. Existing contextual construction such as `values: Tensor[float32, 2] = tensor([1, 2])` continues to work.

Empty data has no element-type evidence and still needs context: `empty: Tensor[int64, 2, 0] = tensor([], [2, 0])`. Shape dimensions alone do not select an element type. Ragged arrays, invalid shapes, mixed numeric identities, Boolean leaves, tensors as leaves, task handles and unhandled expected-error results are rejected. Calls are evaluated once in source order; the first runtime failure stops construction and publishes no partial result.

Inferred tensors use the existing checked tensor IR, ownership and arithmetic lowering. They can pass through ordinary calls, conditional shape joins, nested workers and cached awaits. This does not add general array values, runtime-array construction, dynamic shape arguments or additional GPU numeric capabilities. Explicit element conversion remains the way to change an already inferred tensor's element type.

Qualification on Apple M2 Max: 59 focused checks passed, including debug/optimized/ASan+UBSan/separate TSan execution, 864 worker harness runs, 192 allocation/lifetime cases and 640 exact retained float64 lane checks. All 640 tracked numerical artifacts remain unchanged. This is focused qualification, not a full-suite claim. The [inference evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-tensor-inference-2026-10-17.json) records source/artifact hashes and coverage.

## Native runtime array construction

The CPU constructor now accepts completed numeric arrays through `tensor(values)` or `values -> tensor()`. The result is rank one with the same element type and length. See [array-to-tensor construction](arrays.md#explicit-tensor-construction-from-arrays) for shape checks, ownership, workers and current limits. Ranked literal construction remains unchanged; [explicit runtime shapes](reshape.md) now support checked row-major matrices and higher ranks from flat array data.

## Native tensor-to-array construction

[Explicit CPU `array(tensor)` construction](tensor-array.md) now flattens logical row-major elements into an immutable numeric array, preserving the source element type. Rank-zero tensors yield one element; any zero extent yields an empty array. Helpers, map callbacks and direct/nested workers preserve array categories and retained result ownership. The current implementation makes a checked managed copy; shared storage is deferred optimization work.

## Narrow CPU construction and elementwise operations

[Checked 8/16-bit CPU tensors](narrow-tensors.md) now extend construction, elementwise integer arithmetic, structural array/tensor adapters, indexing, reshaping, record fields and retained local workers to all ten implemented numeric types. [Checked narrow CPU reductions](narrow-reductions.md) are supported. [Checked narrow CPU matrix multiplication](narrow-matmul.md) is supported. [Checked CPU tensor numeric conversions](narrow-tensor-conversions.md) now cover all 100 pairs. Operation support must still be checked independently of storage availability. GPU admission remains separate.
