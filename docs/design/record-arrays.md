# Immutable arrays of records

Native CPU arrays can contain values of one finite record type. The element record may contain nested records, numbers, `Bool`, numeric array/tensor fields, and [Bool-array fields](record-bool-fields.md).

```kalandra
type Reading:
  value: float
  samples: [float]

fn batch() -> [Reading]:
  [Reading { value: 1, samples: [2, 3] }, Reading { value: 2, samples: [4, 5] }]

fn keep(rows: [Reading]) -> [Reading]:
  rows

fn main() -> float ! TaskError:
  rows = try parallel:
    work = spawn keep(batch())
    first = try await work
    again = try await work
    again
  rows[1].samples[0] + float(length(rows))
```

The result is `6`. Helpers can return record arrays, workers can capture and return them, and repeated awaits read the same immutable outcome.

## Types and operations

`[Reading]` names an array of that exact record declaration. Identical field layouts or same-spelling declarations in other modules do not make record types interchangeable. An imported function can return its own module's record array; callers can infer its type, index it, read fields, and pass it back to exported helpers without importing the type name.

Nonempty literals infer their element declaration. Empty arrays need context, such as `rows: [Reading] = []` or a declared `[Reading]` function result. Literal elements are evaluated in written order. Mixing records with numbers, mixing distinct record declarations, and nested arrays are rejected.

`length(rows)` returns `int64`. `rows[index]` returns a record value; indexing accepts the four implemented integer index types and checks bounds before loading a record. Known invalid indices fail during checking. Runtime negative or oversized indices fail through the existing Bounds fault channel. Chained field reads and immutable updates on an indexed record work normally. Such an update does not replace the original array element.

Branches, helper returns and TaskError recovery preserve the array's record identity while allowing different lengths. Existing numeric callbacks can capture a record array and index it, for example `map([0, 1], fn(i): rows[i].value)`.

[Sequential mapping](array-map.md#record-elements-and-results) now accepts record elements and record results with named or anonymous callbacks. [Arrays of records/enums can also be record and enum payload fields](record-array-fields.md). [Slicing](array-slicing.md) and [concatenation](array-concatenation.md) preserve their element identity. Nested arrays, record tensors and record-array CLI display remain separate work. A program's `main` still returns a supported scalar, `Bool`, numeric/Bool array or numeric tensor.

## Ownership and target layout

A record array owns contiguous inline record storage. Element construction copies record values into that storage. Numeric array/tensor fields retain their existing invocation-owned immutable backing; indexing returns an inline record value without copying its numeric field payloads. Reading such a field still follows the [collection-field copy baseline](record-collections.md).

Worker captures and observations retain the array backing and recursively retain each element's nested collection backing before publishing output. Multiple fields may share one allocation. Empty arrays require no array payload allocation. Returning an indexed record retains its own dependencies through the existing record transport path.

Validation checks canonical descriptors, nonnegative lengths, checked byte counts, source ownership, payload bounds, element alignment, and nested numeric field descriptors/static shapes. Failed retention never publishes a partially copied array; acquired owners remain available for settlement. The retention loop polls cancellation every 256 elements. Group release and scheduler destruction do not invalidate an observed value while its observing invocation still owns the storage.

Array allocation, validation and generated element access use the same pinned macOS ARM64 target layout as task transport. This includes field alignment and tail padding; MLIR's unspecified default layout is not used to determine record allocation sizes. The descriptors, layouts and runtime hooks remain private compiler/runtime interfaces, outside the separate [C/C++ integration roadmap](../native-interop-roadmap.md).

Finite record layout limits still apply. Literal materialization also has an expanded storage budget (300,000 scalar/descriptor words by default), counting every repeated record element. Runtime count-by-element-size arithmetic is checked before allocation or traversal. Storage sharing and earlier reclamation remain optimization follow-ups.

## Qualification

Frontend and execution suites cover six numeric families, scalar-only and Boolean-only element layouts, nested collection fields, construction/index/length, branches and empty arrays, imported helpers, worker returns, callback captures, TaskError recovery, evaluation order and failed-build artifact preservation. Private task harnesses check exact raw values, shared nested backing, cached observations, source settlement, cancellation polling, rejected descriptors/static shapes, partial-retention failure and scheduler cleanup. All 103 focused checks passed on Apple M2 Max, including debug, optimized, ASan/UBSan and separate ThreadSanitizer variants. The new suites compiled 224 source programs and ran 464 worker harness invocations. Their success inputs contained 7,488 retained record cells; 1,728 failed-observation checks and 576 cancelled-retention checks required output buffers to remain unchanged. The CLI allocation sweeps ran 320 cases. All 640 tracked numerical artifacts and 17 numerical/storage source files remain unchanged; checked-ABI and invocation sources have intentional additions. This is focused qualification, not the full registered suite. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-record-arrays-2026-10-18.json) records scope and hashes.
