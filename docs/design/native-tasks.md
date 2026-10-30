# Native local task contract

Implementation design — 2026-10-15. This makes the accepted [execution model](execution.md), [error channels](variants.md), and [automatic lifetimes](memory.md) concrete for the native runtime. It is the next implementation contract after the [sequential float32 CPU milestone](native-codegen.md), not a stable public ABI. The checked invocation/ownership foundation (gate 1) and single-worker task core (gate 2) are implemented and tested. The first source task subset (gate 3) now compiles into these resumable frames. The bounded multi-worker CPU extension (gate 4) is now implemented. General application error families and device/remote execution remain pending.

Keep the source model small: immutable values, a parallel group, spawn, and await. Scheduling frames, buffer owners, cancellation tokens, and cleanup machinery remain compiler/runtime details. No allocator parameters, moves, lifetime annotations, or new decorators are introduced.

Current extension: [`parallelMap`](native-parallel-map.md) now provides explicit bounded CPU array mapping through the same scheduler and ownership rules.

## First executable task slice

```kalandra
fn combine(
  left: Tensor[float32, ?, ?],
  right: Tensor[float32, ?, ?],
) -> float32 ! TaskError:
  try parallel:
    first = spawn reduceSum(left)
    second = spawn reduceSum(right)
    a = try await first
    b = try await second
    a + b
```

Both submissions happen before observation; awaiting first does not stop second from running. The ordinary reduceSum remains sequential and preserves the CPU milestone's strict reduction tree. Parallelizing independent calls does not permit reassociating their arithmetic.

Implement local CPU execution first: concrete float32 scalars and ranked dense host tensors, statically resolved named functions and supported built-ins, nested groups, and TaskError. GPU, remote placement, general closures, task collections, resources, arbitrary application error declarations, and a public FFI remain separate increments. Preserve room for typed application errors in the internal outcome from the beginning; do not pretend those language features are implemented by reserving their representation.

An omitted target means local CPU. Accepting devices.cpu() explicitly requires the corresponding selector/type checking; until that exists, diagnose unsupported placement. Never accept a GPU/remote clause and run it on the CPU as a stub.

## Source evaluation and observation

Evaluate the callable, arguments, and optional target once, left to right, in the submitting invocation. Argument evaluation completes before submission. A fault during that evaluation belongs to the parent invocation; no child has been admitted. A submission retains a complete immutable argument packet before making the child runnable. Spawn does not wait for an await to start execution, and does not guarantee the child has started when spawn returns.

A handle identifies one task in its owning group. Aliases identify the same task, not additional invocations. Await observes that task's cached immutable outcome. Repeated await neither reruns it nor consumes its result; each resulting tensor value establishes its own managed storage dependency.

The compiler checks observation by task identity through aliases and supported helpers, rather than requiring every handle variable to be read. Every successful path must observe every child; failure paths may leave observation to mandatory cancellation/join cleanup. Escaping a handle, dropping a spawned expression, or ending a successful group with unobserved work is diagnosed. Initially reject ownership flows the checker cannot establish instead of silently detaching work. A runtime invariant check supplements the compiler; it does not replace static diagnostics.

Prefix grouping remains `try (await (spawn f(args)))`. Ordinary calls and pipelines retain their sequential, once-only evaluation rules. No implicit parallel map is added.

## Ownership objects

| Object                 | Owns or retains                                                                                | Release boundary                                                               |
| ---------------------- | ---------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| Root execution         | Scheduler configuration, shared admission budget, root cancellation state                      | All descendant groups and invocations have settled                             |
| Parallel group         | Child records, completion count, body continuation, tentative body result, failure diagnostics | Body has stopped and all admitted descendants have settled                     |
| Invocation             | Argument packet, execution frame, outstanding allocations and nested groups                    | Execution and all nested cleanup have finished                                 |
| Settled task record    | One initialized immutable outcome, observation bookkeeping                                     | Group teardown, after copying/retaining values still needed by the body        |
| Managed tensor backing | Allocation and initialized contents, retained storage dependencies                             | Final reader/owner releases it; no running task or live value can still use it |

Group ownership and tensor ownership are different. A task handle cannot escape; an awaited tensor can leave a successfully settled group and remain valid. A caller can keep using an input while several children read it. Neither submission nor cancellation invalidates that input.

Use reference-counted backing records for this first acyclic scalar/tensor task subset. Retains/releases that cross threads are synchronized; immutable element data are published before readers receive a reference. Reference counting here does not select the full language's collector or establish cycle reclamation for future closures/records.

Each invocation has an allocation ledger, separate from every sibling and parent. A newly allocated backing has a local owner recorded before any subsequent fallible operation. Sharing across invocations retains the backing rather than transferring its allocation-list node. On publication of a tensor outcome, establish the outcome's ownership before releasing local owners. Fault cleanup releases only that invocation's ownership entries; it never frees another invocation's storage merely because it was allocated earlier.

Within synchronous calls, borrow arguments for the call interval and preserve the existing tensor-return guarantees. Before arguments cross a task boundary, turn any borrowed views into retained managed dependencies or an independently owned copy. Never put a callee-stack address or a naked borrowed memref into a queued packet. The old private test descriptors require trusted ownership; they are not automatically managed task inputs.

Compiler-owned scratch may stay private to a call and use simpler allocation where proven safe. All fault paths must still know which allocations were initialized and which owners were transferred. Partial initialization releases only initialized fields, exactly once. Cleanup is iterative/bounded in stack use, including nested task records.

## Private task ABI

Version the compiler/runtime task interface together, initially as internal revision 1. Reject a revision mismatch at link/startup rather than interpreting incompatible frames. These are logical interface requirements; exact C++ layouts and symbol names follow in the implementation header and are not an external binary contract.

A compiler-generated entry descriptor contains:

- Internal revision, concrete argument/result/error layout identities, sizes, and alignments.
- Checked frame/packet construction and destruction operations, including retain/release operations for managed fields.
- A resume entry taking an invocation context and compiler-owned continuation frame.
- Source metadata and capability/effect requirements sufficient to reject unsupported execution.

