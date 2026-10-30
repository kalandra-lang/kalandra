# Kalandra errors and effects proposal

Accepted errors and effects direction — 2026-10-14. The project owner approved typed expected errors, explicit propagation/handling, inferred effects, and the execution-fault classification below. This builds on the accepted [core](core.md), [numeric rules](numbers.md), and [automatic memory model](memory.md). It describes the fresh implementation, not behavior of Kalandra 0.13.

Expected failures should be visible in types and handled locally or propagated explicitly. Ordinary arithmetic should stay readable. The compiler should infer external effects without asking authors to decorate every function or write effect lists.

## Accepted decisions

- `T ! E` denotes a result containing either a success value of type T or an expected error of type E.
- A function with that return type returns success through its normal final expression; `fail` produces an error result.
- `try` propagates an expected error to the containing fallible function; `catch` supplies a handler with an explicit parameter.
- The compiler rejects discarding an unhandled expected error or treating its result as an ordinary success value.
- Dynamic checked-arithmetic, bounds, and implicit allocation failures are execution faults rather than expected-error unions attached to every ordinary expression.
- Effects such as file IO, external state, network access, nondeterminism, device execution, and native calls are inferred and tracked internally.
- Expected errors are values, not arbitrary thrown objects. Faults are controlled execution termination, not undefined behavior.

Error types, result types, and inference need to stay shared across the compiler, LSP, contract generation, and future task runtime.

## Expected errors

Start with named error types containing named variants:

```kalandra
error ReadError:
  NotFound
  PermissionDenied
  InvalidData
```

An error is an immutable value of its declared error type. Names are qualified, such as `ReadError.NotFound`. Payload-bearing variants and general pattern matching get a later focused proposal; this first example does not introduce message fields or an unconstrained exception object.

Assume a library function with this signature:

```kalandra
fn readSamples(path: String) -> [float] ! ReadError
```

The declaration is an interface example, not a function body. Its result is either an array or a ReadError. There is no success value on failure, and no sentinel empty array is substituted automatically.

Public named fallible functions spell their error type in the signature. Private implementation details and anonymous handlers may infer result/error types from context. The accepted [variants and error payloads design](variants.md) defines multiple error families, error-type aliases, named payload fields, and exhaustive match.

## Propagate with try

```kalandra
fn loadScore(path: String) -> float ! ReadError:
  samples = try readSamples(path)
  sum(samples)
```

Assume `sum` on these floating samples returns a float. On success, `samples` has type `[float]`. On ReadError, the function immediately returns that error and does not execute `sum`. The success result is wrapped by the fallible return type without requiring a separate `ok(...)` call.

`try` is an expression and binds to the following call/grouped expression. It does not throw. The containing function must permit the propagated error type. A nonfallible function cannot use `try` to silently acquire an undeclared error channel.

Bindings already created before propagation are cleaned up automatically according to the memory model. Cleanup does not roll back file writes, network effects, or other external operations performed before the error.

Invalid:

```kalandra
fn loadScore(path: String) -> float:
  samples = try readSamples(path)
  sum(samples)
```

Diagnose that ReadError cannot propagate through the nonfallible return signature. Suggest handling it or declaring `-> float ! ReadError`.

Also invalid:

```kalandra
samples = readSamples(path)
score = sum(samples)
```

`samples` has a result type, not an array type. Reject its use as sum's array argument and suggest `try` or `catch`. A result may be bound for later handling; leaving an expected error silently unhandled at the end of its use is diagnosed.

## Handle with catch

```kalandra
fn loadScoreOrZero(path: String) -> float:
  samples = readSamples(path) catch fn(error): [0.0]
  sum(samples)
```

On success, use the returned samples and do not invoke the handler. On failure, pass the ReadError to the handler and use its fallback array. Returning zero on every ReadError is an explicit application policy in this example, not built-in behavior.

The handler has an explicit parameter using the accepted anonymous-function syntax. Its parameter type is inferred from the error channel. It must return a value compatible with the success type. Block-bodied handlers use the same indentation rules as other functions.

`catch` binds less tightly than the call it handles. Require parentheses when combining result handling with a pipeline if the grouping is not obvious; a complete grammar table will specify this before parser implementation.

