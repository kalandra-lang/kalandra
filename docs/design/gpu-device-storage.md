# Invocation-owned device storage

This is the storage design for the remaining array/owned-value requirements in
#279. It is an implementation contract to qualify, not a claim of implemented
array support. The plain-record checkpoint frame is already an inline value
representation; host pointers are not a valid extension of it.

## Storage and permissions

Use a private, invocation-owned device arena with checked, stable word offsets.
Array descriptors carry backing identity, offset, length and element-layout
identity. Capacity belongs to backing storage, not the logical array value.
Offset/length multiplication and addition must be checked before any device
access, including empty slices and zero-length allocations. Descriptor layout is
private and versioned; public source types and their access contracts do not
change. Every typed element has a traversal layout for its owned references.

Assignment preserves array backing identity. `copy` allocates fresh numeric/Bool
or String array backing, preserving exact bits/order; immutable String text may
retain its backing. CPU `copy` does not accept record/function/nested arrays, so
GPU parity must not silently add those overloads. Record updates copy fields by value with
the same owned-field alias rules. Immutable tensor values retain their existing
contract. A device arena is never shared mutable state between invocations.

Captured input storage is read-only and retained until completion is
acknowledged. Writable storage must be local or proven disjoint/exclusive by the
same frontend rules as CPU access. A direct `mut` helper may borrow eligible
local backing; spawning an in-place mutation of shared caller backing stays a
compile-time error. Device storage does not weaken a host task's read lock.

## Growth and checkpoints

The private control kernel gains an explicit arena binding, distinct from source
input buffers and the scalar frame. A kernel first checks available capacity.
If growth is necessary, it records a checked request and the instruction to
retry, then completes its submission without publishing a partial source value.
The completion service admits the allocation against both device and invocation
budgets, preserves existing bytes, and resumes the same device instruction.
Source size calculations and allocation predicates execute on the device. Host
code validates requests, allocates/copies opaque bytes and schedules; it must not
evaluate the source program to simulate device allocation.

Arena offsets survive buffer growth. Both old and replacement storage remain
alive through the actual copy/submission acknowledgement. Allocation failure,
overflow, invalid descriptors and cancellation clean partial backing and produce
the established failure category. No silent CPU allocation of source arrays and
no arbitrary fixed array-capacity limit are equivalent implementations.

The private shader/artifact contract must explicitly identify arena-capable
control code. Existing runtimes must reject an incompatible artifact rather than
spin on an unknown program counter. Existing scalar-control artifacts retain
their ABI. Metadata, root maps, backing, copies, staging and output transport all
count against resource limits.

## Reclamation and recursive ownership

CPU owned values are cycle-safe. Reference counting alone cannot therefore be
assumed sufficient for the GPU. The arena needs typed root maps and a device-side
reclamation strategy covering cycles, not just a bump allocator freed at task
exit. Repeated creation of temporary values in a loop must reclaim dead backing
before gratuitously exhausting an otherwise adequate budget.

Allocation, collection, tracing and array library operations must be resumable
device work with bounded steps. A stop-the-world collector running unbounded
inside one submission would violate the checkpoint design. Host code may copy
opaque arena data for transport, but object traversal, reachability decisions,
sorting, filtering and collection ordering remain device computations. Keep
temporary source values rooted across retries and checkpoint transitions.

## Collections, calls and transport

Eager `yield` appends in deterministic lexical order within one invocation.
Nested collectors have distinct roots and abandon their partial values on early
error propagation. Empty collection has a valid typed descriptor. Per-invocation
collection is not an unordered workgroup append and does not invent atomics or
barriers. `generate`, `map` and sort operations use the same resumable machinery
and existing numerical/comparison policy.

Function values use a checked code identity plus typed captures, never a host
code pointer. Specialization or defunctionalization must preserve captured alias
identity. Recursive calls use an arena-resident, budgeted call stack; language
call depth is bounded by resources rather than a fixed source-call count.

Public aggregate arguments/results require explicit typed transfer and resident
descriptors. A local device record does not establish that transport. Downloaded
owned values must be reconstructed with host ownership descriptors and preserve
alias identity without importing device addresses. Transfer failure must retire
all partially constructed host/device storage after submitted work settles.

## Required qualification

Test all supported element/layout types; duplicate aliases versus copy; slices
and disjoint writes; shared-reader rejection; repeated allocation and cycles;
growth retries and denied budgets; deterministic empty/filter/nested/early-exit
collection; comparator edge cases; function captures and recursive exhaustion;
concurrent invocations; cancellation in allocation/copy/tracing/sort/collection;
and zero retained resources on every exit. Preserve error code distinctions
(including arithmetic, bounds, allocation and budget) and the ordering between
cancellation before acknowledgement and an already recorded fault.

Qualify actual device execution on Metal/Vulkan/CUDA and installed artifacts.
Report resident and transfer/allocation-inclusive timings separately, retaining
slower results. Keep unavailable Windows hardware explicitly unqualified.