Entry identities are resolved within this executable. They are not serializable function pointers or remote deployment contracts. Validate packet size/alignment arithmetic before allocation. A failed packet construction cannot expose a runnable child.

A resume operation reports one of these internal control states:

| State     | Meaning                                                                                      |
| --------- | -------------------------------------------------------------------------------------------- |
| Completed | A fully initialized success or typed application-error outcome is ready                      |
| Suspended | The frame is retained and registered on a dependency; there is no published task outcome yet |
| Faulted   | A checked fault terminates this invocation; no success value is initialized                  |
| Cancelled | Cooperative stop acknowledged; no success value is initialized                               |

These control states are not source error variants. The supervisor converts Faulted/Cancelled into the declared TaskError outcome only after nested work and invocation cleanup settle. A tagged initialized outcome has exactly one success, application-error, or TaskError payload. It has concrete layout identity and type-directed cleanup, not Any or an unchecked byte dictionary. Awaiting a nonfallible child exposes `T ! TaskError`; a fallible child will expose `T ! (E | TaskError)` when that frontend support is added.

Synchronous leaf kernels need no resumable heap frame solely because they can be spawned. An entry adapter calls them with a checked execution context. Calls which may suspend must preserve their callers' continuation state: infer this internal property through the call graph and lower those call paths into resumable frames. It is not a user-written effect annotation.

## Fault propagation must change before threads

The initial CPU runtime used a process-global allocation registry and a noreturn fault handler that freed the registry and exited. Gate 1 has replaced this on source-generated paths with explicit contexts/status and independent ownership ledgers. Keep the existing numerical tests as a baseline. The separate handwritten toolchain proof remains a historical backend fixture, not a task-capable runtime.

The task-capable calling convention passes an execution context and an explicit control status through every reachable checked call. Result storage is initialized only on the success path. Generated guards branch to compiler-generated cleanup/return paths. A caller must inspect status before reading a result. For a suspending call it also preserves and resumes its continuation. Rewrite all task-reachable callees, allocation hooks, and guard sites together; a legacy callee which can exit the process is not task-safe.

The context follows the invocation, including when its continuation resumes on another worker. A worker-thread identity or thread-local allocation list is not an ownership identity. Context-free MLIR allocation/free hooks are rewritten through context-aware checked adapters on these paths; returning null to generated code that assumes allocation succeeded is invalid. Scheduler locks are not held while invoking generated code or running user-visible cleanup.

Use explicit status propagation, not setjmp/longjmp across C++ objects or a C++ exception thrown through LLVM frames without generated unwind support. Task safety must not depend on an unwinding mechanism the backend never emitted. Runtime entry points must contain unexpected C++ exceptions at their own boundary and report an internal failure; cleanup/destruction paths must be nonthrowing.

An Allocation fault must be reportable without allocating its diagnostic. Reserve minimal task/group control and failure storage before admission; bound textual diagnostics and secondary failures. If even minimal parent-side control cannot be allocated, take the parent's checked Allocation fault path and settle its existing children. Do not promise admission can always manufacture another heap-allocated failed handle under out-of-memory conditions.

Diagnostics retained in a completed outcome must outlive the originating frame and group like any other managed value. Emergency diagnostic storage cannot be a pointer into reclaimed stack/frame memory.

A checked arithmetic/shape/bounds/allocation fault is recoverable by the supervisor after terminating the faulting invocation. Segmentation faults, memory corruption, unsafe foreign crashes, and process death do not acquire this guarantee. Do not catch a native signal and continue using potentially corrupted shared memory. A process-isolated executor is separate future work.

A checked fault in a group's body still belongs to the containing invocation: settle its children, then propagate that fault to its enclosing supervisor (or the root program boundary). The group's ordinary catch does not turn a body arithmetic fault into an expected error. A checked fault in a child instead produces TaskError.Fault and fails the child's owning group, as already specified in the language design.

## Lifecycle and publication

Child execution advances through Prepared → Queued → Running, with Running allowed to suspend and become runnable again. It then enters Settling and finally Settled. Cancelling a queued task may skip Running, but still passes through settlement. No transition revives a settled task or admits its invocation twice.

Cancellation is a separate sticky request flag; requesting it is not a terminal state. A running task cannot be marked settled while it still accesses arguments, scratch, results, or descendants.

Before Settled becomes visible:

1. Stop executing source operations and settle all nested groups.
2. Establish ownership of the complete outcome, or install a bounded failure outcome.
3. Release invocation-local owners and packet dependencies no longer required by the outcome.
4. Publish the outcome with synchronization, decrement outstanding group work once, and wake registered continuations.

An early child fault notification can close group admission and request sibling cancellation before step 4. That notification does not authorize reclaiming the faulting child's live frame. All bookkeeping transitions, wakeups, and cancellation races must be synchronized. Never resume a frame concurrently or destroy it while it is queued, running, or registered as a waiter.

## Bounded scheduling and nested progress

Use a fixed-size local worker pool with a bounded ready queue. Await and group join suspend the current continuation and return the worker to the scheduler; they do not park that worker waiting for queued children. This applies transitively through helper calls and nested groups. One worker can therefore execute a parent until it waits, run its child, then resume the parent. The root caller may block while the pool continues, but a worker must not become an untracked blocking root waiter.

Root configuration bounds admitted unfinished tasks, ready-queue capacity, nesting depth, retained packet/frame bytes, and worker count. Nested groups share root budgets rather than creating new pools. Allocation sizing and quotas are checked before admission, with reservations released on every construction/queue failure.

For the first runtime, submission is nonblocking: if a queue or admission budget cannot accept work, produce TaskError.AdmissionRejected and close that group for further submission. It does not silently queue unbounded work or deadlock a worker waiting for capacity. Use pre-reserved failure/control state; inability to establish that state follows the parent Allocation fault rule above. An admitted child which fails validation before execution publishes its failure without invoking the function.

A rejected submission still has a settled group-owned handle when control storage is available, so the source spawn/await shape remains consistent. Admission rejection is a group execution failure; handling that handle inside the group does not reopen admission. A retry belongs outside the settled failed group.