A nonfallible handler converts `T ! E` into T. If a handler itself returns `T ! OtherError`, the combined expression remains fallible with OtherError. The original E has been handled. Never introduce a nested result accidentally: flatten this handler composition deliberately, with a single success channel. The error type must remain known, not guessed from a runtime object.

Do not support an implicit blanket catch for surrounding code or a catch-all for execution faults in this first model.

## Produce an expected error

```kalandra
error ValidationError:
  EmptyInput

fn mean(values: [float]) -> float ! ValidationError:
  if values.length == 0:
    fail ValidationError.EmptyInput
  sum(values) / float(values.length)
```

`fail` terminates the current fallible function with a declared error value. It is not a normal arithmetic value and is not an exception thrown across unrelated code. This example introduces the conventional indented conditional spelling for illustration; the full control-flow grammar is a later specification step.

An expected validation failure does not turn into an implicit bounds or division fault. The error occurs before evaluating the division. Empty-input behavior is part of the API contract.

A failure returned from an anonymous function terminates that anonymous function, not its enclosing lexical function. If callbacks can return results, their callers need an explicitly defined result-aware API; ordinary `map` still maps exactly one element argument and does not automatically unwrap error results.

## Checked operations and execution faults

The accepted core can keep this signature:

```kalandra
fn double(value: int) -> int:
  value * 2
```

No `try` is required around every multiplication. If a dynamic input makes the multiplication overflow, execution produces an ArithmeticFault. Known invalid arithmetic remains a compile-time error. Overflow must not wrap or become undefined behavior in an optimized build.

| Situation                                     | Proposed classification  | Author-facing treatment                                              |
| --------------------------------------------- | ------------------------ | -------------------------------------------------------------------- |
| File missing or access denied                 | Expected error           | `try`, `catch`, or propagate the result value                        |
| Invalid external data                         | Expected error           | Validate and return a typed error                                    |
| Remote timeout or unavailable service         | Expected error           | Execution API supplies a declared error channel                      |
| Cancellation of an awaited operation          | Expected execution error | Task API supplies a declared cancellation result                     |
| Dynamic checked arithmetic/conversion failure | ArithmeticFault          | Stop the execution unit with diagnostics                             |
| Out-of-bounds access                          | BoundsFault              | Stop the execution unit with diagnostics                             |
| Failure of implicit managed allocation        | AllocationFault          | Stop the execution unit safely where the runtime can report/clean up |
| Native crash or device loss                   | Host/device failure      | Host-specific isolation and execution error mapping                  |

The execution unit is the program entry for a simple executable and a supervised task/request when the scheduler provides that boundary. The execution proposal must define unit failure, sibling cancellation, and root propagation before promising isolation between independent jobs. A native fault is not magically isolated merely because the source is functional.

Within a runtime-controlled unit, a fault ends the calculation, releases resources that can safely be released, and reports the operation and source location. It does not resume at the offending instruction. The initial `catch` cannot intercept a fault and pretend the operation returned a value. Process death or an operating-system OOM kill cannot guarantee language-level unwinding; a supervisor must use host-confirmed termination and resource accounting.

APIs for untrusted numeric input need an expected-error alternative. Propose explicitly named checked helpers, for example `checkedInt32(value)` returning `int32 ! ConversionError`, alongside the accepted ordinary `int32(value)` conversion whose invalid dynamic input is an ArithmeticFault. Similarly, `checkedAdd` can expose overflow as an expected result. Exact generic signatures follow the generics proposal.

This distinction is deliberate: ordinary operators assert their arithmetic preconditions; checked helpers model expected invalid input. A future explicit resource-reservation API may report a predictable capacity failure as an expected error, without making every record construction visibly fallible.

This classification is now accepted alongside the numeric/memory design: ordinary dynamic arithmetic failure is an execution fault, not recoverable through ordinary catch; checked helper APIs expose expected error results.

## Effects are inferred

A function's result/error type describes what it returns. Its effect summary describes what it may do externally. Those are different facts.

