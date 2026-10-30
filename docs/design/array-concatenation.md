# Immutable CPU array concatenation

`concat(left, right)` returns the left array's elements followed by the right array's elements. Both inputs remain unchanged and available.

```kalandra
fn main() -> [int] ! TaskError:
  first = [10, 20]
  second = [30, 40]
  joined = try parallel:
    try await spawn concat(first, second)
  slice(joined, 1, 3)
```

This returns `[20, 30]`. Direct calls and pipelines use the same argument order: `first -> concat(second)` means `concat(first, second)`.

## Types and evaluation

The CPU implementation supports the six existing numeric element types, Bool, String, and finite records with their supported nested fields. Both arrays must have the same element type, including nominal record identity. There is no numeric promotion, array-to-tensor coercion, String flattening, or implicit nested array construction. String values themselves continue to use `+` for text concatenation.

Without a result context, nonempty operands infer their elements independently. Thus `concat([int32(1)], [2])` is rejected: the second array defaults to int64. An expected `[float32]` result can type literals in both inputs as float32; it cannot convert an already typed array.

An empty literal can obtain its element context from the other array in either position: `concat([], [1])` and `concat([1], [])` both return `[1]`. Two empty literals need an expected array type. This supplies type information without reordering runtime evaluation. The inputs evaluate from left to right, once each; an earlier fault stops later evaluation.

The result length is the sum of the input lengths. The compiler retains a constant length fact when both are known, diagnosing statically excessive sizes and subsequent statically invalid indexing. Runtime lengths and the combined allocation byte count are checked before result allocation or element reads; wrapped length arithmetic is never used for allocation.

## Ownership and workers

The current implementation copies outer array cells into managed output storage, respecting logical numeric input offsets and strides. Numeric copies preserve raw bits, including signed zero and NaN payloads. Strings and records share existing immutable child storage through the established ownership machinery; concatenation does not recursively duplicate those payloads. Passing the same array twice is valid and repeats its logical elements.

Ordinary helpers, callbacks, explicit local packages, project manifests, direct `spawn concat(...)`, nested workers and cached awaits use the same checked operation. A returned result remains valid after its producing worker or source invocation settles. Cancellation or allocation failure publishes no partial result; expected worker failures use the existing `TaskError` channel.

Shared backing or segmented collection representations remain later optimizations. This implementation makes no allocation-free or constant-time concatenation promise. GPU arrays and nested arrays remain outside this CPU API.

## Scope and qualification

Array slicing and concatenation are collection library additions. They do not replace or expand the accepted core-language requirements, and completing them does not by itself close those requirements. The roadmap prioritizes the remaining accepted core work after this concatenation milestone.

The focused suites cover all existing array element types, strict contexts and nominal identity, exact numeric bits and strided inputs, shared/aliased inputs, nested worker ownership, allocation/cancellation failures, malformed metadata/IR, project composition and the complete example above. See the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-array-concatenation-2026-10-18.json) for final passing checks and scope.
