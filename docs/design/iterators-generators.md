# Iteration and independently owned generator frames

Iteration separates a reusable source from the state of a traversal. Creating a
cursor does not traverse the source. Advancing a cursor performs work until the
next value, exhaustion, or an expected error. There is no scheduled task or OS
thread associated with an ordinary cursor.

`std/option` supplies the nominal generic enum `Option[T]`: `None` represents
exhaustion and `Some {value}` represents a produced value. Distinct traversals
must have independent positions; a suspended generator instance is single-pass.
Exhaustion is sticky and must not re-execute producer side effects.

## Error type and uninhabited values

The lowercase primitive `never` has no possible values. `Iterator[T, never]`
cannot produce an expected error; `Iterator[T, E]` carries the typed error of
advancing its producer. `! never` is an infallible signature, not a sentinel
error variant or runtime wrapper. Error union normalization removes `never`;
generic callback inference must preserve that bottom-type identity while
omitting impossible runtime error paths. No value or constructor can inhabit
`never`.

## Source boundaries and collection

A loop expression containing `yield` defines a generator. Nested statement loops
and conditionals yield into that generator. A separately bound loop expression
establishes its own generator boundary. Loops without `yield` retain their
statement or single-result `break value` behavior.

`array(sequence)` explicitly consumes and materializes the produced values. The
migration from eager collecting loops changes when effects/errors happen and
when captured backing can be mutated; diagnostics and all authored examples must
migrate with that change. It cannot be implemented merely by delaying an eager
array allocation until the first `next`: partial consumption must perform only
the requested work.

## State and ownership

A generator frame owns its resume position, live locals, captures, cleanup
obligations, and terminal state. The creator's return must not invalidate it.
Advancing, closing, and transferring the frame require one consumer; aliases
cannot independently advance the same state or release a live capture.

This capability is separate from general record mutation. Source `mut` currently
permits flat numeric/bool arrays and `ByteBuffer`; iteration must not silently
make arbitrary record fields writable. Compiler-managed cursor/frame state has
an explicit ownership and access contract.

Borrowed array backing remains read-locked while the cursor can still read it.
Exhaustion, terminal failure, scope exit and early abandonment release captures
and owned resources deterministically. Cancellation/fault precedence and task
settlement follow the existing invocation contract. Suspending or abandoning an
active structured group requires an explicit settlement path.

## Lowering and performance requirements

Keep suspended generator frames separate from scheduler task admission. Charge
persistent state to bounded invocation-owned storage and release it at close.
Existing resumable call frames provide useful control-flow machinery but are
not an independently owned generator representation by themselves.

Specialize source, adapter and item types before lowering. Direct iteration of
arrays/ranges and statically known lazy pipelines should lower to ordinary loops
without allocating a frame per element or adding an indirect call per element.
Escaping stateful generators require explicit owned-frame lowering rather than
an accidental task/closure emulation with different cleanup behavior.

Device-compatible iteration needs a defined Metal/Vulkan/CUDA lowering or a
clear diagnostic. Live CPU cursors are not transferred to a GPU, and host I/O
never becomes a device instruction or hidden host fallback.

Qualification compares equivalent source-level work, validates values and side
effects, and retains control/paired/replication outcomes. Existing ordinary,
resumable, array and GPU programs remain part of regression acceptance.