Bounded admission does not guarantee success for arbitrary nesting under arbitrary small quotas. A root limit of one unfinished task can reject a child's own submission while that parent is active. The single-worker progress test must provide adequate task/frame budgets and prove scheduling progress independently of this deliberate quota failure policy.

Fairly revisit runnable continuations and cancellation requests; do not indefinitely drain a single producer's submissions. Arbitrary blocking foreign calls are excluded from the first worker entries. A later blocking executor needs its own bounded resource policy.

## Cancellation, group exit, and error precedence

Insert cooperative cancellation checks at invocation entry, call boundaries, generated loop backedges, and continuation resumption. Include tensor fill/copy and reduction loops. Polling must not change strict arithmetic order on successful completion. Long-running leaf kernels may continue until their next poll; deadlines cannot promise immediate termination.

Cancel queued tasks without executing their function, release their captured dependencies, and publish Cancelled after cleanup. A running child acknowledges cancellation through its normal checked cleanup path. A cancelled child with an active nested group first cancels and settles those descendants. No thread is forcibly killed and no borrowed input is freed early.

| Event                                                 | Required behavior                                                                                                     |
| ----------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------- |
| Body and all observed children succeed                | Retain the body result; settle children; publish group success                                                        |
| Child returns an expected application error           | Cache its original typed error; siblings continue until the parent handles or propagates it                           |
| Body propagates an expected error                     | Close admission, request descendant cancellation, join, then return that typed error                                  |
| Child has an execution fault or admission is rejected | Immediately close admission, request sibling cancellation, settle, then return TaskError                              |
| Parent cancellation or deadline                       | Close admission and propagate the request throughout the descendant tree; settle before returning the execution error |
| Body faults                                           | Settle children, then continue the containing invocation's fault path                                                 |

A body result is tentative until the group settles. A child fault discovered before group settlement prevents success even if another result was already awaited. Catching a child's TaskError inside a failing group cannot erase the sticky group failure. Recovery is outside the settled group. Application errors deliberately handled inside the group do not poison it.

Choose one primary failure under the group's synchronized failure latch; do not promise which concurrent failure wins. Once chosen, sibling Cancelled acknowledgements generated by that failure do not replace it. Preserve bounded secondary diagnostics. If external cancellation/deadline causes failure first, preserve Cancelled/DeadlineExceeded respectively. Resource/cleanup diagnostics must not replace an existing primary failure.

Cancellation is not rollback. No automatic retry, exactly-once effect guarantee, or permission to reclaim storage before completion is introduced. The first CPU slice has no public deadline/cancellation API; internal test controls exercise the same request and settlement protocol before such APIs are exposed.

## Implementation sequence and gates

1. **Checked invocation foundation — implemented.** Replace global cleanup/process exit on task-capable paths with explicit context/status, per-invocation ownership, managed tensor crossing rules, and nonallocating fault reporting. Preserve all sequential numerical evidence. Test multiple simultaneously live contexts before adding threads.
2. **Single-worker task core — implemented.** Implement entry/packet/frame descriptors, group/child state machines, continuation wait registration, bounded admission, publication, and cancellation using runtime tests with synthetic entries. Prove nested progress and race-safe wake registration; no source concurrency claims yet.
3. **Frontend and lowering — first local subset implemented.** Add parallel/spawn/await grammar with accepted precedence, contextual typed results, observation/escape checks, internal suspension analysis, and continuation lowering. Initially expose only local float32/tensor entries and TaskError; diagnose unimplemented constructs explicitly.
4. **Multiple workers — implemented for the current CPU subset.** Shared storage, queue/group synchronization, execution leases, cooperative kernel cancellation, and concurrency tests now pass, including separate ThreadSanitizer runs. Performance work follows this correctness baseline.
5. **Source conformance.** Compile the two-child example and nested source programs; preserve strict numeric bits and all lifetime/failure guarantees in debug, optimized, and sanitizer variants.

Required evidence includes:

- Both calls submitted before either await; once-only ordered argument evaluation; cached repeated await.
- Returned tensors surviving group teardown; parent/sibling shared inputs remaining unchanged; nested tensor-result reuse.
- Static rejection of escaped/unobserved handles and unsupported captures/placement; no dynamic untyped payload escape hatch.
- Nested groups completing with one worker and sufficient quotas; bounded rejection with insufficient quotas; no leaked reservation or phantom child on admission failure.
- Cancellation before dequeue, while running, after publication, and racing await registration; exactly one terminal publication and wakeup protocol in each case.
- Child shape/allocation faults with siblings holding live buffers, descendant cleanup, and an outer group handler; a body fault retaining its containing-invocation classification.
- Application-error preservation/handling once that frontend feature is implemented; no blanket conversion of application errors into Fault.
- Allocation failure at every packet/frame/backing/control construction point; retained bytes and owners returning to baseline after repeated success, rejection, failure, and cancellation.
- Parent continuation resuming on a different worker without losing its invocation context; cleanup never touching a sibling's ledger.
- Concurrent independent groups, primary-failure races, bounded diagnostics, and a full source build with no JavaScript runtime dependency.

Gates 1 and 2 are implemented. The [checked CPU invocation ABI](native-codegen.md) supplies independent storage/fault contexts; [Tasks.h](../../native/runtime/Tasks.h) and [Tasks.cpp](../../native/runtime/Tasks.cpp) now supply the single-worker task core. The first source integration now implements local parallel/spawn/await, TaskError propagation, and continuation lowering. Built-in TaskError recovery and the multi-worker extension below also have compiled conformance tests. Gates 1–4 now pass for the current CPU subset.

## Implemented single-worker core

TaskCore owns stable, generation-checked task/group records and a bounded FIFO ready queue. Its create operation reserves group/task bookkeeping and one emergency rejection record per group. If that initial reservation fails, creation returns failure without a partially initialized scheduler. Entry descriptors are trusted, immutable, and must remain valid for the core's use interval; this is not an arbitrary plugin/FFI loader.

