# Local mutation and read-only task sharing

Accepted direction, 2026-11-02. Implementation tracked in #266. This document
specifies the intended language; it does not claim the released compiler already
accepts these examples. It supersedes the universal-immutability requirement in
the original core proposal for the local bindings and arrays admitted below.

## Why change the model?

A private counter or array builder does not require universal immutability to be
safe. Automatic lifetime management prevents dangling storage; it does not prevent
concurrent reads and writes. We will permit local mutation and reject shared
mutation at compile time, while retaining functional construction and transforms
as first-class APIs. There are no implicit locks, ownership moves, reference
sigils, or source lifetime annotations in this direction.

## Observable behavior

- `a = 10; a = double(a)` rebinds a local name. A callee cannot change its caller's
  scalar binding. Capturing a scalar takes its value at capture time.
- `b = a` aliases array storage. An in-place edit through either name is visible
  through the other. `copy(a)` creates independent storage. Nested mutable storage
  requires an explicit copy-depth policy before it is supported.
- `sorted(a)` produces a fresh array. `sort(a)` changes the existing array.
  `generate(count, fn(index): expression)` constructs an array functionally, in
  ascending index order, without requiring an intermediate array of indices.
- Ordinary parameters and captures grant read access. Writing caller-owned
  storage requires an explicit mutating parameter contract, including in function
  types and indirect calls. Declaration syntax is still provisional.
- A task can mutate storage it creates privately. Parent storage passed to or
  captured by a task is read-only. `spawn sort(a)` must be rejected.
- While any unfinished task may read storage, the parent and every alias must be
  unable to mutate that storage. Multiple readers are allowed. Cancellation is
  only a request; it does not release access before execution actually settles.
- A proven settlement boundary releases that reader's access. Awaiting one of
  several readers does not release the others. Awaiting only one conditional
  branch cannot prove unconditional settlement after the join.
- Tensors retain value semantics. No new GPU shared-memory, barrier or atomic
  model is implied. User-written kernels remain a separate dependent proposal.

Illustrative pseudocode, pending implementation:

```text
a = [3, 1, 2]
b = a
task = spawn readAndDoSomething(a)
sort(b)                       // reject: same backing still has a task reader
await task
sort(a)                       // allowed after proven settlement

snapshot = copy(a)
task = spawn readAndDoSomething(snapshot)
sort(a)                       // allowed: independent storage
await task
```

Aliasing includes captures, returned values, record fields, views and transfers.
Transfers must not be assumed to copy on unified-memory hardware. When a proof is
unavailable, reject the write and identify the outstanding read and a remedy.
Rebinding a name does not grant write access to read-only storage.

## Implementation sequence and gates

1. Restore functional construction APIs independently of mutation. `generate` is
   implemented in this branch: runtime/constant sizes, empty arrays, ten numeric
   types, owned elements, callbacks/captures, worker import, allocation failure and
   cancellation are covered. Editor completion, hover and signature help use its
   checked types. `copy` is also implemented for numeric, Bool and String arrays;
   raw-bit tests check separate backing, strided inputs, independence after source
   changes, retained results and allocation-failure cleanup. Existing runtime
   quotas apply; no unchecked allocator is added.
2. Numeric/Bool rebinding, statement `if` and `while` are implemented in this
   branch. Scalar types remain fixed. A loop has no source value and cannot be a
   function's final result. Branch/iteration declarations stay local, while writes
   to existing scalar bindings take effect in their enclosing invocation.
   Functions and closures receive scalar values; captures are snapshots. Rebinding
   a captured scalar affects only the current call, not the shared closure
   environment. Native entry-block cells promote to registers/SSA in optimized
   builds; suspended execution uses private continuation slots and explicit read
   snapshots. No scalar cell escapes as a source reference. Every successful
   backedge settles iteration-owned task obligations; zero-iteration paths retain
   incoming obligations. Completed structured groups release scheduler records
   before another iteration. Native checked loops and continuation backedges keep
   cancellation/fault checks; ordinary CLI proofs may omit unnecessary checks as
   before. `break`, `continue`, `for` and general Unit-returning APIs remain outside
   this stage.
3. Numeric arrays now use a reference representation in this branch, undergoing
   qualification. The earlier immutable MLIR tensor representation could clone
   returned arrays or reuse functional results; inserting writes into it would
   violate the intended aliases. Verify identity returns, fresh transforms,
   allocation ownership and every CPU/task ABI boundary before exposing mutation.
4. Implement explicit mutating contracts and path-sensitive storage/read-access
   analysis. Branches conservatively retain possible outstanding readers; loops
   account for zero iterations and require iteration-owned tasks to settle.
5. Add bounds-checked writes and sorting, with documented supported element
   types, copy depth, float ordering and cancellation behavior. In-place mutation
   need not be transactional, but must remain memory-safe on every exit.
6. Update user documentation, formatter, editor, examples and migration notes;
   run numerical, ownership, cancellation and backend regression suites. Measure
   equivalent algorithms rather than substituting native library operations.

