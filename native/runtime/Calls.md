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
collection cleanup, handle resources, and lexical scheduling groups. The runtime
also requires an identical result layout and no outstanding groups owned by the
retiring frame. A failed runtime check keeps the ordinary call frame. A successful
tail replacement first retains the new arguments, waits for the old lease to end,
and then retires the old frame. The original return identity survives mutual tail
calls so that the waiting caller still validates the correct call site.

`drain(steps)` remains a bounded host-controller operation. Standalone entries and
closed runners use `drainToCompletion()`, which continues productive batches and
rejects a stalled graph without an external progress source. A scheduler slice
budget is not a source-program execution limit.

## Mixed GPU host callables

Metal, Vulkan, and CUDA supervisors use the same call runtime. Resumable host
callables identify compiler-generated task entries rather than native synchronous
function pointers. Their capture environments use the existing checked recursive
backing graph, including code-bound identity and ownership validation. Native
layout bridges construct, validate, and retain those environments; extracting
multiple captures commits all affected ownership slots in one transaction.

Functions that transport resumable host callables remain in the host continuation
projection, including factories and callback arguments. Synchronous CPU exports
keep their native callable identities. Host scheduling/call frames are separate
from device call frames. This mechanism neither executes host I/O inside kernels
nor substitutes CPU execution for a device function.
