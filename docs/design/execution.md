# Kalandra resources and task lifetimes proposal

Accepted execution and resource direction — 2026-10-14. [Core syntax](core.md), [automatic memory management](memory.md), [numeric rules](numbers.md), and [errors and effects](errors.md) are accepted design foundations. The project owner approved parallel groups, spawn/await, and scoped resource helpers for the fresh implementation. These are not implemented features of Kalandra 0.13; error payloads are accepted separately and task placement remains under review.

Ordinary functions run ordinary code. Explicit parallel work belongs to a parent operation, and external resources have a defined use interval. Avoid detached tasks, manual freeing, implicit parallel mapping, and allocator scopes in the initial execution model.

## Accepted decisions

- Ordinary function calls and ordinary `map` remain sequential in their observable execution order.
- A `parallel` expression owns a group of child tasks and returns a typed success/error result after the group settles.
- `spawn function(arguments)` starts a task in the nearest owning parallel group; `await task` obtains its typed result.
- Child task handles do not escape their owning group. Every successful path must observe each child's result.
- Failure or cancellation stops admitting new child work, requests cancellation of unfinished children, and completes required cleanup before group exit.
- Files and similar resources are exposed through scoped library functions with a compiler-visible lifetime contract. No new resource keyword is required.
- Ordinary managed data may outlive a function or resource callback if it has independent valid storage. Resource capabilities tied to closing operations may not escape.

Task placement, tensor dispatch syntax, and remote import syntax are later decisions. A parallel group is an ownership boundary, not a promise of a dedicated operating-system thread per task.

## A local parallel calculation

```kalandra
fn combine(left: [float], right: [float]) -> float ! TaskError:
  try parallel:
    first = spawn sum(left)
    second = spawn sum(right)
    leftTotal = try await first
    rightTotal = try await second
    leftTotal + rightTotal
```

Assume `sum` accepts a nonempty or empty array under a defined library contract and returns a float; it is used here only to illustrate task ownership. For left `[1.0, 2.0]` and right `[3.0, 4.0]`, the success result is 10.0. These tiny sums are not a performance justification for spawning work.

`spawn` evaluates the call target and arguments in the parent's context, left to right, once. It captures the resulting values and submits the invocation for execution. The task becomes runnable at submission, not when first awaited. Admission and execution are bounded by the scheduler.

Both sums can run concurrently. Awaiting first does not stop second from progressing. Each task publishes one immutable success/error outcome. The two await bindings are explicit; reading the handles does not magically unwrap a value.

`parallel` is a fallible expression. The outer `try` propagates its TaskError to combine. Inside the group, `try` propagates a child error to that group's failure boundary, triggering cancellation/cleanup before producing the group's error result. This extends the accepted propagation model to a compiler-defined fallible block; it is not a nonlocal return that skips the group's cleanup.

A caller can handle the group result with the accepted catch syntax. Exact precedence and parenthesization of block expressions, spawn, await, and catch will be defined in the grammar before implementation.

## Inline spawn and await

```kalandra
value = try await spawn sum(left)
```

This is valid inside a parallel group and groups as `try (await (spawn sum(left)))`. Prefix operations nest from right to left around the invocation: submission, observation, then expected-error propagation. The group still owns and cleans up the child. Spawn is not valid outside an owning parallel group.

Two consecutive inline spawn/await expressions submit the second task only after the first completes. To overlap two calls, bind both spawn results before awaiting either. Do not infer concurrency merely because a statement contains spawn.

## Task result types

For a nonfallible child returning T, awaiting it produces `T ! TaskError`. TaskError represents execution failures such as cancellation, admission failure, and a child execution fault. It does not replace the child's success type with Any.

For a child returning `T ! E`, the task boundary must preserve both E and execution failures. The semantic result is a success T or an application error E or a TaskError. The accepted [variants and error payloads design](variants.md) supplies `T ! (E | TaskError)` and named payload fields.

An expected application error is a normal task outcome until the parent propagates or handles it. It need not automatically cancel siblings if the parent deliberately handles it. A child execution fault makes the group fail and requests sibling cancellation; recovery is at the enclosing group boundary, not resumption inside the faulting child.

