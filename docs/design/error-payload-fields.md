# Error aliases in CPU record and variant fields

Error aliases can be used as completed data in record fields, enum/error payloads and supported record-array fields. They preserve the original error families and their typed payloads. An alias may be declared after its users or imported through a re-export.

```kalandra
type Report:
  cause: Failure

type Failure = ReadError | ValidationError

error ReadError:
  Missing:
    name: String

error ValidationError:
  TooSmall:
    minimum: int

fn reject() -> Report ! Failure:
  fail ReadError.Missing { name: "missing" }

fn main() -> int:
  report = reject() catch fn(error): Report { cause: error }
  match report.cause:
    ReadError.Missing { name }: length(name)
    ValidationError.TooSmall { minimum }: minimum
```

This prints `7`. The handler stores the original error value; the later match still uses `ReadError.Missing` and reads its original String field.

## Errors as data

A field such as `cause: Failure` stores a completed error value. It is distinct from `Report ! Failure`, which is a result with a success or error outcome. A pending fallible result cannot be placed directly into the field without handling it.

Aliases remain transparent. `type ReadFailure = ReadError` has the same identity as ReadError. A union alias represents its complete canonical family set. Ordinary data fields require that exact set; this feature does not add implicit conversions between different error-value types. Expected-error channels retain their existing subset propagation rules.

Records and variants may nest these values, retain arrays of them and wrap them in another application error:

```kalandra
error RequestError:
  Rejected:
    cause: Failure

type Reports:
  causes: [Failure]
```

Built-in TaskError can also be ordinary field data, directly or through an alias:

```kalandra
type SavedTaskError:
  cause: TaskError

type ExecutionFailure = TaskError
```

Its four variants, original fault code and source location remain intact. Storing a TaskError does not make the containing record a failed task or create an expected-error channel. A mixed alias such as `ReadError | TaskError` works as a tagged field value as well.

## Ownership and validation

The existing immutable storage model applies. Nested Strings, arrays, tensors and records retain their backing across helper calls, worker captures/results, repeated awaits and parallel settlement. Retention examines only the selected variant; absent fields are not interpreted.

Field resolution reserves canonical error-union identities before copying completed member payloads. This preserves declaration-order independence and permits finite recursive record/enum dependencies through aliases or arrays. Pure alias cycles and types without a finite construction remain rejected. [Recursive storage](native-recursive-storage.md#completed-local-cpu-source-integration) gives forwarded nominal error fields a consistent managed representation.

Both synchronous native calls and worker copies validate TaskError field tags. Record validation also runs for records without heap-backed fields. Malformed TaskError data cannot become a valid observation merely because its wrapper has no owned storage. Failed private result copies leave their destination untouched.

This is CPU support. It does not add general untagged unions, GPU error transport, distributed schemas or a public C/C++ ABI. [Whole-value narrowing inside match arms](error-narrowing.md) is supported; field assignments continue to require their exact canonical types.

## Qualification

[Frontend tests](../../native/tests/ErrorPayloadFieldsTests.cpp) cover forward references, transparent and mixed aliases, nested fields, TaskError data, exact field typing, dependency cycles and malformed typed metadata. [Compiled programs](../../native/cmake/VerifyErrorPayloadFields.cmake) cover all ten numeric widths, original application/TaskError identities, enum branches, arrays, wrapper errors, immutable updates, re-exports, worker captures/results and allocation cleanup.

The [private runtime harness](../../native/tests/ErrorPayloadFieldsRuntimeTests.cpp) checks all four TaskError kinds and their metadata, rejected malformed tags, unchanged output after failed result copies, cached observations and scheduler teardown. Its synchronous bridge is test-only and does not define a foreign interface. Debug, optimized, ASan/UBSan and TSan results are recorded in the [qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-error-payload-fields-2026-10-19.json).