| Function or operation                                 | Compiler-visible property                                  |
| ----------------------------------------------------- | ---------------------------------------------------------- |
| Scalar arithmetic and immutable record transformation | No external effect; checked operations can fault           |
| Returning an expected error value                     | No external effect by itself                               |
| Reading/writing a file                                | File IO and external observation/mutation                  |
| Reading a clock or random source                      | Nondeterministic observation                               |
| Making a remote call                                  | Network/external service effect                            |
| Submitting device work                                | Device execution and relevant synchronization              |
| Calling a native function                             | Declared wrapper effects, or unknown effect if unspecified |

Authors do not write `@pure`, `@io`, or effect lists on ordinary declarations. The compiler combines known effects through calls and captures. Function values and module interfaces carry summaries internally, so calling through an anonymous function does not erase them.

Native or separately compiled interfaces must provide trusted compiler-visible summaries, or be treated conservatively as having unknown effects. An unknown function is never assumed pure or GPU-compatible. Boundary declarations may require metadata; that is not a decorator system for everyday functions.

The language distinguishes immutable values from pure computations. A function can call a service or write a file without mutating its arguments. Effects remain explicit through the operations being called and visible through diagnostics and editor information.

## What effect information enables

- Reject IO, arbitrary native callbacks, and unsupported effects inside a restricted GPU compute region.
- Preserve order of external operations rather than reordering them as arithmetic.
- Determine which computations can share immutable inputs safely while retaining their external-effect ordering constraints.
- Generate contract/runtime metadata appropriate to a remote function.
- Explain in the editor why a function is unsuitable for a particular execution context.

Effect inference does not automatically enable parallel execution, memoization, retries, or transactional rollback. A pure-looking numeric function may still fault, and resource-dependent allocation failure is not a reason to promise totality. Optimizations must preserve specified observable arithmetic faults and expected errors; allocation elimination can change whether resource exhaustion occurs because allocation strategy is compiler-selected.

Automatic retries are not the default for remote calls, even if the caller sees a timeout. A service might have performed an external effect before its response was lost. Idempotency and retry policies belong to the distributed-execution contract.

## Cleanup and failure propagation

Expected errors and execution faults must never publish partially initialized values. Managed dependencies are released according to their actual remaining uses. Cleanup of ordinary memory is automatic; explicit external-resource operations need a separate design.

Failures during device execution do not make buffers immediately reusable. Cancellation, error reporting, and device completion are different events. The task runner must retain resources through the required completion or confirmed termination boundary.

At a program entry that returns an expected error, the CLI reports it and exits nonzero. It does not silently ignore it. At a remote/task boundary, expected application errors remain typed contract values; faults and host failures become bounded execution diagnostics with a separately specified execution-error envelope. Do not expose arbitrary local pointers, stack objects, or unbounded native exception text.

## Validation before implementation

1. Success unwraps through `try`; the handler runs only for errors.
2. Incompatible propagation and use of a result as a success value are diagnosed.
3. Discarded fallible calls and abandoned unhandled results are diagnosed; explicit handler policy is respected.
4. Expected errors clean up managed data without rolling back earlier external effects.
5. Handler composition has one success channel and a known replacement error type.
6. Anonymous functions have their own failure boundary; ordinary map does not implicitly unwrap callback errors.
7. Checked arithmetic/bounds faults remain effective in optimized builds and cannot be swallowed by ordinary catch.
8. Checked numeric helpers expose recoverable errors for invalid external inputs.
9. Effect summaries survive higher-order and separately compiled calls; unknown native calls remain conservative.
10. Reordering, parallelization, and GPU admission respect effects and defined failure behavior.
11. Unit failure, cancellation, and device resource lifetimes are verified once the execution model is specified.

## Review and next step

`T ! E`, `try`, `catch fn(error): ...`, `fail`, inferred effects, and the distinction between expected errors and execution faults are accepted. Ordinary arithmetic faults stop an execution unit; explicitly checked numeric APIs return expected errors.

[Resources and task lifetimes](execution.md) are accepted: explicit parallel groups, task completion/cancellation, and library-scoped resource capabilities. [Variants and error payloads](variants.md) are accepted. [Tensor types and shapes](tensors.md) are accepted. [Placement and transfers](placement.md) are next.
