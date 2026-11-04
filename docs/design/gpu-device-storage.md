# Invocation-owned device storage

This is the storage contract for #279. Invocation-local numeric/Bool arrays and
ordered collecting loops use its arena and a collector for reference-free
payloads. Local acyclic records/enums may contain these arrays; their live inline
owned leaves are roots, preserving array aliases through allocation checkpoints. Owned-reference layouts, cyclic values and typed entry transport
remain required work. The plain-record frame remains an inline value
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
An asynchronous upload of mutable CPU arrays is itself a reader: every alias
must remain read-locked until the transfer actually settles. Cancellation alone
does not release that lock. Downloaded copies have fresh host backing, with
alias relationships within the transferred object graph preserved.

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
opaque arena data for transport, but liveness traversal, reachability decisions,
sorting, filtering and collection ordering remain device computations. Keep
temporary source values rooted across retries and checkpoint transitions.

### Selected reclamation algorithm

Use nonmoving, incremental mark-and-sweep. Handles are stable arena offsets, so
collection does not invalidate aliases or require pointer rewriting. Each block
has a checked span, typed traversal-layout identity and mark/scan state. Free
spans are split for allocation and coalesced during sweeping. When no free span
fits, collect before asking the host for larger backing. Fragmentation, peak
growth storage and representation overhead remain real resource costs.

The mutator pauses at an allocation safepoint. Compiler-generated CFG liveness
maps identify live owned slots at that PC; active caller frames, collectors,
captures and the allocation instruction's operands are additional roots. A slot
being overwritten is not kept alive merely because its old bits remain in the
frame. This is necessary for loops that repeatedly allocate temporary arrays
under a steady memory budget. External read-only buffers are retained captures,
not arena blocks to trace or free.

Mark roots, enqueueing newly grey owned blocks in an intrusive work queue.
The existing 64-bit mark word stores a two-bit colour and the next stable handle;
the arena's checked 32-bit word limit leaves sufficient link bits. Reference-free
blocks become black immediately. A pop removes one grey block and makes it black
before tracing its children, so self-edges and cycles cannot enqueue it again.
Private invocation cells retain queue head, active block, element and field
cursors. No auxiliary allocation is needed during collection under memory
pressure. This replaces the earlier proposed repeated whole-heap grey scans;
typed-edge work scales with visited blocks and fields rather than heap-order
rescans. Free-block searches and clear/sweep passes still scan physical blocks.

Repeated payload layouts specify element stride and owned-reference offsets.
Enum references have a tag-cell/value guard; inactive payloads are never treated
as handles. Every tracing step visits one field or pops one block; root scanning
and sweeping are similarly resumable. Layout IDs, spans, lengths, stride
multiples and reference bounds are checked before access. Compiler construction
and typed transfer validation must establish that handles identify actual block
starts, not arbitrary payload offsets. Tracing is not a substitute for that
transport validation. No source mutator runs during collection.

The tracing emitter passes independently generated cyclic graph oracles as
CPU-lowered IR and on physical Metal, Vulkan and CUDA. Source-language ownership
admission remains a separate gate: reference-free source arrays remain the
currently qualified production scope. Source-level recursive owned
values, typed transport, capture/call-frame roots and physical GPU tracing tests
are still required; isolated emitted-IR tests do not establish those capabilities.
This changes private collector representation, not cancellation or source
semantics: collection consumes the existing bounded control quantum and may be
cancelled between submissions. No wall-clock preemption is promised.

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
Transport may pack/unpack typed ownership metadata; that is separate from
executing source algorithms or choosing garbage-collection reachability on the
CPU.

Scalar Bool arguments/results now preserve their source type across the task
boundary. The shader uses a private UInt8 carrier, while prepared programs retain
an explicit Boolean-result tag and publish through `boolScalarLayout()`, whose
identity differs from numeric UInt8. Completion validates the byte before
constructing a C++ Bool; noncanonical bytes produce Invalid rather than false.
Preparation failures retain the Bool task descriptor. Awaiting repeatedly or
moving a Bool between CPU/GPU tasks does not reinterpret its layout. Source
checking rejects substituting numeric UInt8 arguments for Bool and vice versa.

The mixed host bridge uses real C++ Bool fields and checked CPU export ABI
extensions. Bool main displays true/false. Cancellation before acknowledgement
still wins over an unrecorded outcome; an acknowledged fault stays primary over
later cancellation. This scalar boundary does not implement aggregate transport
or Bool tensors.

## Implemented backing protocol

`DeviceControl` is a private runtime instruction with a final invocation-owned
UInt64 arena binding. Source inputs remain separate and read-only. The shared
header defines capacity, growth-request and resume-PC cells; invalid capacity,
non-increasing growth and malformed status are rejected before another source
chunk can execute. Completed scalar results are published only at the terminal
PC. The source compiler emits this instruction for invocation-local arrays and
collecting loops; earlier plain-data programs retain ScalarControl.

The host admits a replacement allocation and copies opaque pages of at most
64 KiB. Both allocations remain charged and retained until the actual GPU copy
settles. Metal initializes new private storage and copies pages asynchronously
through ordinary external task completions. Vulkan/CUDA use their existing
completion services with fence-complete device copies. Each service step permits
cancellation before the next page or source chunk. No source predicate, element
computation or garbage-collection liveness decision runs on the host.

Physical private-protocol tests write 16,385 values on the GPU across repeated
arena growth, then read every value on the GPU for an independently checked
checksum. Tests cover concurrent invocations, consecutive arena instructions,
resource denial, cancellation/deadlines and cleanup; host protocol tests also
exercise interrupted copies and duplicate acknowledgements. These private tests qualify backing growth separately from source-language
coverage. Source tests compare array construction, mutation, iteration,
copy/slice/concat and ordered collecting loops with independent CPU oracles.

The initial typed allocator stores a four-word span/layout/logical-length/mark
header and one 64-bit cell per numeric or Bool element. It performs first-fit
allocation with splitting, then clears marks, marks compiler-proven live roots,
sweeps and coalesces before requesting growth. Each service step visits one
block or root. The compiler computes roots from the CFG, including allocation
operands and implicit collector handles; stale overwritten/dead slots are not
roots. An independent path oracle checks those liveness maps.

These initial payload layouts contain no owned references. They therefore do
not implement the grey-block traversal needed for owned fields, record/function
arrays or cycles. Those types remain rejected. Collector append grows private
backing geometrically, copies one initialized cell per step, then publishes the
new logical prefix. No source array pointer escapes the invocation; typed entry
transport remains unimplemented. The complete reference-tracing design above
remains required for full parity.

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
