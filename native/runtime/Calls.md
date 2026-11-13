# Resumable function calls

Ordinary calls and scheduled tasks have different lifetimes. `Task::call` executes
an ordinary suspending function in the current task. `Task::spawn` admits a new
task into an explicit source group. Native synchronous numerical calls continue
to use their existing checked/native ABI and do not acquire resumable frames.

## Frame protocol

A resumable call uses the existing trusted `TaskEntry`, `Packet`, and `ValueLayout`
descriptors. The caller retries the same call instruction after suspension:

1. The runtime constructs an independent callee frame, copying arguments from
   the packet's source invocation before the packet goes out of scope.
2. A false result suspends the caller. The caller's context and frame remain
   valid until its resume callback returns. Only then does the runtime select the
   callee as the task's active frame.
3. Callee completion copies the result into a retained outcome. Returning to the
   caller is also deferred until the callee's execution lease ends.
4. The retried call copies the result into the caller's active ownership scope,
   then destroys the callee frame and outcome. Compiler value-scope commits
   publish the result into the destination slot.

Push, resume, return, and destruction do not recurse on the C++ worker stack.
Each scheduler dispatch owns one execution lease. The task's cancellation token,
external-completion capability, and timer state remain attached to that task,
independent of the active function frame. The trusted entry callbacks must return
immediately after a call reports suspension.

## Ownership and scheduling scopes

Each call has independent execution/outcome invocations. Argument/result copy
callbacks retain managed dependencies before publication. Borrowed handles keep
their logical owner; owned parameters enter the callee's resource scope. Returning
an owned handle uses the existing resource escape and receive protocol.

Calls inherit the caller's lexical scheduling group. They allocate neither a task
record nor a group. Explicit `parallel` scopes created inside a callee still have
normal parentage, admission limits, and fault behavior. A completed callee must
have settled and released its own groups before returning. Malformed trusted
callbacks that leave groups open fault their task; task cleanup drains those
groups before destroying their frames.

On cancellation or fault, outstanding source groups and external access settle
before frame destruction. Cleanup retires frames iteratively, deepest first.
Late external completions retain their existing generation/epoch checks. Call
frames do not add a recoverable task-error boundary: application errors retain
the declared error channel, and execution faults/cancellation retain the enclosing
task's precedence rules.

## Memory bounds and tail calls

`TaskLimits::callBytes` defaults to 64 MiB per core/invocation. This is separate
from task-record admission storage. It accounts for call descriptors, aligned
frames and outcomes, and backing/environment reservations in call invocations.
Exhaustion produces allocation fault 2. There is no separate maximum call count.
`TaskStats` exposes current/peak call bytes and frame counts for qualification.

A compiler proof can mark a call as tail-positioned only if all remaining control
paths transparently forward its result. It rejects cycles, additional computation,
collection cleanup, handle resources, and remaining scheduling-scope cleanup. The runtime
also requires an identical result layout and no outstanding groups owned by the
retiring frame. A failed runtime check keeps the ordinary call frame. A successful
tail replacement first retains the new arguments, waits for the old lease to end,
and then retires the old frame. The original return identity survives mutual tail
calls so that the waiting caller still validates the correct call site.

`drain(steps)` remains a bounded host-controller operation. Standalone entries and
closed runners use `drainToCompletion()`, which continues productive batches and
rejects a stalled graph without an external progress source. A scheduler slice
budget is not a source-program execution limit.

## Persistent borrowed frames and retirement

`Task::callBorrowed` temporarily executes independently owned continuation
storage on the current task's call stack. Its physical allocation remains pinned
through result publication and the final execution-lease callback. It does not
construct or destroy the producer payload, allocate a task record, or retain a
dormant caller's cancellation token. The entry callback binds cancellation only
for the active advance and removes that binding when the lease ends.

A `BorrowedGroupOwner` gives the producer's structured groups a stable identity.
Completing an advance detaches those groups from the advancing task; their
children continue running. A later advance reattaches them. The control header's
immutable weak core binding survives settlement of producer locals and rejects
use through another application runtime.

Asynchronous retirement uses an intrusive cleanup job in that control header.
The owning task enters `WaitingOwned`; retirement consumes neither a new task
admission slot nor an ordinary call frame. Each job cancels, joins and disposes
its producer groups, queues owned child producers, waits for them, and finally
destroys its payload. Native control headers remain alive under queue pins.
The root fence remains pinned until all siblings and descendants finish, even
when its own payload finishes first. Maintenance runs bounded batches, with
callbacks outside the scheduler mutex. An external completion must acknowledge
cancellation before its producer payload can be destroyed.

