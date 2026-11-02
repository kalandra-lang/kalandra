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
3. Give mutable arrays a reference representation. Current numeric arrays use
   immutable MLIR tensors; simply inserting writes would violate optimization and
   aliasing assumptions. Verify identity returns, fresh transforms, allocation
   ownership and all CPU/task ABI boundaries before exposing mutation.
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

## Compatibility

Existing functional programs keep their observable results. Repeated local `=`
becomes reassignment instead of a duplicate-binding error in admitted contexts.
Once arrays become mutable, aliases are observable and optimizations may reuse
storage only when they preserve both value results and storage identity. Record
field mutation, nested mutable containers, resizing, early loop exits, distributed
retry effects and GPU kernel memory semantics require explicit decisions; this
proposal does not silently admit them.
