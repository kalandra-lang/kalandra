# Anonymous error declarations

Implemented native CPU subset — 2026-10-19. Anonymous functions can declare the expected errors from their invocation directly, for both expression and indented bodies. This extends [managed closures](native-closures.md) and [anonymous statement suites](native-block-closures.md), using the existing [expected-error rules](errors.md).

```kalandra
error InputError:
  Invalid:
    value: int

fn main() -> int:
  offset: int = 42
  validate = fn(value: int) -> int ! InputError:
    if value < 0:
      fail InputError.Invalid {value: offset}
    value + offset
  validate(-1) catch fn(error):
    match error:
      InputError.Invalid {value}: value
```

Creating `validate` captures `offset`; it does not run the body or produce an expected error. Calling it returns `int ! InputError`. The [standalone fixture](fixtures/anonymous-errors.k) returns `42`.

## Inference and obligations

An expected callable signature can still supply parameter and result types. Otherwise annotate parameters. A direct error declaration can accompany an inferred success type:

```kalandra
// checked is separately declared as fn(int) -> int ! InputError.
validate = fn(value: int) ! InputError:
  adjusted = try checked(value)
  adjusted + 1
```

The declared channel is available while checking `try`, before the success type has been inferred. An unconstrained body containing only `fail` cannot establish its success type: provide `-> int` or an expected function signature. No dynamic or default success type is invented.

Every expected result must be handled, propagated or returned. The declared error set must cover each propagated or returned family. An always-successful body still retains its declared invocation channel: it cannot be assigned to an infallible function type. Aliases and reordered/duplicate union members use canonical error sets, while function-value assignments retain exact signature matching.

A direct declaration determines the channel even when an expected function type is present. A different expected channel does not silently override it; incompatible assignment is rejected. Built-in `TaskError` and application-error unions follow the existing distinction between expected results and execution faults/cancellation.

## Factory and returned-function channels

The error suffix belongs to the innermost ungrouped function type. This factory is infallible and returns a fallible function:

```kalandra
factory = fn() -> fn(int) -> int ! InputError: checked
```

Group a returned function type before declaring a separate factory error:

```kalandra
factory = fn(blocked: Bool) -> (fn(int) -> int ! InputError) ! BuildError:
  if blocked:
    fail BuildError.Disabled
  fn(value: int) -> int ! InputError: checked(value)
```

These illustrations assume separately declared `checked` and `BuildError`. Calling the factory requires handling `BuildError`; invoking its returned function requires handling `InputError`. Ambiguous repeated ungrouped suffixes are rejected, as for named factories. Multiple families use `! (FirstError | SecondError)`.

## Calls, ownership and limits

Direct declarations work with captured values, higher-order parameters/results, returned factories, runtime selection and worker-local invocation. Strings, nested records, arrays and tensors retain the existing managed ownership and checked shape rules. Imported declarations resolve their error families in their defining module.

Sequential `map` still requires completed elements. A directly annotated fallible callback is rejected even if its body always succeeds. Bind an infallible wrapper that handles the errors before mapping. This applies to both inline and bound callbacks.

No runtime representation changes are needed: existing code/environment pairs and synchronous checked outcome entries carry these signatures. [Function-valued record and enum fields](native-callable-fields.md) now preserve these signatures. [Arrays of functions](native-function-arrays.md) now also preserve these signatures. Worker transport, indirect spawn and resumable function values remain separate work. Indented callbacks inside open call/collection delimiters remain subject to the [existing layout restriction](native-block-closures.md#calls-maps-and-workers). Public ABI and distributed execution are not qualified by this increment.

## Qualification

All 60 selected checks passed on Apple M2 Max. New execution suites compile 140 programs across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. Relevant existing frontend, closure, function-value, collection, map, module, control-flow and worker/error regressions pass in their selected debug and ASan/UBSan modes.

Each new frontend suite checks 24 accepted and 19 rejected programs, plus header grouping, separate factory/returned-function spans, declared-channel retention, imported diagnostic origins and token budgets. Execution coverage includes all ten scalar widths, success-type inference, unions/aliases, returned factories, nested owned payloads, tensors, worker-local calls, repeated awaits and imported family identity.

Private synchronous probes exercise 72 allocation cases across successful, expected-error and arithmetic-fault outcomes, including stopped/failed contexts and retained payloads after settlement. CLI sweeps run 960 allocation-failure processes; successful executions print the exact result, while failed executions publish no partial output and balance cleanup. Task ownership probes run with pools of one and four threads; owned worker results also run with one, two and four. Rejected declarations preserve existing executable and IR files.

The [qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-anonymous-errors-2026-10-19.json) records selected tests, per-variant counts and frozen source hashes. This qualifies the implemented macOS ARM64 subset, not callable aggregate storage, worker function transport, a public ABI, performance improvements or other platforms.