Awaiting the same settled handle again may read the cached immutable outcome without rerunning the task. The initial runtime should support this read behavior; it must not treat a second await as another invocation. Dropping a handle is not cancellation or proof of completion.

Discarding a spawned call or finishing a successful group with an unobserved child result is diagnosed. Data-dependent task collections and helpers need compiler-visible structured ownership; their iteration/await-all syntax is a separate proposal. Do not reinstate the prototype's ambiguous await-on-array behavior.

## Structured ownership

Task handles belong to the group that created them. They may be referenced by helpers during that group's lifetime, but cannot be returned from the group, stored in an escaping closure, or passed to unbounded native registrations. There are no user-written lifetime parameters for the initial task model.

This restriction concerns live work and its cancellation owner, not ordinary array or record allocation. The accepted automatic memory model continues to permit ordinary data to escape safely.

A function that needs parallel work can create its own group and return its completed result. A child can similarly create a nested group. Nested tasks remain descendants of the root operation for cancellation and admission limits.

A bounded scheduler must make progress when a worker waits for descendants. It must suspend/cooperatively schedule work or implement another proven strategy; it cannot simply block every occupied worker and leave child tasks queued forever. Device/foreign blocking operations require host-specific scheduling treatment.

## Success, failure, and cancellation

| Event                                                                   | Group behavior                                                                                                                  |
| ----------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| All observed children succeed and the body finishes                     | Return the body's immutable value after settling group bookkeeping                                                              |
| A child returns an expected application error and the parent handles it | Continue according to the handler's policy                                                                                      |
| The body propagates an expected error                                   | Stop admission, cancel unfinished children, settle cleanup, then return the error                                               |
| A child has a checked execution fault                                   | Fail the group, cancel unfinished siblings, settle cleanup, and return TaskError diagnostics                                    |
| The parent is cancelled                                                 | Stop admission, cancel descendants, settle cleanup, and return cancellation                                                     |
| Several failures occur                                                  | Preserve one primary failure and bounded secondary diagnostics; do not claim a deterministic winner between concurrent failures |

The body does not return while children still access resources that group exit would invalidate. Cleanup runs even when try leaves the body early. This join behavior is visible in the parallel construct, rather than a hidden wait introduced into every ordinary function.

Cancellation is a request to stop, not rollback. Files already written or external requests already performed remain effects. A parent cancellation is sticky for that group: a catch handler cannot silently reopen admission or restart cancelled work inside it. A retry is an explicit new operation with its own safety policy.

Queued work can be removed before execution. Running CPU work needs cancellation checks or a host boundary that can terminate it safely. Native threads cannot be killed arbitrarily with memory-safety guarantees. Deadlines initiate cancellation; they do not promise immediate physical termination or immediate release of device buffers. A noncooperative foreign call may delay final settlement. Hard termination needs process/device isolation whose semantics are explicitly documented.

## External resources through library scopes

Proposed library shape:

```kalandra
fn loadBytes(path: String) -> [uint8] ! ReadError:
  try files.withRead(path, fn(file): file.readAll())
```

`files.withRead` opens the resource, invokes the callback, and closes the resource before returning its completed result. This is a library operation with a compiler-recognized scope contract, not an ordinary callback with undocumented cleanup behavior.

The callback returns owned managed bytes. They remain valid after the file closes because their storage does not depend on an open file handle. The helper's signature explicitly combines its open/read/cleanup failures under its documented ReadError channel; the callback error is not ignored or implicitly unwrapped by arbitrary higher-order functions.

`file` is an immutable capability value for effectful access to an external resource. Reading may affect an external cursor or observe changing contents; that is an inferred IO effect, not mutation of an ordinary record or array.

Invalid:

```kalandra
file = files.withRead(path, fn(handle): handle)
```

Reject the capability escape: the resource closes at the callback boundary. The same rule applies to a closure or record that retains that handle. A borrowed native view into resource-owned memory cannot escape by looking like an ordinary array. An API that returns independent bytes must establish actual independent ownership.

The reason for this scope is external resource release, which is observable and can be required by the operating system/library. The compiler cannot generally move close operations like it can eliminate a temporary allocation. This does not reintroduce the removed allocation keyword.

