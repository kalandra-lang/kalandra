# Record and enum array fields

Immutable CPU records and enum variants can contain arrays of other records or enums. Use `[T]` with the exact nominal element type:

```kalandra
type Job:
  name: String
  values: [int]

enum Work:
  Idle
  Batch:
    jobs: [Job]

fn make() -> Work:
  Work.Batch { jobs: [Job { name: "sum", values: [20, 22] }] }

fn keep(work: Work) -> Work:
  work

fn main() -> int ! TaskError:
  work = try parallel:
    task = spawn keep(make())
    try await task
  match work:
    Work.Idle: 0
    Work.Batch { jobs }: jobs[0].values[0] + jobs[0].values[1]
```

This prints `42`. Empty fields obtain their element type from their declaration, for example `Work.Batch { jobs: [] }`. Identically shaped records or enum alternatives from different declarations are not interchangeable.

Fields support the existing array operations: indexing, `length`, `map`, `slice` and `concat`. Updating a record replaces the whole field with a new immutable array value; it does not modify the original array or its elements. Enum payloads remain accessible only through a matching case. An array element can itself contain array fields, allowing nested containers through named record/enum types. Direct nested-array types such as `[[Job]]` remain unsupported.

## Storage and lifetimes

An array field stores the existing private descriptor, not all of its elements inline. Constructing a record or enum with an existing record-array value stores that descriptor. Its immutable array allocation and each element's String/collection backing remain managed by the invocation. Repeated fields may share the same allocation. Record layout budgets count descriptor words, separately from the materialization budget for an array's elements.

CPU field boundaries validate array descriptors. Worker captures, results and observations recursively retain the array storage and its elements' backing before publication. Nested enum elements retain only their selected payloads. Empty arrays need no element allocation. An inactive enum field is neither validated nor retained as a live array.

The same rules hold for nested workers and repeated awaits. Observed values survive source invocation settlement, task-group release and scheduler destruction while their observing invocation still owns the retained storage. Failed retention leaves the destination output unchanged; settlement releases any partially acquired owners. Array traversal polls cancellation every 256 elements.

The compiler currently rejects recursive type dependencies, including `type Node: children: [Node]` and mutually recursive record/enum arrays. Array indirection makes the physical descriptor finite, but recursive construction, validation and ownership need an explicit design and bounded lowering before these types can be enabled. CPU record arrays do not introduce GPU records, arbitrary nested arrays, a public C/C++ layout, or distributed execution.

## Qualification

[Frontend checks](../../native/tests/RecordArrayFieldsTests.cpp) cover nominal typing, constructors, immutable updates, worker planning, descriptor layout accounting, independent dynamic lengths, recursive-type rejection and malformed field metadata. [Execution checks](../../native/cmake/VerifyRecordArrayFields.cmake) cover all ten CPU numeric widths inside array elements, nested records/enums, empty fields, String ownership, updates/maps/slices/concat, collections escaping a match, callback captures and TaskError recovery.

The existing [raw worker harness](../../native/tests/RecordArrayWorkerTests.cpp) also runs through a single-field record wrapper. It checks exact element bits and shared allocations, 0/1/257/1025-element arrays, source settlement, cached and surviving observations, cancellation polling, deadlines, admission limits, malformed outer/inner descriptors, fixed tensor shapes and unchanged output on partial-retention failure. Debug, optimized, ASan/UBSan and TSan variants use the same cases and allocation-failure sweeps.

The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-record-array-fields-2026-10-19.json) records the tested sources, generated programs and regression scope.
