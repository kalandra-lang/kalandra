# Immutable native arrays

The native CPU subset now implements the construction and value-passing part of the [accepted core array design](core.md#4-transform-an-array). Numeric arrays support all ten implemented scalar element types, including [8/16-bit integers](narrow-arrays.md). Numeric arrays and [finite record arrays](record-arrays.md) are ordered immutable collections. `[int]` is distinct from a ranked tensor type, and its length is runtime data rather than part of the source type.

```kalandra
fn pair(left: int, right: int) -> [int]:
  [left, right]

fn choose(flag: Bool) -> [int]:
  if flag:
    pair(10, 20)
  else:
    []
```

`choose` may return two elements or none. Both results have type `[int64]`. There is no equal-length requirement between arrays passed to a function or returned by its branches.

## Elements and inference

Numeric arrays support `int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float32` and `float64` elements, including the accepted `int`, `uint` and `float` aliases.

Without context, each leaf expression is inferred independently in source order. Whole-number expressions default to `int64`; decimal/exponent expressions follow the scalar `float64` defaults. Numeric array elements must have exactly the same completed CPU numeric type. `[1, 2.5]` and `[int32(1), 2]` are rejected in either order. An expected array type supplies literal context:

```kalandra
fn samples() -> [float32]:
  [1, 2, 3]

fn empty() -> [uint]:
  []
```

Already typed values must conform exactly; contextual typing does not silently convert them. Empty arrays need element context, so an unannotated `values = []` is rejected. Nested arrays and arrays of functions, tasks or expected-error results remain separate work. Bool, String and finite record arrays now have their own implemented storage paths.

Array elements evaluate left to right, once each. A runtime failure stops evaluation and retains its original source location. No partial array result is published. Source/value/depth limits and the existing bounded collection-element, indexing-range and allocation-byte checks apply.

[Collection size queries](collection-queries.md) now expose `length(values)` as `int64`. Tensor extents use `shape(tensor)`, returning an immutable `[int64]` in dimension order.

## Sharing, calls and workers

Passing an array does not consume its binding. Functions can return it, and the returned value remains valid after the function or worker completes. Multiple calls and tasks may share the same immutable input. Ordinary calls, pipelines to named functions, conditional length joins, structured parallel scopes, nested workers, cached awaits and TaskError recovery preserve array types and ownership.

Numeric arrays reuse checked rank-one storage and managed ownership from the tensor runtime. A separate source-category flag distinguishes arrays throughout typed IR. Worker values have separate array layout names and private identities, even when an array and a rank-one tensor carry the same numeric element type. These private layouts are not a public native or wire ABI.

There are no implicit array-to-tensor or tensor-to-array coercions. Tensor arithmetic, reductions, matrix multiplication, numeric conversion calls and device transfers do not accept arrays in this slice. GPU/mixed-target array execution is gated.

## Explicit tensor construction from arrays

For all ten implemented CPU tensor element types, use `tensor(values)` to construct a rank-one CPU tensor from a completed numeric array:

```kalandra
fn total(values: [float], offset: float) -> float:
  values -> map(fn(value): value + offset) -> tensor() -> reduceSum()
```

The constructor preserves the array's numeric element type, element order and length, including private dynamic extent facts. An empty typed array produces an empty tensor. The array remains available for later calls; it does not become a tensor implicitly. To change element type, use an explicit numeric conversion in `map` or on the resulting tensor.

A fixed tensor annotation checks the runtime length rather than reshaping the array:

```kalandra
fn four(values: [int]) -> Tensor[int, 4]:
  tensor(values)
```

A statically known wrong length is rejected; an unknown wrong length raises a checked Shape fault before allocating the result. [Explicit runtime shapes](reshape.md) now support `tensor(values, [rows, columns])`, with a fixed rank and checked integer dimension expressions. The existing ranked literal constructor, including `tensor([1, 2, 3, 4], [2, 2])`, remains available.

The current implementation performs a checked ordered copy into managed tensor storage. It preserves raw floating-point representations, including signed zero and NaN payloads. Borrowed native input storage does not escape as an owned tensor result. Cancellation or allocation failure publishes no partial result and settles intermediate storage.

Construction also works in ordinary worker functions, in anonymous map callbacks and through `spawn tensor(values)`. Worker handoff preserves the separate array-input and tensor-result categories and the existing retained ownership rules. Calls used as arguments are evaluated before dispatch; use a worker helper when the whole map/construction pipeline should run in that worker. GPU/mixed-target array construction is not part of this CPU slice.

[Half-open CPU array slicing](array-slicing.md) now selects immutable ranges through `slice(values, start, end)`, preserving element type, bounds checks and retained worker ownership.

## Remaining collection work

This is an array value foundation, not the complete collection API. [Sequential `map` with named functions or inline anonymous numeric/Bool/record callbacks and lexical captures](array-map.md) is now implemented. First-class function values remain separate work. [CPU tensor-to-tensor reshaping](tensor-reshape.md) is implemented separately. [Explicit CPU tensor-to-array construction](tensor-array.md) now provides `array(tensor)`, flattening logical row-major elements while preserving the element type and retained worker ownership. Nested arrays and other general element types remain unimplemented; finite record elements use the [record-array storage path](record-arrays.md), while [Bool arrays](bool-arrays.md) use checked canonical byte storage. [Checked read-only numeric array indexing](array-indexing.md) is implemented; [CPU tensor scalar indexing](tensor-indexing.md) is also implemented. Indexed writes are never part of the immutable model; any future replacement operation must produce another logical value.

The development CPU CLI now [displays array and tensor `main` results directly](collection-results.md), including completed worker results. Array-returning helpers also remain available through checked native test interfaces and worker outcomes.

## Qualification

On Apple M2 Max, 83 focused checks passed across frontend and generated/native execution, including debug, optimized, ASan/UBSan and separate ThreadSanitizer variants. The array suites ran 1,296 worker harness invocations, 192 allocation/lifetime cases and 640 exact retained float64 lane checks. Existing tensor inference, control-flow, numeric workers and selected Metal regressions also passed. All 640 tracked numerical artifacts remain unchanged. This is focused qualification, not a full-suite claim. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-arrays-2026-10-17.json) records coverage and source/artifact hashes.

Explicit runtime array-to-tensor construction passed 87 focused checks on Apple M2 Max, including debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. The constructor suites ran 2,304 copy invocations with 3,159,552 exact lane checks, 312 fixed-shape/ownership cases, 72 size-metadata cases, 48 pre-entry/active cancellation cases and 1,272 worker harness runs. Existing map/closure, array, numeric frontend, tensor inference and selected CPU/GPU regressions passed. All 640 tracked numerical artifacts and the numeric/runtime sources remain unchanged. See the [constructor evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-array-tensor-2026-10-18.json) for scope and hashes.

## Record elements

[Immutable record arrays](record-arrays.md) now extend construction, length, indexing and CPU helper/worker transport to finite record values with nested numeric collection fields. Mapping over record elements and record-array fields remain separate gates.

[CPU array concatenation](array-concatenation.md) combines two same-typed arrays with `concat(left, right)`, retaining immutable inputs and checked worker ownership.