Revision-1 entries describe their concrete packet layout, frame size/alignment, construction/destruction, resume entry, and immutable result layout with type-directed copy/destruction. The runtime checks descriptor revision, packet identity/size/alignment, sizes, and quotas before invoking construction. Constructors copy/retain arguments synchronously before submission returns. Mutable continuation frames remain internal; published result payloads are immutable. An unexpected exception from entry construction/resumption becomes a checked Native fault. Destruction and value-copy callbacks are required to be nonthrowing.

The current limits cover normal task records, group records, unfinished invocations, physical ready-queue slots, nesting depth, and retained frame/result bytes. Pre-reserved control arrays are separately bounded by record/group/queue counts. Tensor backing bytes and allocator overhead are not charged against the frame/result byte limit; it is not a bound on total process memory. Arbitrary over-aligned packet/frame/result layouts beyond max_align_t are rejected by this initial private adapter. No tensor-rank restriction is introduced by this scheduler.

Submission rejects without blocking when a quota is full. Each group has pre-reserved rejection bookkeeping so exhausted normal task slots do not require allocating another error handle. Subsequent submissions into the same failed group may return that same settled rejection identity; no child invocation is admitted or run for those attempts. Admission construction failure instead publishes an Allocation fault and releases any partial frame/result reservations. Failure to obtain nested group-control storage faults the owning invocation; exceeding the configured nesting quota fails its group with AdmissionRejected.

Await and group join register a dependency and return from the resume callback. The event loop serializes completion and wait registration; it tests readiness both on registration and during settlement, covering completion-before-wait and wait-before-completion without losing a wakeup. A suspended parent consumes a task record/frame, but does not hold the executor. If the bounded queue is full when an already admitted continuation becomes runnable, its record retains that state and a rotating scan feeds it into the queue as space opens. Waking existing work does not require a new admission or an unbounded queue.

Settlement is iterative over the bounded tables. A failing group closes admission, marks its descendants for cancellation, and retains the first failure. A queued or suspended cancelled invocation is resumed only for runtime settlement, without re-entering its source callback. A parent cannot publish completion while its owned groups still have unfinished children. Packet/frame destruction and invocation ledger settlement precede task publication. Result backing owners stay with the cached outcome until group release, and reading a result retains its dependencies into the reader's context. Repeated reads do not rerun work.

Successful group closure checks that its task identities were observed. Stale task/group handles are rejected through generation checks when slots are reused. A failure in an inner group can be handled by its owner after join without poisoning the outer group; an unhandled group or checked body fault still settles descendants before the owning task's failure is published. Dropping the core cancels and settles its remaining work before releasing records.

This original deterministic mode remains available with TaskLimits.workers = 1 and explicit step/drain. The multi-worker extension below now synchronizes metadata, drives a fixed pool, and polls generated CPU loops. Physical deadline timers, external blocking calls, and general application-error payloads remain outside this implementation. Test-only deadline requests exercise the distinct DeadlineExceeded outcome without claiming a timer service exists.

The three task-core CTest suites compile both runtime and tests in debug, optimized, and ASan/UBSan modes. They cover 100 repeated nine-level nested calculations, twelve-level chains with a one-slot queue, waits before/after completion, cached repeated reads, failure handling after nested settlement, queued/yielded/waiting-parent cancellation, admission allocation failure, partial managed packet/outcome construction, tensor results outliving group teardown, record/queue/unfinished/byte/depth/group-control limits, stale handles, abandoned results, unexpected host exceptions, and core teardown with retained arguments. Frame/result bytes, retained owner entries, unfinished work, and records/groups return to baseline. These foundation tests use synthetic native entries. The separate source suites below exercise compiled .k spawn/await.

## Compiled source task subset

The [scalar program](../../native/tests/fixtures/task-scalars.k) compiles repeated and inline awaits to 16. The [tensor program](../../native/tests/fixtures/task-tensors.k) returns a completed tensor across a group/function boundary and reduces it to 163. The [nested program](../../native/tests/fixtures/task-nested.k) spawns functions which create their own groups and returns 40. [Shared inputs](../../native/tests/fixtures/task-aliases.k) survive two child readers and a completed group, returning 20. [Lexically nested groups](../../native/tests/fixtures/task-lexical.k) in one invocation return 10.

```sh
cmake --build --preset native
build/native/native/kalandra native/tests/fixtures/task-nested.k \
  -o build/native/task-nested
build/native/task-nested
ctest --preset native
```

The parser accepts parallel blocks, spawn, await, try, and the explicit `! TaskError` return channel. Prefix operations group right-to-left around a parenthesized invocation. The checker tracks task identity through aliases, requires observation of every child on a successful group path, rejects handle escapes/value uses, and rejects discarded or implicitly unwrapped error results. The first task subset requires explicit TaskError channels on named fallible functions; it does not silently infer a nonfallible signature. Bindings in parallel blocks have lexical scope.

Task IR distinguishes handles, completed values, and fallible results while retaining the concrete float32/tensor payload. A spawned function's TaskError result stays an expected result; it is not automatically turned into a child execution fault. Try propagates to the innermost lexical parallel boundary or the fallible function boundary. Propagation closes admission and joins cancelled descendants before returning the original TaskError. A body's own checked fault remains a fault of its invocation; a supervised child fault becomes TaskError.Fault. CLI expected TaskErrors exit 88, distinct from the existing checked-fault exits. These are development reporting conventions.

The C++ compiler emits native LLVM continuation callbacks with a program counter, typed frame slots, group/child handles, and immutable tagged result layouts. Await/join save the resume point and return the worker to the core when pending. Ordinary supported helper calls are expanded into the caller's continuation while preserving their own expected-error boundary; they do not become implicit spawned tasks. Captured arguments are evaluated once before submission and retained into the child context. Repeated awaits read the settled cached outcome. Tensor result copies establish managed backing dependencies before the producer/frame/group can be reclaimed.

Tensor primitives retain the existing MLIR bufferization/ownership/strict reduction pipeline. The compiler creates typed kernel helpers for those operations, then emits native continuation control through LLVM's C++ API. It verifies the final LLVM module and links it directly; there is no interpreter, generated JavaScript, generated C++ source, or runtime compiler. In a task program, saved tensor MLIR describes its numerical kernels; final LLVM includes the continuation control and entry/layout descriptors.

