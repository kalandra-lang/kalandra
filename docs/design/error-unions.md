# Local application-error unions and aliases

The native CPU compiler supports combining declared application-error families in one expected-error channel. This implements the local union/alias portion of the accepted [variant design](variants.md), building on [local application errors](application-errors.md). [Local TaskError composition](mixed-error-channels.md) is now implemented. [Local CPU worker transport](worker-errors.md) preserves these channels across worker boundaries.

```kalandra
error ReadError:
  Missing

error ValidationError:
  TooSmall:
    minimum: int

type LoadError = ReadError | ValidationError

fn read(available: Bool) -> int ! ReadError:
  if not available:
    fail ReadError.Missing
  15

fn load(available: Bool) -> int ! LoadError:
  value = try read(available)
  if value < 10:
    fail ValidationError.TooSmall { minimum: 10 }
  value

fn main() -> int:
  load(true) catch fn(error):
    match error:
      ReadError.Missing: 0
      ValidationError.TooSmall { minimum }: minimum
```

This prints `15`. `load(false)` returns `ReadError.Missing`; the handler returns `0`.

## One result, several known error families

`int ! (ReadError | ValidationError)` is an alternative spelling of the aliased channel above. Parenthesize a multi-family channel after `!`. An alias uses `type Name = E | F`; its entire right-hand side may also be parenthesized.

Aliases are transparent. Nested aliases flatten, repeated families collapse, and member ordering has no semantic significance:

```kalandra
type First = ReadError | ValidationError
type Second = ValidationError | First | ReadError
```

Both denote the same set. `ReadError | ReadError` is just `ReadError`. An alias does not introduce a wrapper error or a new family identity. Constructors and patterns keep the original family names, such as `ReadError.Missing`.

Aliases can be exported, imported with a local name, and re-exported. Import the original member families as well when a caller needs to name their variants in a handler. Re-exporting an alias does not implicitly export its members. Alias cycles, unknown members, ordinary enums/numeric types as members, reserved names and duplicate module bindings are rejected.

Aliases work in error channels and function type annotations. [Record and enum payload fields](error-payload-fields.md) also support aliases as completed error data, including nested storage and TaskError fields. There are no general structural or untagged value unions.

## Widening and recovery

A function may propagate `ReadError` into `ReadError | ValidationError` with `try`, or return the fallible result directly as its final expression. It may likewise propagate a smaller union into a larger one. Every source family must be covered by the destination channel. The compiler never drops an unhandled family.

Conditional and match results combine their escaping application-error families. Anonymous handlers infer the families they still propagate. Recovering a result removes its original channel; errors produced by the handler form the replacement channel, with the same single success type.

For example, a handler may recover read failures while keeping validation failures:

```kalandra
fn loadOrDefault(available: Bool) -> int ! ValidationError:
  load(available) catch fn(error):
    match error:
      ReadError.Missing: 20
      ValidationError.TooSmall: fail error
```

[Whole-value match-arm narrowing](error-narrowing.md) gives `error` the selected family inside each branch. The example rethrows the original ValidationError without reconstructing its fields. Outside a narrowed branch, `fail error` still propagates its complete current family set. Wildcards retain every family with unmatched variants.

## Qualified, exhaustive matching

A union match must cover every variant of every member, unless an explicit final `_` supplies the policy for remaining variants. Families can both define `Bad`; `ReadError.Bad` and `ParseError.Bad` remain distinct cases with distinct field types. Duplicated and unreachable cases are rejected. An alias does not supply replacement variant qualifiers.

Matching evaluates the subject once and reads only the selected variant's fields. Dispatched intermediate results belong to the enclosing match; they do not create an extra unhandled-error obligation. This also fixes multi-arm matches which mix ordinary successes and explicit failures.

## Ownership and reporting

Widening preserves the original family, variant, field types and managed backing. It does not stringify an error, copy its owned arrays, or invent a success payload. Only the active variant's dependencies are retained. Cached observations can share backing and outlive the originating scheduler. Failed observations leave the destination untouched and release partially acquired ownership during settlement.

The current implementation uses canonical private tagged aggregates and remaps variants when widening. It is a correctness baseline; compact variant storage remains optimization work. Generated ordinals and local layouts are not public network or C/C++ contracts.

Unhandled CLI errors still print the original `Family.Variant` and exit with status `89`. An alias name or synthesized union name does not replace that identity. Arithmetic, bounds and implicit allocation faults retain their execution-fault behavior and bypass ordinary application-error recovery.

## Remaining boundaries

[Local mixed application/TaskError channels](mixed-error-channels.md) are implemented. [Local CPU workers and parallel groups](worker-errors.md) also preserve application unions alongside execution failures through observation and settlement. Fully recovering an application union inside a worker is supported.

GPU error transport, distributed schemas, public foreign interfaces, recursive payloads and general function values are not introduced by this milestone.

## Qualification

[Frontend tests](../../native/tests/ErrorUnionTests.cpp) cover canonical alias identity, subset propagation, exhaustive qualified matching, handler composition, invalid/cyclic aliases and malformed union metadata. [Compiled cases](../../native/cmake/VerifyErrorUnions.cmake) exercise all ten numeric widths, direct returns, `try`, guards, conditional/match joins, rethrows, channel replacement, local worker recovery, shape refinement, imported aliases and original CLI error identities.

The shared [private result harness](../../native/tests/ApplicationErrorRuntimeTests.cpp) checks both active union families with managed String/array payloads, cached observations, scheduler teardown, rejected descriptors, inactive-arm isolation and partial retention failure. Debug, optimized, ASan/UBSan and TSan runs are recorded in the [qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-error-unions-2026-10-19.json).
