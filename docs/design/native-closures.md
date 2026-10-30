# Managed CPU closures

Current CPU extension: [proven coordinating function values](native-resumable-callables.md) now use checked continuation dispatch, managed environments and the shared worker scheduler. This supersedes the synchronous-only coordination restriction in this earlier implementation record; recursive coordinating calls now use bounded CPU continuations; unknown targets and GPU callables remain gated.

Current aggregate extension: [proven function-containing CPU worker values](native-worker-callable-aggregates.md) now support records, ordinary enums, function arrays, record arrays and transitive captures. Unproven parameters, expected-error payloads and indirect spawn remain staged. Earlier subset restrictions below describe the preceding increments.

Implemented native subset — 2026-10-19. Anonymous functions can be bound, passed, selected and returned with immutable lexical captures. [Indented anonymous bodies](native-block-closures.md) and [direct error declarations](anonymous-errors.md) now extend the initial expression form. [Function-valued fields](native-callable-fields.md) now also store these closures in records and enum payloads. [Arrays of functions](native-function-arrays.md) now also preserve managed captures. This extends [native function values](native-function-values.md) under the [accepted function-value design](function-values.md).

```kalandra
fn makeAdder(offset: int) -> fn(int) -> int:
  fn(value: int): value + offset

fn apply(value: int, transform: fn(int) -> int) -> int:
  transform(value)

export fn main() -> int:
  addTen = makeAdder(10)
  apply(22, addTen) + addTen(0)
```

The [complete fixture](fixtures/closures.k) returns `42`. `addTen` keeps its captured `offset` after `makeAdder` returns. Calling it repeatedly does not reevaluate the capture's initializer. Distinct calls to the creator keep separate immutable environments.

## Types and lexical scope

An expected function type supplies anonymous parameter types:

```kalandra
increment: fn(int) -> int = fn(value): value + 1
twice = fn(value: int): value * 2
```

Without an expected signature, each parameter needs an annotation. Result types are inferred from the body’s final expression or checked against the expected/annotated result. Later calls do not retroactively infer missing parameter types. Exact numeric widths, nominal record identities, collection categories/shapes and canonical expected-error sets retain the existing callable rules.

Only referenced lexical bindings are captured. Repeated references to one binding produce one capture. Parameters shadow outer bindings, nested closures capture through their enclosing scopes, and captured function values keep their own environments valid transitively. Named functions and closures with the same signature can be selected at runtime or passed to the same higher-order parameter. Parameter and capture counts have no special small limit; ordinary compiler complexity and allocation limits apply.

Supported captures are completed CPU numeric scalars, Bool, String, arrays, tensors, records/enums and function values. Pending expected results, task handles, resident device values and device selectors are rejected. Completed built-in TaskError/FaultKind captures remain outside this first capture subset. Returning a closure cannot hide an escaping task or device capability.

## Expected errors and local execution

A closure can return an expected error or propagate one through a declared expected signature:

```kalandra
fn makeValidator(offset: int) -> fn(int) -> int ! InputError:
  fn(value): try checked(value, offset)
```

This is a signature illustration with a separately declared `InputError` and compatible `checked` function. The invocation error belongs to the returned function. A factory's own error channel remains separate, as described in [fallible signatures](native-function-values.md#factories-and-returned-functions). Result/error inference does not turn an unhandled expected result into a completed value.

Compatible closures work with ordinary calls, pipelines and sequential maps. Existing inline map callbacks remain supported and may capture a closure. Empty maps do not execute the callback body. Maps still require completed element results; recover errors within a compatible callback rather than relying on implicit fallible-map aggregation.

A local worker may create and invoke a closure and return ordinary supported data, including owned records. A coordinator may hold a completed closure across an await. [Capture-free named function arguments](native-worker-functions.md) now cross to named CPU workers. [CPU data-capturing closure arguments](native-worker-closures.md) are also supported. [Direct nested function captures](native-worker-nested-closures.md) are also supported. Function-containing aggregate captures and unproven arguments, unproven function-valued results and spawned-map captures, indirect spawn and task/device-coordinating callable targets remain rejected. This does not add scheduler waits to synchronous invocation.

## Private representation and managed lifetime

A function value now contains a code pointer and an environment pointer. Named references and capture-free anonymous functions need no environment allocation. Capturing closures allocate immutable, invocation-owned environment storage; creators store completed values there rather than pointers to stack locals. Both pure MLIR adapters and synchronous expected-error entries use the environment argument alongside the existing checked invocation context.

Captured numeric arrays/tensors currently use checked descriptor packing and reconstruction, which can copy data. Captured owned arrays, records, Strings and other closures retain their existing invocation-owned backing. The environment and transitive backing stay alive until invocation settlement; this is automatic lifetime management, not a source allocator or a promise of prompt reclamation. Worker output data uses the ordinary retained-result path. Transporting a closure or keeping one beyond its invocation is not a public runtime API.

Allocation failures and cancellation use existing checked execution rules. A failed or pre-cancelled invocation cannot enter a closure creator, publish a partial callable, or execute its body. Checked arithmetic/shape faults retain their source locations. Temporary synchronous control-flow frames remain separate from captured environments and are released when the call exits.

## Remaining work

- Continue the [local CPU worker function-transport contract](worker-function-transport.md) with broader proven provenance after the implemented [nested function capture stage](native-worker-nested-closures.md), then containing values and indirect synchronous spawn; resumable callable values remain separate.
- Earlier environment reclamation, allocation elision and shared numeric capture backing, tracked in the [optimization roadmap](../roadmap.md#optimization).

[Recursive synchronous callables](native-recursive-callables.md) and [direct control-flow map callbacks](native-control-maps.md) are now implemented.

GPU callbacks, distributed contracts and public C/C++ integration retain their separate roadmaps. The code/environment representation and test exports are private compiler details.

## Qualification

### Initial expression closures

All 183 selected checks passed on Apple M2 Max, including existing frontend, function-value, map, collection, module, task, error and inline-capture regressions. The new closure execution suites compile 176 programs across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. Frontend checks cover 28 valid programs, 15 rejected programs and six malformed capture-metadata cases per variant.

Private lifetime probes invoke returned closures 256 times after their creators exit, checking independent record/String/array environments. They exercise 52 context allocation probes and 32 returned-environment creation probes, including untouched output on failed creation, stopped creator/callee contexts and balanced storage after settlement. CLI sweeps run 960 allocation-failure processes. Owned worker results are additionally checked with worker pools of one, two and four threads, including repeated awaits. Empty and rank-zero captures, runtime shape mismatches, all ten scalar widths, nested functions, lexical shadowing, typed errors and empty maps are covered. A separate debug check verifies imported creators retain their helper-module name resolution.

The [qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-managed-closures-2026-10-19.json) records the selected tests, per-variant probes, manual import check and frozen source hashes. This is focused macOS ARM64 qualification; it does not qualify the remaining body/storage/transport features, a public callback ABI, earlier reclamation or performance improvements.

### Indented anonymous bodies

[Anonymous statement suites](native-block-closures.md) extend the same managed closures with ordered immutable bindings and a final expression. All 56 selected checks passed for that increment, including four new execution variants and relevant debug/ASan/UBSan regressions. See its [qualification record](native-block-closures.md#qualification) for the block-body, capture, parser and failure coverage.

Current result extension: [proven function-valued CPU worker results](native-worker-function-results.md) now support cached awaits, use after group release and onward submission. Containing values and unproven provenance remain staged.
