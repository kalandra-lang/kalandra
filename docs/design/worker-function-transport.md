# Function values across local CPU workers

Current CPU extension: [proven coordinating function values](native-resumable-callables.md) now use checked continuation dispatch, managed environments and the shared worker scheduler. This supersedes the synchronous-only coordination restriction in this earlier implementation record; recursive coordinating calls now use bounded CPU continuations; unknown targets and GPU callables remain gated.

Current CPU support includes [function-containing values](native-worker-callable-aggregates.md), [indirect spawn](native-indirect-spawn.md), [spawned maps](native-worker-callable-maps.md), [spawned collections](native-worker-callable-collections.md), and [function-containing error payloads](native-worker-error-functions.md). These use bounded, conservative provenance proofs; unknown origins and unsupported transitive captures remain diagnostics. General task/device-coordinating callable values remain separate work.

Implementation contract — 2026-10-19, extended by the linked CPU implementation stages. This follows the accepted [function-value design](function-values.md) and [native task contract](native-tasks.md).

Local workers are threads in one native executable. Transfer a typed function value and its immutable captures within that process. No network serialization, source-code shipment, dynamic library loading, or public callback ABI is involved. Distributed execution still requires the separate design discussion.

## Source behavior

A function-valued parameter needs no worker-specific annotation:

```kalandra
fn apply(value: int, transform: fn(int) -> int) -> int:
  transform(value)

fn main() -> int ! TaskError:
  offsets = [20, 20]
  transform = fn(value: int): offsets[0] + offsets[1] + value
  try parallel:
    work = spawn apply(2, transform)
    first = try await work
    second = try await work
    first + second - 42
```

The [argument fixture](fixtures/worker-function-arguments.k) returns `42`. Creating `transform` captures the completed array once; submitting the task does not invoke the callback or reevaluate its initializer. The parent retains its own usable binding. Both awaits observe the same cached outcome.

A worker can return a function which remains usable after the parallel group has settled:

```kalandra
fn makeAdder(offset: int) -> fn(int) -> int:
  values = [offset]
  fn(value: int): values[0] + value

fn main() -> int ! TaskError:
  transform = try parallel:
    try await spawn makeAdder(40)
  transform(2)
```

The [result fixture](fixtures/worker-function-results.k) returns `42`. Keep the captured values alive, not the worker's invocation, task handle or frame. Invocation of the returned function uses the caller's current execution context.

Indirect synchronous CPU spawn uses the same ordinary call spelling:

```kalandra
transform = double
work = spawn transform(21)
answer = try await work
```

This fragment belongs inside a parallel group, with `double` defined normally. The [complete indirect-spawn fixture](fixtures/worker-indirect-spawn.k) should return `42`. Evaluate the callee first, then its arguments left to right, once, before admission. Submission makes the task runnable; awaiting does not start it. Separate transport of function arguments from indirect spawn in the implementation sequence below.

## Eligible values

Only supported synchronous CPU functions are eligible initially. Existing exact callable signatures, numeric widths, nominal record/enum identities, tensor shape guards and canonical expected-error sets apply. Named references, runtime-selected targets and closures use the same source type. No `Send`, capture list, decorator, allocator annotation or manually written effect list is introduced.

A closure's hidden environment has compiler-known capture types even when its source signature is only `fn(int) -> int`. Eligibility follows those types transitively, including nested functions and aggregate fields. A compatible signature alone is not proof that an arbitrary environment is safe. Code references and capture descriptors must come from the same linked executable's trusted compiler metadata.

Completed supported CPU scalars, Bool, String, arrays, tensors, records/enums and eligible functions may cross this boundary. Preserve existing prohibitions on pending expected results, child task handles, resident GPU values, device selectors and scoped resource capabilities. Wrapping an ineligible value in a closure, array, record, enum or error payload must not bypass those restrictions. Do not turn current unsupported capture categories into supported ones incidentally.

Synchronous functions may return owned values or typed expected errors and may use [bounded recursion](native-recursive-callables.md). Their bodies must remain transitively free of task/device coordination. A function which creates child tasks requires a later resumable-callable convention, even if its source signature resembles a synchronous callable. Existing statically named coordinating worker entries remain supported.

## Ownership at each handoff

| Handoff                            | Required ownership before publication                                                                                                               |
| ---------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| Parent to child arguments/captures | Child execution owns a complete environment and all required captured backing before the parent may resume or release its packet.                   |
| Child to cached outcome            | Outcome owns the callable and transitive backing before child execution storage is settled.                                                         |
| Cached outcome to observer         | Observing invocation owns its callable and backing before the task/group may be released. Repeated observations remain independent of that release. |
| Onward submission                  | The new child acquires its own ownership; the previous producer or observer need not stay alive.                                                    |

Use the existing `ValueLayout` copy/retain and task publication protocols as the integration points. They currently transport ordinary owned data; copying two callable pointers through those hooks is insufficient. Never access a source invocation concurrently with its execution or after settlement. Task publication synchronization establishes visibility of immutable environments before another worker reads them.

