# CPU function values that coordinate tasks

CPU function values can now create and await child tasks, including through higher-order calls, returned closures, records, function arrays and `parallelMap` callbacks. No effect annotations or special callback syntax are required.

```kalandra
fn double(value:int)->int:
  value*2

fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)

fn main()->int ! TaskError:
  callback=run
  try callback(21)
```

This program returns `42`. An infallible function that handles its task errors internally can also be passed around. Synchronous expected-error handling remains distinct from task coordination.

## Supported execution and ownership

The callee expression is evaluated once before its arguments; arguments are evaluated once, left to right. Runtime selection may choose between compatible synchronous and coordinating functions. The selected function's parameter/result signature and expected-error channel retain their ordinary meaning. A named reference, captured anonymous function, field projection, indexed function array or returned closure uses the same source call spelling.

Captured CPU values must satisfy the existing proven worker-copy contract, even for a same-invocation coordinating call. This includes supported numeric values, arrays/tensors, Bool, String, completed TaskError data, records/enums and transitively proven callables. Environments are validated and copied into managed invocation storage before their fields enter the continuation. They cannot depend on an exited creator or producer group. Unsupported transitive capture types remain compile-time diagnostics; this does not make task handles or device capabilities capturable.

`spawn callback(value)` copies the callable with its arguments into an ordinary scheduler-owned task packet. `parallelMap(values, callback)` can use a coordinating bound callback; its element tasks share the invocation's worker pool and resource limits. Ordinary `map` still requires a non-coordinating callback. This extension does not silently turn a sequential map into a parallel one.

Awaiting a child suspends the current continuation. It does not block a worker thread, create a thread per call, or start a second scheduler. Direct invocation does not create an extra task merely because its callee is a function value. Typed application errors, TaskError propagation/recovery, original execution faults, cancellation and settlement use the existing control-flow and ownership rules.

## Checked dispatch

The [inferred execution analysis](native-callable-execution.md) supplies a bounded set of compatible possible targets. The compiler emits a runtime selection check followed by the chosen coordinating body's continuation, including its captured values and argument bindings. This is specialization of a finite known target set, not a dynamic machine-code call into a synchronous entry that secretly waits for children.

The private code/environment pair now supports a coordinating entry identity as well as a synchronous code address. A coordinating identity is a tag for compiler-generated dispatch; it is never called as a native function pointer. The existing typed environment-copy machinery validates identities, signatures, environment ownership, sizes and keys. The synchronous alternative is validated too and explicitly rejects coordinating identities before an indirect native call. Unknown identities and malformed environments cannot fall through into a native call.

This remains a private implementation convention. It is not a public closure ABI, serialization format, C callback interface or distributed function contract.

## Current limits

- Coordinating indirect calls require a complete proven CPU target set. The underlying whole-module flow proof is conservative about aggregate fields and callers. Unknown sources in reachable coordinating calls and exhausted flow proofs remain diagnostics. Uncalled generic library helpers need no invented incoming callback proof: their unproven coordinating dispatch retains a checked rejection stub. They cannot be used as an unchecked private-entry fallback.
- Recursive coordinating call graphs now suspend recursive backedges in bounded scheduler child records, including direct, indirect and mutual calls. Source error channels, copied captures and existing scheduler record/group/depth/byte limits apply. Existing checked synchronous recursion is unchanged. See [recursive source integration](native-recursive-storage.md#completed-local-cpu-source-integration).
- Function values remain ordinary CPU capabilities. Placement-aware/Metal callables and device-valued captures are not enabled.
- The existing continuation depth, slot and frame limits apply. Target specialization can increase generated code/frame size; large target sets do not have an unlimited expansion guarantee.

Sharing or eliminating repeated environment copies, specialized direct entries and more compact dispatch are optimization work. No speedup is claimed by this milestone.

## Qualification

The source catalog and companion programs exercise zero/multiple arguments, aliases, captured and returned functions, higher-order calls, synchronous/coordinating runtime selection, infallible recovery, function arrays/records, explicit indirect spawn, parallel-map callbacks, captured owned data, application errors, execution faults, callable error payloads and producer-independent results. Former synchronous-only coordination rejection cases become accepted compiler/executable checks; unknown-source calls remain rejected; recursive coordinating graphs now use bounded scheduler continuations.

A private generated-entry probe runs with one, two and four workers. It checks overlapping nested child work, one-worker progress, cancellation, deadlines, admission failure, complete settlement and balanced scheduler/invocation accounting. Private callable-layout probes reject unknown/wrong identities, invalid environment addresses, wrong ownership keys and invalid headers, and verify independent environment copies after source settlement. Allocation-failure injection checks cleanup and unpublished failures. An ordering probe verifies callee construction before argument evaluation; rejected builds preserve existing executable and IR artifacts.

**117/117 selected checks passed**, including debug, optimized, ASan/UBSan and TSan execution. Four additional boundary/arity probe checks passed against the same compiler implementation. See the [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-resumable-callables-2026-10-20.json) for the exact selection, source hashes, private probe limits and remaining scope. This is focused qualification, not the full project suite.