Long-lived services, UI objects, and mapped buffers need explicit owned-resource APIs. Do not automatically force all such resources into a short callback or silently prolong a scoped handle because a closure retained it. Their construction, close, and event-registration contracts are a later resource-API proposal.

## Resources and child tasks

A scoped resource can be used by work that is guaranteed to finish within its callback interval, subject to the resource's concurrency contract. The compiler rejects escapes and unbounded retention.

To start with, file read capabilities are exclusive to the callback's sequential operations. An immutable capability does not imply that an underlying cursor, C object, or device is thread-safe. Independently opened files or APIs explicitly declaring safe concurrent access can be used by separate tasks.

If a resource callback creates a parallel group, that group must settle before the callback closes the resource. A native operation or device command that retains borrowed resource memory also needs a documented completion event that is joined within the interval. Otherwise the wrapper must produce a managed owned object with a separate lifetime or the compiler rejects the use.

Foreign callbacks carry an explicit registration lifetime. Callback unregistration and the end of any already executing callback must be confirmed before releasing its captured resources. This is essential for a future Qt/native integration, not something automatic heap ownership alone establishes.

## Resource cleanup failures

Scoped library helpers must clean up on success, expected error, and runtime-controlled fault paths. They must not publish partially initialized resource state. Host process death cannot guarantee language-level cleanup of external effects.

On success, a cleanup failure becomes an expected error under the helper's documented error type. On an existing error, preserve the primary error and attach bounded cleanup diagnostics rather than silently replacing it with the cleanup error. Concrete diagnostic payload representation follows the error/sum-types proposal.

Output-resource APIs expose important completion operations such as flush/commit as fallible behavior, not an invisible finalizer. A close operation is not a transaction rollback. No implicit transaction semantics are introduced.

## Native, remote, and GPU execution boundaries

A supervised task converts its execution fault into a bounded TaskError outcome. It cannot resume the faulting arithmetic instruction. The runtime isolation mechanism must match the promise; unsafe native crashes may require a worker process to preserve supervisor availability.

A remote call transfers contract values, not file capabilities, local task handles, raw pointers, or local resource owners. Application errors remain typed values. Transport/authentication/timeouts are declared execution errors. Retry and idempotency policies are explicit, and response loss does not prove the remote operation never ran.

GPU dispatch has the same logical parent-task ownership but additionally depends on command completion and synchronization. Group cancellation cannot immediately reclaim or recycle a buffer whose device users remain active. Device loss and reset use backend-confirmed lifetime rules. A bounded diagnostic result and resource quarantine may be necessary when normal completion cannot be established.

A tensor buffer can remain alive as an ordinary managed value after a completed group. That differs from a borrowed native pointer or scoped external handle. Typed values and resource capabilities must not be conflated by serialization or task admission.

## Validation before implementation

1. Spawn evaluates parent arguments once and submits work before await.
2. Multiple child calls progress concurrently within configured limits.
3. Try/catch on a parallel result preserves expected error types and always settles children.
4. Repeated await reads a settled result without rerunning work.
5. Unobserved children, escaping handles, and unsafe resource captures are diagnosed.
6. Expected application errors may be handled; execution faults fail the owning group.
7. Nested groups progress with a single worker and inherit root cancellation/admission limits.
8. Cancellation stops admission and releases data only after confirmed completion/termination.
9. A file callback returns independent managed bytes and rejects retained scoped handles/views.
10. Resource concurrency metadata prevents sharing a mutable external cursor as if it were pure data.
11. Cleanup failure preserves primary diagnostics and exposes explicit flush/commit failures.
12. Native callbacks and device commands do not outlive borrowed resources unnoticed.

## Review and next step

The explicit `parallel` / `spawn` / `await` model, observation of child results, scoped resource helpers, and inline `try await spawn` grouping are accepted. Complete grammar and implementation follow the remaining type and execution-boundary decisions.

[Variants and error payloads](variants.md) are accepted. [Tensor types and shapes](tensors.md) are accepted. Next, review [placement and transfers](placement.md), then the CPU/GPU/remote compute examples before beginning the MLIR/native vertical slice.