For the first implementation, copy the environment's typed descriptor/value storage into the destination invocation. Retain or copy captured collection/String backing through the existing checked value handoff paths. Rebuild nested function environments transitively. Sharing whole immutable environments and eliminating these copies are later optimizations; no deep-copy or zero-copy promise is observable in the language.

Copying an environment must not invoke source functions, evaluate initializers, copy native stack addresses, retain an invocation pointer, or capture its cancellation/fault state. The code itself has executable lifetime. Each invocation supplies its own checked context, cancellation token and recursion counter. Concurrent calls to one immutable closure use separate invocation state; no shared call frame or mutable scratch belongs in the environment.

Immutable construction prevents a local binding from capturing itself during initialization. Named recursive code references do not create recursive capture storage. Shared sub-environments can still form large graphs: transport must use bounded traversal, checked size arithmetic and memoization or equivalent accounting to avoid exponential copying. Do not recurse unchecked on the C++ stack. Establish and document private traversal/storage budgets when implementing this path; integrate new transport reservations with scheduler accounting and release them on every exit. Do not claim existing packet/frame quotas already bound all transitively retained collection payload bytes.

## Private metadata and checked publication

Transport needs more information than the source callable signature: a trusted invocation entry, capture-layout/copy information and ownership of the environment. Named capture-free functions need no environment copy. Distinct closure definitions with the same signature may have different capture layouts.

The compiler may extend its private two-pointer representation or associate entries with immutable descriptors. It must validate signature/layout identity and target compatibility before interpreting environment storage, rather than trusting a caller-supplied pointer or treating every compatible signature as the same capture layout. Invalid private metadata takes the checked internal-fault path; it never triggers an arbitrary indirect call. No stable numeric identity, exported wire format or public runtime constructor is defined here.

Do not publish a partial callable. If any allocation, validation, retention or cancellation check fails, unwind temporary copy bookkeeping and let the destination invocation settle all acquired backing. Mark no packet/result initialized until its complete value is owned. Preserve the existing task contract's failure policy at submission, outcome publication and observation; do not convert a failed copy into an application error or silently run the body in the parent.

Expected errors from executing the callback retain their declared error channels. Scheduler admission failures and checked execution faults remain separate. If a typed error payload contains a function, its active payload needs the same ownership transfer as a successful function result. All variants must satisfy boundary eligibility statically; an unchosen variant is not a workaround. Payload-copy failure follows the checked execution-failure path and cannot publish an incomplete expected error.

## Reviewable implementation sequence

1. **Capture-free named function arguments (implemented):** private trusted target/layout metadata, copy validation and a function parameter of a statically named CPU worker. Keep all captured environments, worker function results and indirect spawn rejected until their own increments pass. Use conservative compiler provenance: a value whose capture-free eligibility cannot yet be established remains an implementation diagnostic, not a runtime surprise.
2. **Captured function arguments (CPU data captures implemented):** destination-owned environments, nested eligible captures, shared backing retention, runtime-selected targets and concurrent worker-local invocation. The [implemented CPU data subset](native-worker-closures.md) covers direct arguments; direct nested function captures are [implemented](native-worker-nested-closures.md); [function-containing capture aggregates](native-worker-callable-aggregates.md) and [spawned-map captures](native-worker-callable-maps.md) are also implemented. Retain transitive checks on every transported value.
3. **Function results (proven synchronous values implemented):** transfer child → outcome → observer, including captured functions used after group release, cached awaits and onward submission. Separate factory errors from invocation errors.
4. **Containing values (proven CPU payloads implemented):** extend the same descriptor traversal to records, enum variants, function/record arrays and expected-error payloads in both directions. Do not remove blanket aggregate checks before each representation is qualified.
5. **Indirect synchronous spawn (implemented):** generate a scheduler-owned task entry for a runtime callable, preserving callee/argument evaluation, admission, cancellation and settlement. This does not introduce resumable callable bodies.

Keep each earlier restriction diagnostic until its replacement path is qualified. General task/device-coordinating callable values, GPU callbacks, network contracts and public C/C++ callbacks remain separate work.

## Qualification gates

For each increment, exercise debug, optimized, ASan/UBSan and separate ThreadSanitizer builds, with adequate scheduler quotas and one/two/four-worker pools. Include:

- Parent/creator exit before child use; child settlement and group release before returned closure use; onward handoff and repeated cached awaits.
- Different environments with the same signature, capture-free targets, nested captured functions, imported definitions, empty collections and runtime shape mismatches.
- Concurrent calls to the same immutable closure, independent checked contexts/counters, recursive targets and owned expected-error payloads.
- Failure at each allocation/retention/publication step, rejection before admission, queued/running cancellation, stopped contexts, bounded deep/shared graphs and balanced final ownership/reservations.
- Forged or mismatched private target/signature/capture metadata rejected before dereference/invocation; untouched outputs on rejected copying.
- Transitive task/resource/device escape rejection, coordinating-target rejection, and unchanged ordinary task, closure, map, error and recursion behavior.

The original design fixtures are supplemented by the linked implementation-stage catalogs and qualification records. Those records identify their selected compiler/runtime/sanitizer checks and remaining limitations.
