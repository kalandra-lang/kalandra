# Anonymous function bodies

Implemented native subset — 2026-10-19. A [managed CPU closure](native-closures.md) may now have an indented body containing immutable bindings and a final result expression. The compact expression form remains supported.

```kalandra
fn makeAdder(offset: int) -> fn(int) -> int:
  fn(value):
    adjusted = value + offset
    adjusted * 2

export fn main() -> int:
  addOne = makeAdder(1)
  addOne(20)
```

The [complete fixture](fixtures/block-closures.k) returns `42`. The expected function signature supplies `value`'s type. Without an expected signature, annotate anonymous parameters, just as for expression closures.

## Bindings, results and captures

Statements execute in source order when the function is called. Creating the closure does not execute its body. The last statement must be a result expression; an empty body or a body ending in a binding is rejected. Local annotations and the declared/expected result type use the existing exact type rules.

Bindings are immutable. A parameter cannot be rebound, and duplicate bindings in the same body are rejected. A local may shadow an outer binding; its initializer is checked before introducing the new name. Body locals do not escape their lexical scope. An inner closure may capture a completed local and keep it valid after the outer invocation returns, using the same managed environment rules as other captures.

`if`/`else`, exhaustive `match`, `catch` handlers and early failure guards use the existing control-flow rules. An ordinary `if` still needs `else`; the no-`else` form remains a non-result guard ending in `fail`.

## Expected errors

Use an expected callable signature to declare a block closure's error channel:

```kalandra
fn makeValidator(offset: int) -> fn(int) -> int ! InputError:
  fn(value):
    if value < 0:
      fail InputError.Invalid {value: offset}
    adjusted = try checked(value, offset)
    adjusted + 1
```

This is an illustration with separately declared `InputError` and `checked`. The error suffix belongs to the returned function. The compiler preserves that channel even when the last expression succeeds. A guard failure exits the body before subsequent bindings execute. Each expected result must be handled, propagated or returned; binding an error-producing call and then ignoring it is rejected.

[Direct anonymous error declarations](anonymous-errors.md), such as `fn(value: int) -> int ! InputError:`, are now supported too. Expected signatures remain an alternative.

## Calls, maps and workers

Block closures use the existing function-value calling convention. They work with higher-order parameters/results, pipelines, runtime selection, sequential maps and worker-local creation/invocation. A callback can recover its own errors and return a completed element. Empty maps do not execute the body. Closure arguments/results across worker boundaries, indirect spawn and task/device-coordinating callable targets remain rejected.

The lexer currently suppresses indentation and newlines inside parentheses and brackets. Bind an indented callback before passing it to a call:

```kalandra
transform = fn(value: int):
  adjusted = value + 1
  adjusted * 2
result = map([1, 2, 3], transform)
```

[Indented bodies directly inside `map(..., fn(...): ...)`](native-delimited-closures.md) are now supported. Close the call on a dedented line after the body. Calls and collections nested inside that body keep their ordinary multiline continuation rules.

## Lowering and lifetime

Anonymous bodies now use the same ordered statement suites as other language bodies. Lifted functions retain every local binding and the final result, while closure construction captures only referenced lexical values. Pure bodies continue through MLIR; error-control bodies use the existing synchronous checked entries. No new runtime representation or scheduler behavior is introduced.

The existing environment, numeric descriptor copying and invocation-settlement lifetime rules remain in force. This increment adds no allocator annotations, implicit error aggregation or function transport. The [closure optimization work](../roadmap.md#optimization) remains separate.

## Qualification

All 56 selected checks passed on Apple M2 Max. The new execution suites compile 156 programs across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. Existing relevant parser, semantic, function-value, collection, closure, map, module, control-flow, application-error and worker-error regressions pass in their selected debug and ASan/UBSan modes.

Frontend suites check 28 valid programs and 16 rejected programs per variant, plus ordered AST statements and spans, dedented following calls, `P007`/`P035` diagnostics, parser budgets, CRLF/EOF termination and direct-map expected-error obligations. Runtime coverage includes all ten numeric scalar widths, nested captures, typed bindings, guards, matches, error propagation/recovery, imported creators, empty maps and owned worker results.

Private probes exercise 256 returned-closure calls after creator exit, 52 context allocation probes, 32 creation-allocation probes and 12 body-fault allocation probes. Arithmetic faults preserve their original body location. CLI sweeps run 960 allocation-failure processes, checking exact successful output and balanced failed settlement without partial output. Worker results are additionally exercised with pools of one, two and four threads, including repeated awaits. Invalid block bindings preserve existing executable and IR artifacts.

The [qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-block-closures-2026-10-19.json) records the selected tests, per-variant probes and frozen sources. This qualifies the implemented macOS ARM64 body subset, not delimiter-aware suites, direct anonymous error annotations, callable transport, a public ABI or other platforms.