Groups now retain explicit lexical parents as well as task owners. Cancellation of an outer block reaches inner blocks owned by the same invocation, and outer settlement waits for those inner groups. This also applies when an ordinary helper creates its own group within an outer block. The synthetic runtime regression confirms an inner queued entry never runs after outer cancellation.

Four source-task CTest suites run in debug, optimized, ASan/UBSan, and TSan variants. They execute the five source fixtures above, distinguish parent allocation faults from child TaskError.Fault, preserve TaskError through a fallible child which creates nested work, and verify typed AdmissionRejected when a source program exceeds the retained-record quota. Syntax/semantic sanitizer suites also check task forms, typed IR dependencies, repeated alias observation, propagation requirements, and rejected escapes/unobserved results. With recovery and multi-worker coverage, the complete native suite now has 39 tests.

CPU kernels execute concurrently through the multi-worker extension below; single-worker mode remains a progress/conformance baseline. The [CPU scaling suite](../../native/benchmarks/README.md) measures generated kernels through a private C++ adapter. The original float32-only value/entry and continuation-expansion restrictions have been superseded by the later typed values, application errors, callables and [recursive CPU source integration](native-recursive-storage.md#completed-local-cpu-source-integration). Bare source placement and public runtime-input task wrappers remain separate work. Compilation bounds each frame to 20,000 slots and the whole plan to 300,000; scheduler frame/record/group/depth/byte quotas remain checked at admission.

Multiple-worker synchronization and generated loop cancellation polling are implemented below. Next measure native CPU throughput, scheduling overhead, and scaling before optimizing the strict numerical kernels or advancing device backends.

## Compiled TaskError recovery

Catch handlers, exhaustive TaskError/FaultKind matches, named field binding/aliases, and re-propagation of an existing TaskError with `fail error` now lower to native continuation branches. Only the selected body executes. The match subject is evaluated once. A handler may return a float32/tensor fallback or a fallible result; its error channel is flattened with the original success channel. Try/fail within an anonymous handler return from that handler, leaving subsequent enclosing-function statements to run when the combined result is handled.

```kalandra
fn totalOrZero(samples: Tensor[float32, ?]) -> float32:
  result = parallel:
    work = spawn reduceSum(samples)
    try await work
  result catch fn(error):
    match error:
      TaskError.Fault { kind }:
        match kind:
          FaultKind.Allocation: 0
          _: -1
      _: -2
```

The outer handler runs after group settlement. Catching a failed child inside its already failed group cannot restore that group's success. Repeated awaits retain the same failure. A checked fault in the containing invocation or handler still terminates that invocation; catch does not intercept it.

TaskError payloads remain typed values. Fault.kind maps the current shape/allocation/native fault codes to FaultKind.Shape/Allocation/Native. Checked integer arithmetic produces Arithmetic faults; checked numeric array/tensor reads produce Bounds faults. Message/reason bindings reference bounded static diagnostic text (`tensor shape mismatch`, `allocation failed`, `integer arithmetic fault`, `index out of bounds`, `native execution fault`, or `task admission rejected`), not arbitrary exception strings. Their storage has static lifetime. General String construction/operations and public String signatures are not implemented. General error declarations, construction, and unions remain separate future work.

LLVM emits a success/error branch for catch, tag tests for match, and a local propagation exit for the handler. Tensor fallbacks use the existing managed ownership ledger. Conditional shape guards execute only in their selected body. A refinement applied to a fallible tensor copies the failure header and skips the numerical kernel on error; it never reads the missing tensor payload.

Four recovery CTest suites cover debug, optimized, ASan/UBSan, and TSan builds. They exercise [tensor fallback with queued sibling cancellation](../../native/tests/fixtures/task-recovery.k), [newly allocated fallback storage](../../native/tests/fixtures/task-recovery-allocated.k), [different fallback tensor shapes](../../native/tests/fixtures/task-fallback-shape.k), [typed re-propagation](../../native/tests/fixtures/task-repropagate.k), [handler propagation boundaries](../../native/tests/fixtures/task-handler-boundary.k), successful try inside a handler, repeated awaits of a failed child, sticky group failure, real admission rejection, lazy handler-only shape checks, and fallible tensor refinement. Caller and handler allocation faults retain their checked classification and report zero remaining owners. The frontend suites also verify exhaustiveness, invalid fields/types, immutable binders, conditional obligations, and merged fallback shapes.

## Bounded multi-worker CPU execution

The root core now owns a fixed-size worker pool. Generated programs choose the detected hardware concurrency capped at eight workers (at least one). The development environment setting `KALANDRA_WORKERS=1..64` overrides that count; an invalid setting is rejected before root admission. Nested groups share that core, queue, and budgets. They do not create additional pools. Frame/result byte quotas exclude worker stacks, tensor backing, control tables, and allocator overhead; worker and table counts have their own bounds. This is not a total-process-memory limit. The private C++ TaskLimits interface defaults to one worker for deterministic driving; explicit step remains available when drain is inactive.

```sh
KALANDRA_WORKERS=1 build/native/task-nested
KALANDRA_WORKERS=4 build/native/task-nested
```

For more than one worker, create establishes the pool before accepting work. Drain first settles pending state; if no unfinished work, execution/cleanup pump, or preparation remains, it returns without enabling workers. Otherwise it enables the pool and waits for quiescence. The pool stays alive but idle between drains and shuts down before core storage is destroyed. Await/join suspend continuations without parking workers. A synchronized bounded ring and record-linked deferred FIFO supply ready work; admission still rejects when a configured quota is unavailable. Source admission conformance now exceeds the retained-record limit, rather than depending on whether other threads happen to drain a full queue first.

One scheduler mutex protects task/group state, failure selection, admission counters, ready registration, publication, and reclamation. Each resume owns an execution lease until its callback actually returns. Await, yield, and complete can change scheduling state during that interval without allowing another thread to resume or destroy the same frame. Contexts follow frames across workers. Only cancellation tokens and backing reference counts cross invocation ownership as atomic mutable state; the numerical inputs/results remain immutable. Live-owner counters are also atomic so statistics do not race with running kernels. Owner counts during execution are an observational sample; quiescent zero-resource checks are exact.

Entry construction, result copying, frame/value destruction, and invocation settlement run outside the scheduler mutex. Prepared records, active execution/cleanup leases, and outcome read pins keep their storage alive while those callbacks run. Group release refuses a concurrent pinned read. No result is published until callback return, descendant settlement, and frame cleanup have finished. Cancellation arriving before publication discards a prepared success and publishes Cancelled; a previously settled successful task stays cached. The group's synchronized first-failure latch preserves the original Fault/Cancelled/DeadlineExceeded/AdmissionRejected cause.

The host may inspect/cancel a core while another thread drains it. It must retain the core, descriptors, packets' source storage during synchronous construction, and callback state for their documented intervals. Destruction is not concurrent with external host calls, and callbacks must not recursively drive or destroy their core. Root control does not run as an implicit blocking operation inside generated task entries. No arbitrary blocking FFI executor or public source deadline API is introduced.

Generated functions poll at entry and checked call boundaries. Every generated LLVM loop, including tensor fill/copy and balanced reductions, polls on entry and at most every 256 generated-loop iterations. A SIMD iteration can process four independent lanes; this is not a bound of 256 scalar operations. The extra loop counter/predicate avoids a runtime call for every scalar element. A poll sees either the existing checked fault or the invocation's atomic cancellation token. Cancellation returns through the checked ABI without inventing a success payload or a checked Fault; the supervising core acknowledges it and settles all owned storage. Polling is cooperative, not a wall-clock termination guarantee. Strict floating-point operation/reduction order is unchanged.

`--sanitize` now marks emitted functions with LLVM's AddressSanitizer attribute as well as linking the ASan/UBSan C++ runtime. The separate `--thread-sanitize` build marks every generated function for ThreadSanitizer and links a TSan-instrumented runtime. The options are mutually exclusive. Passing Clang sanitizer flags to unmarked LLVM input alone is insufficient; the compiled-worker tests also inspect the post-Clang IR for actual ASan/TSan load/store checks. UBSan covers C++ runtime operations; LLVM ASan covers the generated memory accesses.

```sh
build/native/native/kalandra native/tests/fixtures/task-nested.k \
  --thread-sanitize -o build/native/task-nested-tsan
KALANDRA_WORKERS=4 build/native/task-nested-tsan
ctest --preset native -R 'native-(workers|compiled-workers|source-tasks|task-recovery)'
```

[Worker conformance](../../native/tests/WorkerTests.cpp) proves overlapping callbacks, no frame reentry/cleanup before callback return, cancellation during completion/cleanup, forced cross-thread resumption with preserved storage, simultaneous primary faults, repeated immutable reads, pinned outcomes, bounded drain budgets, and 25 repeated nested calculations across two independent roots with each of 1/2/4 workers. All scheduler-owned bytes, backing owners, records, and groups return to zero.

[Compiled-kernel conformance](../../native/tests/CompiledWorkerTests.cpp) runs real generated tensor kernels on 1/2/4 workers. It requests cancellation/deadlines after observing in-loop polls, verifies unchanged inputs and complete cleanup, and checks that a shape fault cancels its running sibling while an independent root completes normally. Runtime and generated code pass debug, optimized, ASan/UBSan, and separate TSan variants. Source task and recovery suites also run with fully instrumented generated continuations and runtime under TSan; they check worker-count overrides and invalid configuration rejection.

This implements CPU concurrency and bounded cooperative cancellation. The [CPU scaling report](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-cpu-2026-10-15.md) records warmed numerical-kernel scaling, small-task scheduling costs, and per-kernel allocation counts. Matrix reduction scratch now reuses one buffer per operation, with serial cell loops and independent invocation ownership; the [comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-cpu-2026-10-15-scratch.md) records the reduction to three allocations per measured projection and its throughput effect. Numerical, allocation-failure, cancellation, ASan/UBSan, and TSan conformance pass. The [scheduler profile](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-scheduler-2026-10-15.md) now records uninstrumented phase timings, independent capacity controls, and separate diagnostic event/wait observations. The [idle-wakeup optimization](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-scheduler-2026-10-15-wakeups.md) now suppresses idle broadcasts and skips worker activation for empty drains, retaining active-controller, lease, cancellation, and shutdown wakeups. The [targeted-wakeup optimization](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-scheduler-2026-10-15-targeted.md) now separates worker/controller waiters and reduces broadcasts during active execution. Ready-table sweeps are now replaced by [bounded runnable tracking](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-scheduler-2026-10-15-runnable.md); maintenance scans remain a later target. Detailed lock-contention attribution, source continuation throughput, GPU support, physical deadline scheduling, remote execution, and general application errors require separate work.

## Optional scheduler profiling

`KALANDRA_TASK_PROFILE` enables a private cumulative `TaskProfile` snapshot and diagnostic probes. It must be defined consistently for the runtime and its harness. Normal builds omit the fields, clocks, counters, try-lock probing, and snapshot API. Profiling itself does not change queue ordering or admission quotas. The subsequent idle-wakeup optimization changes notification/empty-drain behavior as described below. The normal lock/wait paths use the existing mutex and predicate-based condition variable through small shared helpers.

The [scheduler benchmark protocol](../../native/benchmarks/README.md#scheduler-diagnostics) defines the counters and their limits. In particular, primary scan counts exclude nested scans; lock counts exclude const inspection and condition-variable reacquisitions; condition-variable wait durations include reacquisition/startup, and can overlap between threads. Diagnostic durations cannot be combined with uninstrumented wall time into a percentage breakdown. A separate TSan CTest runs the profiled runtime with repeated yields, checks exact frame/result allocation/free counts and task resume/publication counts, and verifies zero remaining resources.

## Idle notifications and empty drains

Every guarded state change still advances the revision under the scheduler mutex. Idle notification suppression remains in place; the subsequent targeted-wakeup contract below narrows which waiters are notified during active execution. Fully idle workers wait for drain activation/shutdown, so admission and cached-result observation do not need to wake them. Active notifications now follow the separate worker/controller channels below; queue ordering is unchanged.

The controlling flag must participate even after draining becomes false. Budget exhaustion or lack of runnable work can stop draining while a controller still waits for execution, construction, or cleanup leases. Their return must wake that controller. Shutdown always broadcasts to all worker waiters. Manual host stepping has no blocked drain controller and keeps its existing progress rules.

Empty-drain settlement still runs maintenance before checking unfinished/pumping/preparing counts. It settles closed empty groups and pending failures, then returns without starting the worker pool when fully quiescent. Repeated empty drains do not consume a task budget or wake idle workers. Group/record/byte limits and immutable cached outcomes are unchanged.

Worker conformance now repeats empty root settlement/release and one-step budget drains while yield/completion/cleanup leases are held. The controller must not return early and must resume after each lease unwinds. The profiled TSan test additionally proves zero idle-phase/initial-empty-drain broadcasts. All 39 native suites pass with these checks.

## Separate worker and controller wakeups

The core now has distinct condition variables for workers and the single drain controller. All state/revision changes remain under the same scheduler mutex. Workers wait on activation or revision changes; the controller waits only on `!draining && !pumping && !preparing`. Notifications for one role cannot be consumed by the other role.

Nonempty multi-worker drain activation broadcasts once to workers so independent long-running callbacks can overlap immediately, including workers that start after the notification and observe the active predicate. During draining, an ordinary progress event signals one worker to re-check queue/dependency state. The executing worker continues its own progress loop too. Shutdown broadcasts to every worker, including workers parked on a revision from a previous drain. A worker need not be woken merely to park again when draining ends; its predicate/revision is re-checked on the next activation or shutdown.

Controller notifications use `notify_one` only when its quiescence predicate is true and controlling remains set. This includes the last execution/cleanup/construction lease returning after budget exhaustion has made draining false. Empty drains and idle host operations remain silent. Single-worker host driving needs neither condition variable notification. The redundant worker-loop notification after a successful step is removed: the step's Pump already signals after the lease fully unwinds.

No queue, capacity, allocation, cancellation, deadline, or cached-outcome rule changes. Worker regression now admits two overlapping tasks while another callback is active and spare workers are parked, observes/releases that independent root, then cancels the running root and verifies controller return and full cleanup. Existing activation overlap, bounded-drain/held-cleanup, nested progress, and sanitizer tests remain gates. Probe counters distinguish targeted worker signals, worker broadcasts, controller signals, and waiter returns by role; their timing remains diagnostic only.

## Bounded runnable tracking

The runtime no longer scans every record to rediscover ready work on each step. Admission completion, wait wakeups, and execution-lease return explicitly enqueue eligible runnable records. A record can belong to the ready ring or the deferred FIFO at most once. Running callbacks cannot enter either structure; a yield or wake observed during execution is enqueued only after the callback actually returns.

The ready ring retains its configured capacity and generation-tagged handles. Already-admitted tasks that cannot fit use intrusive previous/next links in their existing records. That FIFO is bounded by record count and needs no transition-time allocation. Deferred records are unlinked before settlement or slot reconstruction. Promotion validates runnable state and lease/queue membership. Cancellation preserves a deferred task until it is dispatched to acknowledge cancellation, and duplicate wakeups do not duplicate FIFO entries.

Deferred FIFO promotion happens at dispatch boundaries, before choosing work and after the callback/settlement phase. Crucially, popping a task does not immediately refill its vacancy before its callback runs. With `ready=1`, that callback must still be able to spawn a child. New admissions continue to use the ring's actual capacity check and may take that vacancy even while older continuations are deferred, matching the existing admission behavior; concurrent construction completion can itself defer an already-admitted record. Deferred continuations retain FIFO order among themselves, and later yields do not overtake older deferred records. This is not a new global execution-order guarantee for parallel tasks.

Stats now expose ready-ring and deferred counts for exact quiescent checks. Probe `readyVisits` stays zero because the full ready sweep is gone; `runnableEvents`, `readyPushes`, `pendingPromotions`, and `peakPending` describe event-based tracking. Maintenance/dependency/owner/reclamation scans are unchanged and remain measured separately.

Worker conformance explicitly fills a one-slot ready ring while three callbacks hold yield leases. After lease return, it checks three deferred continuations, FIFO dispatch, unchanged full-queue admission rejection on an independent root, cancellation/deadlines across ready/deferred tasks, zero scheduler resources, and stale handles after slot reuse. The existing depth-12 one-slot child-spawn/await chain remains a compatibility gate for promotion timing. Wider concurrency, nested source tasks, and sanitizer suites remain required.

## Occupancy-indexed maintenance

Three fixed bitmaps track live task records (including reserved rejection records), unfinished ordinary tasks, and live groups. They are allocated at core creation and updated under the scheduler mutex. Admission sets task membership before invoking construction; publication clears unfinished membership only after execution/cleanup leases unwind. Reclamation clears live membership before reconstructing the record or group with its next generation. No transition allocates index storage or changes admission limits. The bitmaps are control metadata outside the frame/result byte quota, like the existing record/group arrays.

Primary maintenance examines unfinished tasks and live groups. Nested owner/child checks, failure propagation, and group disposal also walk indexed live entries instead of every reserved record. Iteration keeps increasing slot order and reads the current bitmap on each advance. It never caches a pointer into a mutable list across an unlocked constructor/destructor: another thread may admit, reclaim, or reuse unrelated slots during that callback. Changes behind the cursor are handled by the existing repeat-until-stable maintenance loop, as with the previous increasing-slot sweep. Generation checks, cleanup/read leases, cancellation acknowledgements, and ready-ring promotion are unchanged.

This reduces capacity scans; it does not implement direct dependency subscriptions. Empty bitmap words still require traversal, and maintenance still examines live entries repeatedly. Admission free-slot searches, public statistics inspection, and core destruction retain their separate table scans. Primary visit probes count actual task/group entries, excluding bitmap-word traversal and nested passes; historical reports counted all reserved entries instead.

Conformance crosses 64-bit bitmap boundaries with depth-70 nested task/group waits, with and without yielding before awaiting and with a one-slot ready ring. Sparse-slot tests retain unfinished tasks/groups above slot 64 after reclaiming lower words, then exercise cancellation, deadlines, high emergency rejection slots, full cleanup, repeated slot reuse, and stale handles. Existing concurrent construction/cleanup, pinned reads, active admissions, generated-kernel cancellation, and sanitizer suites remain gates. See the [paired maintenance measurements](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-scheduler-2026-10-15-maintenance.md).

## Task maintenance with no waiters or settlements

`maintenanceTasks` counts records in `WaitingTask`, `WaitingGroup`, or `Settling`, under the scheduler mutex. Entering a wait increments it; waking a waiter decrements it. Constructor failure enters settlement and increments it. Finishing a task increments it only when that record was not already waiting/settling. Publication decrements it after cleanup and execution leases finish. Converting a wait into failure settlement preserves membership. Quiescent resource checks require this count to return to zero.

Group maintenance always runs. After that phase, if the count is zero, maintenance skips the primary task scan: prepared, runnable, running, and already-settled records cannot advance it. If group settlement changed anything, the existing repeat-until-stable loop continues normally. A waiting callback still holds its count even while its execution lease is active, and a completion held during cleanup stays counted until publication. No callback can change the count outside the mutex. No transition-time allocation or admission policy changes are introduced.

Bitmap advance also returns the requested slot directly when it is occupied, avoiding a bit-position decode along contiguous runs. When task scanning is needed, both sparse and dense tables retain increasing-slot indexed traversal. Diagnostic `taskPasses` and `idleTaskPasses` sum to maintenance passes; primary visit counters exclude skipped task passes and bitmap-word reads. Live dependency passes and separate admission/statistics/destruction scans remain.

Nested-chain tests cover 16/32/33/128 task capacities and depths 12/20/70 where capacity allows, with one-slot ready rings and yield-before-await. Deterministic checks cover waiter entry, child publication/wakeup, cancellation/deadline wakeups, and final zero counts. Held-yield/completion leases and concurrent active admissions run with four task slots as well as default capacities. The [three-way comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-scheduler-2026-10-15-dense.md) retains the preceding indexed build and older pre-index build as controls. It also preserves the initial contiguous-scan experiment, which did not fully resolve the measured slowdown and is not the shipped traversal.

The [CPU SIMD microtile comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-cpu-2026-10-15-simd.md) now applies four-column tiles within an invocation. Scratch remains private, and generated vector/tail loops retain cancellation polls and checked cleanup. This does not add scheduler parallelism or change task admission budgets.

## External completion leases

Native task entries can now call `Task::suspendExternal()` after preparing an external operation. It reserves a record-local, epoch-tagged completion capability and changes the task to WaitingExternal. Cancellation already recorded under that same mutex wins without creating a lease. After reservation the backend must acknowledge completion on every path. The current Metal bridge treats this reservation as the submission decision before committing its prepared command.

A foreign callback signals the copied `ExternalCompletion` token after external access has ended. Signal only updates bounded scheduler state and wakes waiters; it does not execute the task continuation. Completion before the original resume callback returns cannot re-enter its frame. Tokens use weak scheduler ownership, task generations, and suspension epochs, so duplicates, previous suspensions, recycled records, and destroyed cores do not acquire a new task accidentally. There is at most one active external lease per record.

An external lease pins frame cleanup independently of task state: a latched fault, cancellation, or deadline can mark the task unsuccessful but cannot reclaim its storage until acknowledgement. The existing maintenance count includes WaitingExternal, and a separate externalPending count drives waiting/quiescence. Worker pools wait for external events without occupying execution leases; single-worker drain parks only its host controller when necessary. Step remains nonblocking. Drain budget bounds execution steps, not time spent waiting for an external acknowledgement. Source deadline timers and GPU preemption remain absent.

The [external completion corpus](../../native/tests/ExternalTaskTests.cpp) runs with 1/2/4 workers and one-slot ready rings in debug, optimized, ASan/UBSan, and separate TSan suites. It checks immediate, foreign-thread, duplicate, stale, and repeated-epoch signals; live execution leases; faults/cancellation/deadline cleanup; worker availability; and destruction waiting for acknowledgement. CPU-only configurations now have 43 tests. The [Metal transfer bridge](../../native/metal/README.md) uses this facility for real upload/download commands and has its own optional four-variant conformance tests.

## Mixed CPU/GPU task integration

The optional [Metal source target](apple-gpu.md#cpu-tasks-in-mixed-programs) now submits ordinary pure CPU functions through these same bounded task entries. Capture construction retains host Buffers and copies scalar values before publication; each child uses its own checked CPU invocation, and results keep the existing scalar/Buffer layout and ownership rules. Unplaced spawn selects CPU; an explicit CPU device or alias is also accepted. Transfers remain explicit. This target-specific bridge does not yet unify placement/residency with the common typed task IR, or admit nested task creation and source recovery inside its CPU function bodies.

The [mixed Metal supervisor](apple-gpu.md#nested-parallel-scopes-in-mixed-programs) now opens nested source groups with their actual lexical parent and resumably joins each block. Group failure settles through the existing task runtime. Completed scalar/tensor results may escape the block; task handles may not. Inner task obligations are checked before closing the block, and outer handles may be observed by an inner block of the same owning supervisor. This does not yet admit asynchronous helper bodies; scoped source recovery is covered by the next increment.

The [mixed target now handles settled TaskError results](apple-gpu.md#scoped-taskerror-recovery-in-mixed-programs), including exhaustive matching and handler-local propagation. `Task::fail(Failure)` carries the original variant/code/location into terminal task settlement without converting cancellation, deadlines or admission errors to execution faults. Invalid tags become checked faults; an existing invocation fault or cancellation takes precedence. This supplements `fault`, which continues to report a fault in the currently executing unit.
