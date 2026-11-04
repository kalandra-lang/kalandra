# Checkpointed scalar GPU control flow

This is the first device-language extension in #279. It supports numeric/Bool
scalar state, local plain-data records/enums and numeric results, `while`, integer-range `for`, lexical exits,
and single-result loop expressions across Metal, Vulkan and CUDA. It does not
complete #279: device arrays, collecting `yield`, tensor loop state, owned values
and recursive device calls remain outside admission. Scalar helpers compose with
ordinary immutable tensor graph operations.

## Execution and private ABI

The CPU continuation planner already defines scope, value snapshots, checked
ranges and lexical exits. Device planning reuses that CFG, expands nonrecursive
scalar helpers, and rejects task/owned/recursive instructions. The CPU planner's
default behavior is unchanged; device inlining is an explicit internal option.

A `ScalarControl` numerical-program instruction launches a compiled device state
machine. It executes at most 256 CFG instructions per submission. One active
thread owns its scalar state; this is not shared workgroup mutation. The program
counter and scalar bit carriers live in a private writable parameter buffer.
Zero marks a fresh frame, one-based PCs identify instructions, and UINT64_MAX
marks completion. Input capture buffers stay immutable. Range validation returns
the initial index, rejects a zero step before testing emptiness, and clamps final
advancement overflow to the stop, matching CPU behavior.

Completed GPU state is copied into invocation-owned storage and used in the next
submission. The host copies bits and schedules work; it does not evaluate source
arithmetic or loop predicates. No total iteration limit, device heap, GPU call
stack or implicit CPU fallback is introduced. Numerical policy remains explicit.
An infinite loop stays cancellable between finite device submissions.

A chunk's provisional scalar output is discarded until its PC reports completion.
Checked fault flags are inspected before accepting a chunk. Only then may the
runtime publish a final result or schedule another chunk. No other invocation or
prepared executable shares mutable control state.

Plain-data records flatten into private 64-bit leaf cells in canonical field
order; enum tags occupy their existing synthetic first field. Narrow numeric
leaves retain their typed bit carriers and Bool leaves normalize on scalar reads.
Nested field copies snapshot all source cells before writing a destination.
Construction clears inactive enum payloads. The nominal layout graph is walked
iteratively; recursive/owned fields require the separate device-storage layer.
Compilation charges expanded frames/copies against a work budget and rejects ABI
overflow before generating huge shaders. No aggregate pointer crosses the
host/device boundary: entry parameters/results remain scalar, and host-continuation
or CPU bridge aggregate transport is rejected explicitly. Internal inlined helpers
may pass/return plain aggregates without exposing that transport ABI.

## Lifetimes, budgets and cancellation ordering

Control instructions remain outside numerical fusion/preencoded progression.
Each submission keeps its input buffers, parameters and output alive through
actual completion. Acknowledged temporary storage is retired before further
chunks; live storage depends on frame size rather than elapsed iterations.
Parameter/state copies count against runtime metadata/argument limits. Initial
control frames are zeroed and cannot be overridden by arbitrary capture words.

The existing task cancellation ordering is preserved. Cancellation before an
external result is acknowledged prevents further source execution; an unobserved
GPU fault flag does not override that cancellation. A checked fault already
recorded in the task/group remains primary over later cancellation. Submitted
work must finish before reclamation in either case. The chunk bound is a bound on
compiled control instructions, not a wall-clock deadline or GPU preemption.

## Validation

Use equivalent CPU code and independent expected results, and inspect the emitted
shader for actual device loops. Qualify nested traversal, repeated resumption,
conditional mutation, short circuiting, range endpoints, all ten numeric types,
lexical exits and loop-result expressions. Cover infinite-loop cancellation,
reported faults versus cancellation before acknowledgement, concurrent invocations,
allocation failures, memory budgets and final resource counters. Hardware-less
package checks compile real device artifacts and explicitly report compile-only
status; physical execution is a separate qualification requirement.
