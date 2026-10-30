# Closed synchronous GPU entry

Standalone Metal programs now enter through an opaque `ClosedTaskRunner`. A
serial, fully proved float32 region can execute as one GPU command without
exposing a cancellation capability. Source parallelism and ordinary `TaskCore`
embedding retain their existing scheduling and cancellation behavior.

This addresses the command-handshake cost identified by the
[lower-bound experiment](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/gpu-command-lower-bound-2026-10-29.md).
That experiment was instrumented and deliberately lacked a cancellation proof;
its timings are not acceptance results for this implementation.

## Why a separate owner

A normal `TaskCore` can receive cancellation from its host even when its ready
queue is empty. It therefore never qualifies for synchronous execution. The
closed runner privately owns its core, serializes host calls, and exposes no
core pointer, group handle, task handle, or cancellation operation. Its statistics
are read-only. `run` is a host entry operation, not a recursive scheduler entry;
source recursion continues through the normal compiled function mechanism.

The generated standalone `main` uses this owner. Generated `TaskEntry` values
remain usable with ordinary cores, so embedding the same program does not lose
cancellation. The reusable Metal overload returns `expected<Buffer, Failure>`;
scalar results use their actual value layout through the generic runner rather
than being reinterpreted as a buffer.

The runner copies a successful result into the caller's context before closing,
draining and releasing the root group. Escaped backing remains owned by the
result. It preserves group failure before child failure, reports observation
failure, and cleans rejected admissions. A productive execution can cross the
ordinary `drain` batch budget; that budget is not a language execution limit.

## The progress proof

`Task::canExecuteSynchronously` requires, under the scheduler lock:

- The private closed owner, with the current task executing on this thread.
- One running task, no pending constructions, no external operations, and no
  existing cancellation, invocation fault, or shutdown.
- Every other live task waiting on a task/group or already settled, with no
  executing frame, active cleanup, reader or external lease.

Waiting ancestors are safe: they need this task to make progress. A runnable
sibling, a still-executing parent, a source canceller, or an external sibling
rejects the proof. With no external control capability and no other progress
source, no new independent work can appear after unlocking. The trusted runtime
leaf must not publish new work or capabilities before completing its region.

The scheduler currently receives deadlines through explicit host/source
cancellation; it has no autonomous timer queue. Adding timers must invalidate
this proof while a timer can fire. This is not a promise of real-time GPU
latency or isolation from other processes using the device.

## GPU admission and lifetime

The Metal runtime additionally requires the existing admitted, compiler-costed,
straight-line nonfaulting float32 plan. Shape, epsilon, memory, metadata and plan
size checks run first. Missing proofs, long plans, other types and constrained
budgets retain their existing fallback. No arithmetic policy is relaxed.

Only this admitted region omits the intermediate command-work checkpoints.
Cost saturation, fusion legality and the generic batching work limit remain
unchanged. The same shaders execute the same operations in the same arithmetic
order. CPU encoding finishes before the one command is submitted, so parameter
storage is never rewritten while that command is running. This path does not
reserve credits for nonexistent later commands.

The isolated execution lease may wait for GPU completion. The frame and its
external completion lease stay pinned through the actual acknowledgement;
only then are status, profiling counters and ownership handed back to the
scheduler. Device failure still takes precedence over a later encoding failure
for work already submitted. A general task worker never takes this wait path.

`Options.closedSynchronous = false` is a private qualification control using
the same runner and bounded command policy. It does not alter source semantics.

## Qualification

The dedicated runner fixture covers public-core rejection, serial waiting
ancestors, runnable/running/external siblings, source deadlines, owned results,
failed observation, admission rollback, concurrent host callers, and more than
100,000 scheduler steps. The GPU fixture compares every output bit with the
staged reference across shapes, captures and worker counts; retains results
across later calls and runner destruction; injects every allocation failure;
holds device completions; checks fault precedence; and tests missing-proof,
long-plan and parallel-source fallback.

Qualification covers all 1,407 native tests. The initial full run exposed stale
cancellation-observer, completion-prefix and upload-command assumptions in older
fixtures; all 62 affected cases passed after test-only repairs. Original failures
and the targeted rechecks are retained in the qualification evidence. The new
runner and prepared GPU fixtures pass all four build/sanitizer modes.

The frozen acceptance sweep checks all output bits for 13 regular/odd shapes and
all upload modes, including the streaming fallback, against the previous runtime
and an independent numerical oracle. CPU LLVM and Metal shader artifacts are
unchanged. Unprofiled paired, policy, ordinary-API, replication, streaming and fresh-reference
studies are complete in the [acceptance report](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/gpu-closed-entry-2026-10-29.md).
Largest resident time falls 43.32% in the primary comparison and 43.38% in
replication. Small cases remain mixed, including a −4.02% same-binary policy
control. Ordinary API medians range from −1.85% to +1.38%. Fresh largest-resident
Metal takes 1.57× MPSGraph's time; MPSGraph is a reference, never a production
dependency. The [encoder diagnostic](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/gpu-dispatch-counter-summary-2026-10-29.md)
is instrumented and does not replace these accepted timings.
