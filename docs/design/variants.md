# Kalandra variants and error payloads proposal

Accepted variants and error payload direction — 2026-10-14. The project owner approved the decisions below. This extends the accepted [core](core.md), [errors/effects](errors.md), and [task lifetimes](execution.md). The goal is to describe a value with several possible forms, attach typed details to failures, and handle those forms without dynamic error objects or hidden fields.

These examples describe the native design; they are not supported by the released Kalandra 0.13 compiler. The native branch now implements the [first CPU enum subset](native-enums.md): scalar and collection payloads, nested records/enums, exhaustive matching and local worker transport. Record/enum arrays are also supported as payload fields. [Local user-defined error channels](application-errors.md) are now implemented. [Local application-family unions and aliases](error-unions.md) are implemented. [Local mixed application/TaskError channels](mixed-error-channels.md) are also implemented. [Local CPU workers](worker-errors.md) preserve application errors through observation and settlement. [Error alias payload fields](error-payload-fields.md) are also supported. Recursive type dependencies remain ahead.

## Accepted decisions

- `enum` declares a named sum type: a value has exactly one declared variant.
- A variant may have named fields using the accepted record-field syntax.
- `error` uses the same variant/field model but declares an error type suitable for an error channel.
- Construct payload-bearing variants with the existing named-record style: `Type.Variant { field: value }`.
- `match` selects a variant, binds its fields, and produces a value. Matches are exhaustive unless an explicit fallback is supplied.
- `|` combines known error types in an error channel; a type alias can name the combination.
- Task application errors remain their original typed values. TaskError describes execution failures rather than wrapping every application error in an untyped payload.

This proposal does not introduce generics, inheritance, arbitrary runtime type inspection, or null. Optional-value convenience types can later use this same variant mechanism once generics are designed.

## A value with two forms

```kalandra
enum Reading:
  Missing
  Value:
    value: float

missing = Reading.Missing
available = Reading.Value { value: 21.5 }
```

A Reading is either Missing or Value. Only Value carries a float field. All fields are immutable and use ordinary automatic memory management.

An enum is not restricted to numeric constants. No user-visible ordinal or memory representation is implied by declaration order.

Named payload fields reuse the record construction model. A misspelled, repeated, missing, or incorrectly typed field is diagnosed. A payload-free variant is a value; it does not need empty braces or parentheses.

## Match a variant

```kalandra
fn valueOrZero(reading: Reading) -> float:
  match reading:
    Reading.Missing: 0.0
    Reading.Value { value }: value
```

This returns 0.0 for Missing and the payload for Value. The subject is evaluated once. Only the selected branch runs. Each branch is an expression or indented block whose final expression supplies its result.

`{ value }` binds the payload field to a local name in that branch. To choose another local name, write `{ value: measurement }`. The field binder is a pattern, not an expression that reads an uninitialized variable. Bindings follow the accepted lexical-scope/shadowing rules; duplicate local bindings within one pattern are rejected.

Pattern fields may be omitted when unused; omitted fields are not copied or initialized. Unknown or repeated pattern fields are errors. This is different from construction, which requires all fields.

All returning branches must have one common result type. Expected context may type literals as usual; matching does not introduce a new implicit numeric promotion rule. A branch ending in `fail` has no success value and does not force the success type to become an error union.

Invalid:

```kalandra
fn valueOrZero(reading: Reading) -> float:
  match reading:
    Reading.Value { value }: value
```

Diagnose the missing Reading.Missing branch. Duplicate or unreachable variant branches are also diagnosed. The first matching branch is not a way to hide a duplicate case.

An explicit fallback uses `_` as the final branch:

```kalandra
match reading:
  Reading.Value { value }: value
  _: 0.0
```

The fallback deliberately accepts every remaining variant. Future additions to the enum will therefore use that policy rather than force this match to change. Do not insert a fallback automatically. A fallback after all variants have already been covered is redundant and diagnosed under the same unreachable-branch rule.

`enum`, `error`, and `match` are recognized in their syntactic positions; an error-handler parameter named `error` remains valid. The complete grammar must preserve this contextual-keyword rule.

This first match design covers named variants and field binding. Guards, arbitrary value patterns, and nested destructuring are later extensions; no side effects run while selecting a variant.

## Errors carry the details callers need

```kalandra
error ReadError:
  NotFound:
    path: String
  PermissionDenied:
    path: String
  InvalidData:
    line: uint
    reason: String
```

Construct and return an expected error using the accepted fail operation:

```kalandra
fn unavailable(path: String) -> [float] ! ReadError:
  fail ReadError.NotFound { path: path }
```

The error is a typed immutable value. It has a path field because its selected variant declares one, not because all errors have a global optional message/path object.

An error value may be passed to an ordinary function and inspected by match:

```kalandra
fn context(error: ReadError) -> String:
  match error:
    ReadError.NotFound { path }: path
    ReadError.PermissionDenied { path }: path
    ReadError.InvalidData { reason }: reason
```

Each branch binds only fields available in that variant. Accessing `error.path` before narrowing is invalid because InvalidData has no path field. Even if several variants share a field spelling, no implicit common-field interface is introduced by this proposal.

`error` is a restricted variant declaration for expected failures. `fail Reading.Missing` is invalid because Reading is not an error type. Error values can still be ordinary payload data when an API explicitly declares that type; being an error type does not cause a value to throw when constructed or passed.

## Catch and match work together

Assume `readSamples(path)` returns `[float] ! ReadError`:

```kalandra
samples = readSamples(path) catch fn(error):
  match error:
    ReadError.NotFound { path }: [0.0]
    ReadError.PermissionDenied { path }: [0.0]
    ReadError.InvalidData { line, reason }: [0.0]
```

