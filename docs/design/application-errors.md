# Local application-error channels

This document describes the initial native implementation of a single declared application-error family per function. It is the first implementation stage of the [accepted errors](errors.md) and [variant](variants.md) design. [Application-family unions and aliases](error-unions.md) are now implemented in a subsequent milestone. [Local composition with TaskError](mixed-error-channels.md) is implemented; [local CPU worker transport](worker-errors.md) also preserves application errors through `spawn`/`await` and group settlement.

```kalandra
error StatisticsError:
  EmptyInput
  TooFewSamples:
    required: int
    received: int

fn average(samples: [float]) -> float ! StatisticsError:
  count = length(samples)
  if count == 0:
    fail StatisticsError.EmptyInput
  if count < 2:
    fail StatisticsError.TooFewSamples {
      required: 2,
      received: count
    }
  reduceSum(tensor(samples)) / float(count)

fn adjustedAverage(samples: [float]) -> float ! StatisticsError:
  result = try average(samples)
  result + 10.0

fn main() -> float:
  adjustedAverage([1.0]) catch fn(error):
    match error:
      StatisticsError.EmptyInput: 0.0
      StatisticsError.TooFewSamples { received }: float(received)
```

This prints `1`. With three samples `[1.0, 2.0, 3.0]`, the successful result is `12` and the handler does not run.

## Declared values and channels

An `error` declaration has the same immutable variants and named payload fields as an enum. Its nominal identity additionally makes it suitable for an expected-error channel. Ordinary enums cannot be passed to `fail`. Errors are not thrown when constructed: they can be passed to ordinary functions, matched, or stored in supported records and arrays as data.

`T ! E` has one success value of type `T`, or one error of family `E`. Named fallible functions declare their error channel. Success types follow the existing inference rules; explicit success annotations supply the context needed by `fail` and early validation guards. A normal final expression produces success; `fail E.Variant` terminates the current function or handler with the error. No success payload is read or retained on that path. Payloads use the [supported CPU enum field types and ownership rules](native-enums.md).

`try` explicitly propagates an error. The receiving function must declare the same family. An expected-error result cannot be silently discarded or used as its success type. Different declarations remain distinct even when their variants or field layouts look identical. Named imports, aliases and re-exports preserve the original family identity.

## Early failure guards

A non-final statement may use `if` without `else` when its body ends in an explicit `fail`. This is the early validation form used above. The condition must be a completed Bool. Only the selected body executes; local bindings remain inside it.

A guard is not a value and cannot initialize a binding, be returned, or replace a general conditional expression. General `if` expressions still require an `else` branch. A guard currently requires a direct final `fail`; arbitrary non-returning control-flow analysis is not introduced here.

## Recovery and replacement

`catch fn(error): ...` infers its parameter as the operand's exact error family. Its result must have the original success type. A handler may return a fallback, rethrow the same value with `fail error`, or produce a different declared error family:

```kalandra
error PublicError:
  Unavailable

fn publicAverage(samples: [float]) -> float ! PublicError:
  average(samples) catch fn(error): fail PublicError.Unavailable
```

The handler replaces the handled channel; it does not nest results or wrap `StatisticsError` in `PublicError`. Success bypasses the handler and remains success. Handlers can now escape with multiple application families through the [local union support](error-unions.md); [local mixed application/TaskError channels](mixed-error-channels.md) are also implemented. Matches remain exhaustive unless an explicit final `_` supplies a policy for remaining variants.

Arithmetic, bounds and implicit allocation faults retain their execution-fault behavior. An application-error handler cannot swallow such a fault. Existing `TaskError` propagation and recovery remain available for execution failures at task boundaries.

## Lifetimes and execution boundaries

Application payloads use automatic managed ownership. Strings, numeric/Bool/String arrays, tensors, nested records/enums and record/enum arrays retain their backing when a typed result is transferred by the private runtime. Observations may outlive the originating task and scheduler. Failed retention must leave the output untouched and release partially acquired owners on settlement.

A worker may return `T ! E` directly or handle an application-fallible helper locally. [Local CPU worker transport](worker-errors.md) preserves application outcomes alongside execution failures as `E | TaskError`, including group propagation and settlement. No implicit conversion to `TaskError.Fault` is supplied.

An unhandled application error at the CLI prints its original `Family.Variant`, exits with status `89`, and settles managed backing. No success output is printed. This diagnostic does not expose an ordinal, host layout, or public C/C++ or network ABI.

## Private representation

The existing execution-failure prefix is unchanged. An application-fallible value adds a separate typed error aggregate to its success wrapper. Its discriminator distinguishes an active application error from success. Only the active arm's dependencies are retained; absent success tensors and unselected variant fields are not interpreted. Wrappers without application channels retain their existing layout.

This is a correctness baseline using the current tagged-aggregate implementation. Compact variants and result-carrier optimization remain follow-up work. Payload representation does not determine future distributed schemas or C/C++ contracts.

## Qualification

[Frontend tests](../../native/tests/ApplicationErrorTests.cpp) cover declared families, strict propagation and handler typing, guards, rethrows, channel replacement, ordinary error values, malformed metadata and strict channel coverage diagnostics. [Compiled programs](../../native/cmake/VerifyApplicationErrors.cmake) exercise all ten numeric widths, typed and nested payloads, modules/re-exports, guard execution, absent tensor success values, unhandled CLI errors, checked-fault separation and allocation cleanup.

The [private result harness](../../native/tests/ApplicationErrorRuntimeTests.cpp) checks exact payloads, shared cached observations, source/scheduler settlement, malformed tags/descriptors and untouched output on partial retention failure. Debug, optimized, ASan/UBSan and TSan variants share the same cases. The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-application-errors-2026-10-19.json) records sources, programs and regression scope.
