# Spawning synchronous function values on CPU workers

Current CPU extension: [proven coordinating function values](native-resumable-callables.md) now use checked continuation dispatch, managed environments and the shared worker scheduler. This supersedes the synchronous-only coordination restriction in this earlier implementation record; recursive coordinating calls now use bounded CPU continuations; unknown targets and GPU callables remain gated.

Implemented native subset — 2026-10-20. A completed function value can now be dispatched to a local CPU worker with `spawn`. This extends [indirect-call provenance](native-worker-indirect-provenance.md) and uses the existing checked worker handoff.

```kalandra
fn main() -> int ! TaskError:
  offsets = [40]
  transform = fn(value: int): offsets[0] + value
  try parallel:
    work = spawn transform(2)
    first = try await work
    second = try await work
    second
```

The result is `42`. Bindings, runtime-selected functions, callable record fields, indexed function arrays, factory expressions and directly supplied anonymous functions are supported. A named coordinator can receive a callback and spawn several calls to it, including inside an independently scheduled coordinator worker.

Arguments retain their declared types and order. Supported CPU data and proven function-containing values can be mixed in one signature. Results can include owned arrays, records and functions, with the existing cached-observation and post-producer lifetime guarantees. All ten implemented numeric widths use the same lowering and handoff rules.

## Evaluation, errors and ownership

The caller evaluates the callee first, then arguments from left to right. A factory or argument failure happens before task admission; those computations are not deferred into the child. The child executes only the resolved function invocation.

The compiler generates a fixed task entry for the spawn site, with a packet containing the typed callee followed by its arguments. Existing copy helpers validate the exact callable signature and trusted entry, clone its owned environment and retain immutable backing before the child can use it. The source remains immutable. Copied environments retain the existing checked call, invocation storage and root scheduler budgets. No new dynamic scheduler dispatch ABI is introduced.

Declared expected errors remain part of the child's result. Awaiting adds the normal `TaskError` channel, while handled callback errors preserve their declared payloads. Runtime faults inside the callback are supervised child faults. In particular, the child plan must preserve a fallible invocation's wrapper rather than treating it as a pure numeric kernel result.

## Remaining boundaries

Targets must remain transitively synchronous. Function values whose targets coordinate tasks/devices are still rejected; statically named coordinator calls keep their existing support. This milestone does not add resumable function values.

The compiler must prove the callee's transitive captures, every function-containing argument and any function-containing result. Unknown provenance and unsupported capture types still fail compilation. The conservative caller/aggregate joins described in the provenance design remain in effect.

Indirect spawn currently requires ordinary CPU execution. Explicit `on devices.cpu()` and `on devices.gpu()` are rejected for indirect invocations; this does not enable callbacks in placement-aware/Metal modules. [Function-containing spawned maps](native-worker-callable-maps.md) are now supported. [Function-containing spawned collections](native-worker-callable-collections.md) are also supported. [Function-containing worker error payloads](native-worker-error-functions.md) are now supported with proven origins. Distributed execution and public C/C++ callbacks are separate roadmaps.

## Qualification

The focused qualification passed **73/73 selected CTest checks** against one compiler build. This is not the full project suite. The source catalog covers aliases, captures, selection, fields, indexed arrays, factory expressions, zero arguments, nested coordinator workers, repeated awaits, returned functions, aggregate results, mixed argument types, expected errors and checked faults. A separate ordering probe verifies that callee construction faults precede argument faults.

The private generated-entry harness verifies producer-independent ownership, cached observations, invalid code/environment pointers, stopped destinations, allocation failures and balanced resource ledgers. Rejected source programs preserve existing executable, LLVM and MLIR outputs. The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-indirect-spawn-2026-10-20.json) records the final selected checks and scope.