`Task::leaveOwnedScope` moves an escaping result to its enclosing logical scope
before starting retirement. The generated continuation retries the same leave
instruction after suspension. The scope's files, captures, ownership stores and
budget observers remain alive until the entire fence settles. Task success,
fault and cancellation also preflight execution scopes before frame destruction
or public completion. A quiescent settling task may retire its own borrowed
payload; an independently active mutable lease cannot be retired.

Native producer wrappers can provide a `stageCompletion` hook on their borrowed
entry. A generated body's completion then retains its typed result without
publishing it or detaching groups. The wrapper distinguishes an item from a
terminal result and publishes explicitly through `completeFrom(..., true)`.
Only an entry with the `terminalCleanupAllowed` proof can use that publication
or begin self-retirement. Its control header must survive payload destruction.
`beginBorrowedCleanup` first waits for the current resume execution lease to end,
including the scheduler's status check, before destroying the producer body.
After retirement the native wrapper may publish its independently retained
result; it must not re-enter the destroyed source payload. Cancellation and
faults while waiting settle the same cleanup fence before task publication.

Owned asynchronous captures must be retained directly in their logical scope's
ownership store. Retaining the same dynamic resource first in a temporary store
and then in its logical store creates two conservative accounting paths. Repeating
that pattern through nested producers can amplify reservations despite sharing
physical backing. Separate fresh body and staged-outcome invocations may bind to
one generator's aggregate resource budget; rebinding an existing invocation is
invalid. Their ledgers must settle before their control header is destroyed.

These private primitives do not by themselves lift source `yield` expressions
into resumable producers. A producer entry must supply its own suspension and
typed-result protocol, preserve its native header during payload retirement, and
arrange cleanup before exposing a returned or abandoned owned result. Destroying
a standalone foreign `Invocation` is not an asynchronous scheduler barrier.

`ensureOwnedEscapeScope` admits a root logical owner beneath existing function
and value controls. It reparents the outermost resource scope without changing
which function scope is current or destroying existing controls. Returning an
owned value can then move it into this root instead of leaving it unowned before
result-copy admission. If destination admission fails, the source still has an
owner available for failure cleanup. Root admission rolls back while its control
is still newest; only a fully constructed root is moved below existing controls.
An asynchronous producer factory must admit this prerequisite before taking
ownership of captures. Result transfer and outcome disposal must still use their
own ownership and asynchronous cleanup protocols; root admission alone does not
implement those operations.

Discarding an unread task result preflights its owned outcome scopes before any
result value, parent frame, or ownership ledger is destroyed. Generated group
release instructions park the same task and retry after cleanup; they consume no
additional scheduler admission slot. Once disposal begins, new result reads and
ownership transfers are rejected. Existing readers finish before disposal starts.
A producer cleaning its own groups attaches unread result producers as children
of the same retirement fence. Readiness checks the scope's direct members rather
than the global fence count, so the parent does not wait on its own completion.

Host `TaskCore::release` returns false while asynchronous result cleanup is
pending; the host drives `step`/`drain` and retries release. It must still deliver
any outstanding external completion acknowledgements. Core destruction keeps
workers alive while disposing unread outcomes and draining the cleanup those
outcomes create. It cannot substitute for an external kernel acknowledging its
completion. Cancellation arriving during frame destruction also preflights a
previously successful owned outcome before publishing cancellation.

## Mixed GPU host callables

Metal, Vulkan, and CUDA supervisors use the same call runtime. Resumable host
callables identify compiler-generated task entries rather than native synchronous
function pointers. Their capture environments use the existing checked recursive
backing graph, including code-bound identity and ownership validation. Helper result envelopes
carry returned `TaskError` values separately from the task's execution failure,
so a caller can catch a real child task's returned failure without manufacturing
a new task boundary for an ordinary call. Native layout bridges construct, validate, and retain those environments; extracting
multiple captures commits all affected ownership slots in one transaction.

Functions that transport resumable host callables remain in the host continuation
projection, including factories and callback arguments. Synchronous CPU exports
keep their native callable identities. Host scheduling/call frames are separate
from device call frames. This mechanism neither executes host I/O inside kernels
nor substitutes CPU execution for a device function.
