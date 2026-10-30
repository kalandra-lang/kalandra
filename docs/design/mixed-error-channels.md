# Local channels combining application errors and TaskError

The native CPU compiler now supports `ApplicationError | TaskError` in local function signatures, aliases, propagation and recovery. This extends [application-error unions](error-unions.md). Source-level spawning of application-fallible functions and propagation of application errors out of parallel groups remain the next implementation stage.

```kalandra
error InputError:
  Negative

type ComputeError = InputError | TaskError

fn divide(value: int) -> int:
  100 / value

fn calculate(value: int) -> int ! ComputeError:
  if value < 0:
    fail InputError.Negative
  try parallel:
    work = spawn divide(value)
    try await work

fn recover(value: int) -> int:
  calculate(value) catch fn(error):
    match error:
      InputError.Negative: 0
      TaskError.Fault { kind }:
        match kind:
          FaultKind.Arithmetic: 42
          _: 1
      TaskError.Cancelled: 2
      TaskError.DeadlineExceeded: 3
      TaskError.AdmissionRejected { reason }: length(reason)

fn main() -> [int]:
  [recover(-1), recover(0), recover(4)]
```

This prints `[0, 42, 25]`. The negative input is an application error. Division by zero faults inside the supervised child and becomes `TaskError.Fault` at the existing task boundary. An ordinary arithmetic fault in the calling execution unit still bypasses `catch`; adding TaskError to a signature does not automatically supervise every expression.

## Identity and propagation

Mixed channels use the same canonical family-set rules as application-only unions. Nested aliases flatten, duplicate members collapse and ordering has no semantic significance. `TaskError | TaskError` is `TaskError`, and aliases containing only that family retain the existing TaskError channel.

A `T ! TaskError` result may be propagated or returned into `T ! (E | TaskError)`. Application-only E or a smaller application union may likewise widen into a mixed channel which covers every family. A mixed result cannot pass through an application-only or TaskError-only signature unless the other families have been handled.

Catch handlers infer mixed replacement channels when their branches propagate different families. They may recover to an ordinary success value, replace an execution error with an application error, replace an application error with an explicitly called fallible operation, or rethrow the complete mixed value with `fail error`. There is one success channel; handler composition does not nest results.

[Whole-value match-arm narrowing](error-narrowing.md) gives the matched binding its selected or residual family set within each branch. `fail error` preserves that narrowed value and its payload, including original TaskError metadata. Outside those branches it still propagates the complete current set.

## TaskError patterns and fields

Use the original built-in names in a mixed handler:

- `TaskError.Fault { kind, message }` provides `FaultKind` and a bounded String diagnostic.
- `TaskError.Cancelled` and `TaskError.DeadlineExceeded` carry their existing execution identities.
- `TaskError.AdmissionRejected { reason }` provides a bounded String reason.

TaskError may be named by a transparent channel alias, imported and re-exported. Patterns remain qualified by `TaskError`; the alias does not rename its variants. A mixed match must cover all variants of all members, unless an explicit final `_` supplies a policy for the remaining cases. Unknown fields and fields belonging to another TaskError variant are rejected.

Diagnostic text is materialized through the same managed String path as existing TaskError handlers. An allocation failure during that materialization remains an execution fault in the handler; the handler cannot swallow it as another expected error.

## Representation and ownership

The existing runtime `Failure` and scheduler failure classifications are unchanged. A mixed result preserves the original task failure kind, fault code and source line/column. Task errors are never relabeled as application errors or wrapped in `TaskError.Fault` merely to combine channels.

The compiler represents TaskError as a private canonical member of its tagged error-family model. Each selected task variant carries its original bounded failure metadata. Widening normalizes a legacy expected-error prefix into that member; handlers project the selected payload and recover its existing fields. Application variants keep their own nominal identity and managed payloads.

Only the active variant is retained. Task-only outcomes do not retain inactive application strings or arrays, and application outcomes do not interpret inactive task metadata. A TaskError may itself be ordinary success data; that success value remains separate from the function's expected-error channel. An expected failure must not validate or read its absent success value, even when both have TaskError as their type. Malformed active task discriminators are rejected before publishing observations. Failed retention leaves output untouched, and surviving observations remain valid after the source task, group and scheduler have settled.

These aggregates and generated discriminators are private implementation details, not network schemas or a public C/C++ ABI. Compact error storage remains optimization work.

## CLI reporting

An unhandled application error still prints its original `Family.Variant` and exits with status `89`. An unhandled TaskError still reports its task classification and original source location and exits with status `88`. A mixed alias does not change either policy. Execution faults retain their existing diagnostics and exit codes.

## Local worker execution

The example above spawns an ordinary function whose checked fault is handled by the scheduler. [Local CPU worker transport](worker-errors.md) also supports spawning a function returning application errors.

`spawn applicationFallible()` preserves the child's channel alongside TaskError. Application errors may be handled inside the group without cancelling siblings, or propagated through group cancellation and settlement while retaining their original payload.

No distributed execution, GPU error transport, public foreign interface or change to sibling-cancellation policy is introduced here. [Error aliases in record/enum payload fields](error-payload-fields.md) are supported as completed data. [Whole-value match-arm narrowing](error-narrowing.md) is also supported.

## Qualification

[Frontend checks](../../native/tests/MixedErrorTests.cpp) cover canonical TaskError aliases, subset propagation, exhaustive mixed handlers, exact field types, rethrows, replacements, rejected family loss, strict worker channel coverage and malformed metadata. [Compiled cases](../../native/cmake/VerifyMixedErrors.cmake) cover all ten numeric widths, local legacy workers, real arithmetic/bounds/shape/allocation task faults, admission rejection, shape refinement, rethrows, channel replacement, imports and CLI classification/source-location preservation.

A [source regression](../../native/tests/fixtures/mixed-task-error-value.k) verifies a worker whose success type is TaskError but which returns an expected failure, preserving its original arithmetic classification. The [private result harness](../../native/tests/MixedErrorRuntimeTests.cpp) checks all four task variants, original fault codes/locations, active managed application backing, shared observations, inactive-arm isolation, malformed task metadata, partial-retention failure and scheduler teardown. Debug, optimized, ASan/UBSan and TSan qualification is recorded in the [evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-mixed-errors-2026-10-19.json).
