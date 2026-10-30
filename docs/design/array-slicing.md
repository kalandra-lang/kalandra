# Immutable CPU array slicing

Use `slice(values, start, end)` to select an array range. The start is inclusive and the end is exclusive, matching the existing String slicing convention.

```kalandra
fn middle(values: [int]) -> [int]:
  slice(values, 1, 3)

fn main() -> [int] ! TaskError:
  original = [10, 20, 30, 40]
  selected = try parallel:
    try await spawn middle(original)
  [original[0], selected[0], selected[1], original[3]]
```

This prints `[10, 20, 30, 40]`. The selected array contains `[20, 30]`; `original` stays available and unchanged. A slice can be returned from a function, captured by a callback, passed to another worker, or observed through repeated awaits.

## Types and bounds

The CPU subset supports all existing array element types: `int32`, `uint32`, `int64`, `uint64`, `float32`, `float64`, `Bool`, `String`, and finite records with their supported nested fields. The result preserves the exact element type and nominal record identity. Floating-point values retain their raw bits, including signed zeros and NaN payloads.

Both bounds must be completed CPU integer scalars; signed/unsigned 32-bit and 64-bit bounds can be mixed. A valid range satisfies `0 <= start <= end <= length(values)`. There is no negative indexing, clamping, inclusive end, or step argument. Empty ranges are valid, including `slice(values, length(values), length(values))`.

Statically provable invalid ranges are compiler errors. Dynamic invalid ranges produce a checked Bounds fault at the slice expression, before result allocation or element reads. Worker faults use the existing `TaskError` channel and can be recovered with `catch`. Input and bounds evaluate from left to right, once each; an earlier failure stops later evaluation.

An expected array type can provide literal context, including `fn empty() -> [float32]: slice([], 0, 0)`. Already typed arrays do not convert implicitly. Without element context, `slice([], 0, 0)` is rejected.

Pipelines and direct worker dispatch are supported:

```kalandra
fn main() -> [int] ! TaskError:
  try parallel:
    try await spawn slice([1, 2, 3, 4], 1, 3)
```

Ordinary composition such as `values -> slice(1, 3) -> map(fn(value): value * 2)` has the same argument order and sequential evaluation as ordinary calls. A slice has runtime length; constant bounds retain private length facts for subsequent checking.

## Storage and execution scope

This implementation copies the selected outer array cells into managed storage. Numeric reads honor logical input strides. Strings and records retain their existing immutable child storage through the shared ownership machinery; slicing does not recursively duplicate their payloads. The result remains valid after the source worker or invocation settles. Allocation failure and cancellation publish no partial result and release temporary storage.

Shared-storage array views are a later optimization, tracked with the existing collection storage work. This implementation does not introduce a public backing-storage identity or promise that slicing is allocation-free.

Array slicing is CPU-only. Tensors, nested arrays and mixed CPU/GPU placement are outside this operation's current scope. The existing scalar-based String slice behavior is unchanged.

## Qualification

The focused suites cover all nine element categories and four integer bound types, malformed typed IR, exact numeric bits and strided input, nested worker calls and cached awaits, String/record child ownership, source and scheduler allocation failures, cancellation, deadlines and project/package composition. Debug, optimized, ASan/UBSan and separate TSan execution use the existing shared runtimes. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-array-slicing-2026-10-18.json) records the final passing checks and scope.