Each stage must preserve fault precedence and resource limits. No pass may hide
live array references from reclamation analysis. Read-only inputs must never gain
write permission merely by passing through an unknown function. Conditional
settlement must not use the existing union-of-observed-tasks bookkeeping as an
exclusive-access proof.

## Array ownership boundaries

Numeric arrays lower to rank-one memrefs with explicit loads and stores. Source
arrays retain their identity through synchronous calls, records, closures and
settled task results. Private array-returning implementations carry ownership
information; their C and task adapters keep the existing collection descriptor
layout. The ownership pass may return a borrowed reference without cloning its
data. Layouts remain predictable at these boundaries. Programs that lower no
numeric array references retain the existing tensor-only lowering pipeline.

`generate`, `map`, `copy`, `slice` and `concat` allocate fresh outer storage.
Array-to-tensor and tensor-to-array conversions also copy: editing an array must
not change an immutable tensor or a buffer already used by a GPU operation.
Empty arrays require no backing and use canonical empty descriptors at opaque
boundaries. No pointer-identity operation is exposed by the language.

Before a reference enters an opaque record, closure or ABI descriptor, generated
code retains an invocation owner while the memref remains visible to reclamation
analysis. Ordinary deallocation retires its ordinary owner first, preserving the
explicitly marked escaped owner. These invocation-local markers do not modify
shared backing metadata. Escaped owners last until invocation settlement or the
scalar root's aggregate cleanup boundary, where no source reference can remain.
Cleanup removes escaped owners while preserving ordinary-allocation leak checks;
existing admission charges remain conservative. This is a safe lifetime bound,
not a claim of minimal retention for every captured alias. Fault precedence,
cancellation checks and scheduler resource limits remain in force.

The private host/test adapter preserves managed input identity. When unmanaged
host storage could escape, it imports a managed snapshot; such foreign snapshots
are per argument and do not promise preservation of aliases between foreign
arguments. Read-only queries and proven fresh transforms borrow their inputs
without this copy. Argument extents are validated before any import allocation
or payload read. This adapter is not a stable public foreign-function interface.
The new private runtime helpers require matching development runtime bundles;
this branch reports `0.99.3-dev`, separately from published `0.99.2`.

Array binding reassignment and element mutation remain gated on the next access
analysis. Reassigning array bindings must keep ownership visible through branch
results and loop-carried state. Hiding descriptors in scalar cells and retaining
on every read would accumulate owners instead of implementing proper liveness.

Flat numeric and Bool storage has no owned child references. String, record and
function elements do: overwriting them after an alias crossed an invocation
boundary requires the backing to retain newly installed children. Context-local
dependency lists alone are insufficient. Such writes must remain rejected until
their dependency updates are implemented and qualified; immutable transforms
remain available. No nested copy-depth policy is implied by this migration.

## Access analysis for the next stage

The planned spelling is `fn edit(mut values:[int])->int`, with the corresponding
function type `fn(mut [int])->int`. The marker describes access to the array's
backing, not assignment to the parameter's local name. Ordinary parameters and
all captures remain read-only access paths. A mutating contract must survive
function values, record fields, arrays of functions and indirect calls; erasing
the marker must be a type error rather than an implicit permission conversion.
The first mutating contracts will admit flat numeric and Bool arrays only.

Storage permission and storage origin are separate facts. A local fresh array
provides writable backing, but rebinding a read-only parameter does not make its
old backing writable. Aliases of local backing keep the same origin. Direct
function summaries describe which parameters a returned reference can alias and
which results are fresh. Capturing an array creates a read-only access path even
when its original local binding remains writable. Unknown indirect results must
not manufacture writable storage; a fresh `copy` can provide independent storage
when the compiler cannot establish a safe origin.

Reader tracking must follow statement evaluation order, not merely all tasks
observed anywhere in the function. Each unfinished reader keeps its possible
storage origins protected. A write is rejected when its possible origins overlap
a protected origin. Conditional normal exits join the outstanding-reader sets;
a zero-iteration loop preserves incoming readers. An actual await settlement can
remove that task's reader, while a cancellation request cannot. Structured group
settlement removes its own readers only. Summaries and control-flow joins must be
bounded, and uncertainty must yield a diagnostic rather than a weakened rule.

These rules require negative tests through ordinary aliases, captures, returned
arrays, aggregate fields, indirect calls, multiple readers, conditional awaits,
zero-iteration loops and cancellation. Positive tests must include independent
copies, private task allocations and writes after all relevant readers settle.
This section specifies the access proof to implement; the representation migration
above does not enable writes by itself.

## Compatibility

Existing functional programs keep their observable results. Repeated local `=`
becomes reassignment instead of a duplicate-binding error in admitted contexts.
Once arrays become mutable, aliases are observable and optimizations may reuse
storage only when they preserve both value results and storage identity. Record
field mutation, nested mutable containers, resizing, early loop exits, distributed
retry effects and GPU kernel memory semantics require explicit decisions; this
proposal does not silently admit them.
