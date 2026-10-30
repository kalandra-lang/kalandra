# Error-family narrowing in match branches

Matching a named error-union value narrows that binding inside each branch. You can pass the whole value to a helper expecting its selected family, store it in a field of that type, or rethrow it with `fail error` without reconstructing its payload.

```kalandra
error ReadError:
  Missing:
    name: String
  Unavailable

error ValidationError:
  TooSmall:
    minimum: int

type Failure = ReadError | ValidationError

fn operation() -> int ! Failure:
  fail ReadError.Missing { name: "missing" }

fn readOnly() -> int ! ReadError:
  operation() catch fn(error):
    match error:
      ReadError.Missing: fail error
      ReadError.Unavailable: fail error
      ValidationError.TooSmall: 0

fn main() -> int:
  readOnly() catch fn(error):
    match error:
      ReadError.Missing { name }: length(name)
      ReadError.Unavailable: 0
```

This prints `7`. Both read-error branches rethrow the original ReadError value. Recovering the validation family removes it from `readOnly`'s output channel. Payload Strings, arrays and nested records retain their existing ownership.

## Wildcards keep the remaining families

A wildcard narrows the binding to the families that still have unmatched variants. For a `ReadError | ValidationError | TaskError` subject, handling every ReadError variant first leaves `ValidationError | TaskError` in the wildcard. Nested matches can narrow that remainder again.

Removing one variant does not necessarily remove its family. If `ReadError.Missing` was handled but `ReadError.Unavailable` was not, ReadError remains possible in the wildcard. The compiler will reject a declaration that silently drops it.

Narrowing tracks families, not new types for individual variants. In a `ReadError.Missing` branch the whole binding has type ReadError. Payload fields still use pattern bindings such as `{ name }`; matching the whole narrowed value again still requires exhaustive coverage of its family's variants.

Mixed TaskError branches expose TaskError as ordinary completed data. Rethrowing it preserves its original kind, fault code and source location. Catching application families can therefore leave a TaskError-only output channel, and catching execution errors can leave an application-only channel.

## Scope and snapshots

Narrowing applies to the matched immutable variable within its branch. Parameters and handler variables follow the same rule. Nested helpers and synchronous map callbacks can consume or capture the narrowed value.

Other bindings keep their existing types:

```kalandra
original = error
match error:
  ReadError.Missing:
    handleReadError(error)
  _:
    handleOther(error)
```

`original` still denotes the full union. Outside the match, `error` also keeps its original type. A pattern binding with the same name shadows it normally. This feature does not discharge an unrelated pending result or allow a child task to escape its group.

For a field expression, bind the value first if you need to refer to its narrowed form: `cause = report.cause`, then `match cause`. Matching `report.cause` directly does not introduce a new named binding.

Ordinary data fields and helper parameters retain their exact canonical error-value types. A narrowed ReadError value is not implicitly converted back into a larger union for those uses; keep a snapshot of the original union when needed. Expected-error channels still support their existing subset propagation and widening rules.

## Representation and checks

A typed projection remaps the selected variant into the narrower family's representation. It copies the selected payload and preserves managed backing; it does not stringify errors, copy their underlying arrays or alter their identity. The compiler indexes original family/variant identities when constructing the projection.

Synchronous lowering uses this checked operation. CPU continuation planning now lowers it directly in the task frame, including recursive task components; preserved-call planning retains its kernel path. A tag outside the destination families causes a checked native fault. TaskError carrier tags must also agree with their selected failure metadata; checking only that the embedded kind is within the four-kind range is insufficient. Inactive payloads remain ignored.

Typed-IR validation rejects invalid subjects, non-subset destinations, self references and malformed result types. Projections are introduced only inside proven source match branches. They do not provide a source-level unchecked cast or define a network/public C/C++ representation.

This milestone is CPU-only. GPU error transport and distributed execution remain separate work.

## Qualification

[Frontend checks](../../native/tests/ErrorNarrowingTests.cpp) cover selected and residual families, nested matches, helper calls, binding scope, shadowing, snapshots, expected-result obligations and malformed projection metadata. [Compiled programs](../../native/cmake/VerifyErrorNarrowing.cmake) cover all ten numeric widths, managed payloads, worker rethrows, synchronous callbacks/captures, records, aliases/re-exports and CLI error identity.

The [private runtime harness](../../native/tests/ErrorNarrowingRuntimeTests.cpp) verifies all four TaskError kinds with original metadata, active-tag consistency, rejected corrupt tags, unchanged output on failed checked calls and inactive-payload isolation. Debug, optimized, ASan/UBSan, TSan and allocation-cleanup results are recorded in the [qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-error-narrowing-2026-10-19.json).

[Recursive mixed-channel component tests](native-recursive-storage.md#mixed-recursive-error-channels) also qualify direct continuation narrowing, residual union tag remapping, retained recursive/callable payloads and exact TaskError details across scheduler lifetimes. Recursive source admission remains separate.
