# Synchronous control flow in map callbacks

Implemented native CPU subset — 2026-10-19. Direct named and inline callbacks can handle expected errors and match variants before returning each completed element. They use the same checked synchronous calls as bound function values.

```kalandra
error E:
  Bad:
    value: int

fn checked(value: int) -> int ! E:
  if value == 0: fail E.Bad {value: 21}
  value

fn recover(value: int) -> int:
  checked(value) catch fn(error):
    match error:
      E.Bad {value}: value

fn main() -> int:
  values = map([0, 21], recover)
  values[0] + values[1]
```

The [standalone fixture](fixtures/control-maps.k) returns `42`. An inline expression callback can also recover before returning: `map([0, 21], fn(value): checked(value) catch fn(error): 21)`.

## Execution and ownership

Mapping remains sequential and ordered. Conditionals, enum/error matches, guards and `try` in synchronous helpers follow their existing rules. Every expected error must be handled before the callback returns an element. `map` does not implicitly collect, discard or propagate callback error channels.

The supported scalar element types are numeric values, Bool, String, records/enums and completed function values. A completed function value may itself have an expected-error signature; invoking it requires handling that error as usual. Array/tensor callback results remain outside this subset.

Captured values are evaluated once before iteration. Named functions need no environment; inline callbacks capture referenced immutable values. Record fields, collection backing and returned function environments retain their existing invocation ownership. Empty maps do not execute callback bodies. Runtime faults stop iteration at the failing operation, preserve its source location and publish no partial array. Cancellation and allocation faults use the existing checked invocation machinery.

A direct `spawn map(values, callback)` creates its internal callback inside the worker, after transporting the ordinary input and captures. Mapping inside a named worker also works. Results can be retained after worker completion and read through repeated awaits. This does not enable function-valued inputs, captures or results across worker boundaries.

## Lowering

Straight-line callbacks keep their existing MLIR lowering. For callbacks needing synchronous control flow, the continuation planner builds a private checked function value and passes it to a small MLIR map adapter. The adapter invokes the synchronous callback entry for each element. Nested maps use the same route. There is no new public ABI or scheduler protocol.

The capability check distinguishes synchronous control/error handling from task/device coordination. It also verifies the callback capture count against the environment layout. Inline map functions now retain their lexical capture count explicitly.

This implementation favors correctness over allocation reduction: synchronous callback frames and environments follow invocation settlement. Their storage can accumulate across a large map. Frame reuse, earlier release and avoiding descriptor copies belong to the [optimization roadmap](../roadmap.md#optimization).

## Remaining limits

- Callback task/device coordination is rejected transitively, including on empty inputs.
- Fallible callback results require an explicit recovery wrapper; there is no fallible-map result model.
- GPU/mixed-target maps remain unsupported. [Recursive synchronous callbacks](native-recursive-callables.md) use the bounded checked CPU calling path.
- [Indented anonymous suites inside call delimiters](native-delimited-closures.md) are supported; general standalone indented expressions in delimiters remain separate syntax work.
- Function transport, indirect spawn, distributed execution and the public C/C++ ABI remain separate work.

## Qualification

All 88 selected checks passed on Apple M2 Max. The new execution suites compile 108 programs across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. Relevant existing frontend, map, module, closure, aggregate, application-error and worker regressions pass in their selected debug and ASan/UBSan modes. This is focused qualification, rather than a full-suite claim.

Coverage includes all ten numeric scalar widths, enum matching, owned records, nested maps, function-valued elements, imported callbacks, empty inputs and retained worker results. Four worker programs run with one, two and four workers in each build mode. The raw invocation harnesses check 56 allocation/context probes and 24 first-fault allocation probes, plus eight already-stopped contexts. Sixteen invalid CLI programs preserve all 48 pre-existing output artifacts. Frontend suites check 11 accepted programs, four effect/transport rejections and six forged map metadata cases per variant.

The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-control-maps-2026-10-19.json) records the selected checks, coverage and frozen implementation/test hashes.