Every error case is handled with an intentional fallback in this example. A real application can inspect the bound details, report them through an effectful operation, or propagate selected errors with fail from a fallible handler. The accepted catch composition rule preserves a single success channel and the handler's remaining error channel.

A handler may also re-propagate its entire error value, such as `fail error`. Matching only borrows/reads immutable data; it does not consume the original error or destroy fields not bound in the pattern.

No arbitrary thrown JavaScript object, runtime exception class, or string parsing is needed to distinguish these cases.

## Combine known error families

```kalandra
type LoadError = ReadError | TaskError
```

This alias denotes either error family. It does not introduce a new wrapper or replace their named identities. Variants remain qualified by their original error type.

Use the alias in a function signature:

```kalandra
fn loadTotal(path: String) -> float ! LoadError:
  try parallel:
    work = spawn readSamples(path)
    samples = try await work
    sum(samples)
```

A read error remains a ReadError with its original fields. A scheduling, cancellation, or child execution failure remains a TaskError. The caller can handle both in one exhaustive match. This single-task example illustrates type composition, not a reason to dispatch small work.

For a child returning `T ! E`, await produces `T ! (E | TaskError)`. A nonfallible child still produces `T ! TaskError` on await. A parallel group's error channel includes unhandled expected errors from its body and the execution errors the group can produce.

Parentheses make the error-channel grouping explicit when no alias is used. `T ! (E | F)` has one success type T. It is not `(T ! E) | F` and not a nested error result.

Initially, `|` combines error types only. Ordinary alternatives use an enum declaration. General untagged unions and inferred structural unions are not introduced merely because error composition needs this notation.

## Propagation and narrowing

- Propagating E into `E | F` is valid and preserves the original value.
- Propagating `E | F` into E requires handling F first; otherwise it is a type error.
- Nested aliases flatten and duplicate members collapse. `E | E` is E; declaration/order of union members has no semantic significance.
- An alias is transparent for propagation, matching, and contracts. It does not create inheritance or a new error tag.
- A match over an error union must cover every variant of every member type unless it has an explicit fallback.
- In a selected branch, the compiler knows the exact variant and its field types. A payload-free variant can be matched by its qualified name alone.
- Qualified identities disambiguate two error families that both have a variant named NotFound.

The compiler may represent an error union efficiently, but it cannot infer a variant by guessing from payload field names or a memory layout. It must preserve the declared type/variant identity.

## TaskError payloads

Use the following small built-in execution-error family as the starting design. Its transport encoding remains a later schema decision:

```kalandra
enum FaultKind:
  Arithmetic
  Bounds
  Shape
  Allocation
  Native

error TaskError:
  Cancelled
  DeadlineExceeded
  AdmissionRejected:
    reason: String
  Fault:
    kind: FaultKind
    message: String
```

The runtime bounds and sanitizes diagnostic text. Source location/trace metadata can accompany a failure report without becoming an arbitrary payload object or the stable discriminator for handling errors. Unsupported device/transport operations need their own declared error variants/types in the later placement/remote proposal; they are not silently converted to one generic message case.

A checked fault inside a supervised child produces TaskError.Fault at the task boundary. Its original operation does not resume. The owning parallel group follows the accepted failure/cancellation policy; an outer handler can inspect the settled group's error. Application ReadError values are not wrapped in TaskError.Fault.

This design avoids requiring a generic `TaskError[E]` wrapper just to preserve application errors: `E | TaskError` keeps both families typed. Task handles may carry this information internally while their user-facing type syntax remains part of the future generics proposal.

## Memory, effects, and boundaries

Variant construction and matching have no external effect by themselves. Fields use the existing automatic lifetime model. Resource capabilities cannot escape their scopes merely by being placed inside a variant or error field; the same lifetime restrictions still apply.

Not every locally valid payload is a valid network payload. Remote contracts require concrete serializable field types and declared type/variant identity. A closure, local task handle, raw pointer, or scoped file capability cannot cross a boundary because it is hidden inside an error.

Contracts identify types and variants by an explicit schema mapping rather than source declaration ordinals or host memory layout. Changing field types, introducing new variants, and handling version skew need compatibility rules. For the initial strict model, unknown variants or mismatched schemas are contract errors; do not reinterpret them as arbitrary existing cases. Forward-compatible extension rules require a later versioning decision.

Generated clients represent the same alternatives with their host language's tagged-value mechanism and exhaustive handling where available. Preserve integer widths and error payload validation; do not serialize errors as unchecked string dictionaries.

Compiler diagnostics, LSP completion, and generated docs should list the available cases and the fields in the current branch. This is where static typing should reduce the author's need to memorize error conventions.

## Validation before implementation

1. Construct every payload-free and payload-bearing variant with checked field names/types.
2. Diagnose missing, duplicated, unknown, or wrongly typed construction fields.
3. Match a subject once, execute only the selected branch, and bind fields within that branch.
4. Diagnose missing, duplicate, unreachable, and ill-typed match branches.
5. Preserve immutable fields and the original value across matching and capture.
6. Propagate error subsets, reject missing family coverage, and flatten aliases without dynamic fallbacks.
7. Preserve application errors and TaskError independently through await/parallel/catch.
8. Reject resource or nonserializable-value escapes inside nested payloads.
9. Preserve type/variant identities and numeric payloads across valid contracts; reject incompatible schemas.
10. Expose precise variant fields in diagnostics and editor information.

## Review and next step

Enum/error field syntax, named payload construction, exhaustive `match`, explicit fallback behavior, and error-family unions are accepted additions to the core.

[Tensor types and shapes](tensors.md) are accepted, including Shape in FaultKind. Review [placement, transfers, and bulk tasks](placement.md) next. Keep scalar language values, tensor buffers, and execution capabilities distinct while giving CPU/GPU/remote computation one coherent application model.
