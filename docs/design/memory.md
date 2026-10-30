# Kalandra automatic memory management

Accepted programming direction — 2026-10-14. Allocation scopes and their keyword have been removed from the language proposal. The compiler chooses allocation strategies while the language guarantees valid immutable values. These rules belong to the fresh design, not a change to the released Kalandra 0.13 implementation.

## Programming model

Ordinary code can bind, pass, return, share, and capture immutable values without manual allocation/freeing, allocator parameters, ownership moves, or lifetime annotations. Values remain valid while accessible by the program or unfinished operations.

```kalandra
fn shifted(values: [int], offset: int) -> [int]:
  values -> map(fn(value): value + offset)

input = [1, 2, 3]
result = shifted(input, 10)
```

Both arrays remain usable. The returned array survives the function's return. A closure or view also keeps its backing data valid. These guarantees do not depend on whether the implementation copies, shares, or allocates inline.

Temporary computation uses ordinary code:

```kalandra
fn variance(samples: [float]) -> float:
  average = mean(samples)
  deviations = samples -> map(fn(value): value - average)
  squares = deviations -> map(fn(value): value * value)
  mean(squares)
```

Assume nonempty input and a scalar-returning `mean` function. Intermediates are no longer needed after the calculation. There is no additional lifetime fact for the author to express. The compiler can fuse operations and remove the arrays, release storage after its final use, or group proven temporary allocations internally.

## Compiler and runtime responsibilities

- Track backing-storage dependencies through records, arrays, views, returns, and captures, including across module boundaries.
- Use stack/inline allocation where lifetimes permit it.
- Share immutable data where advantageous; do not require a deep copy merely to pass a value.
- Reuse a buffer only when no live reader, alias, capture, task, or foreign operation can observe its old contents.
- Use arenas or pools internally when analysis establishes safe reclamation and measurements justify the strategy.
- Fall back to managed storage when an escape or lifetime cannot be bounded statically. Returning an array is ordinary valid code, not a reason to reject it.
- Release outgoing managed dependencies and partially constructed values correctly, even when underlying storage is reclaimed in bulk.

Allocation strategy is not a language-visible distinction. Compiler-chosen arenas cannot impose escape restrictions on otherwise valid code, prolong invalid storage, or expose alias mutation. Conservative analysis may cost performance but must preserve correctness.

The default mechanism remains an implementation decision. Reference counting with optimization, tracing, region analysis, or a combination must be evaluated against supported closure/cycle behavior, cross-thread sharing, cleanup overhead, peak memory, and latency. Immutability alone does not rule out reference cycles. No particular mechanism is accepted merely because the language uses automatic cleanup.

Deep destruction must not exhaust the call stack. Safe-language guarantees hold in optimized builds, not only under debug instrumentation. Observable external resources need their own explicit operations; the effects/resource proposal must distinguish those from ordinary memory reclamation.

## Unfinished work

CPU tasks and GPU commands retain required arguments, captures, buffers, and backend resources until confirmed completion or termination. Dropping a task handle or cancelling the caller's request is not proof that execution stopped.

GPU buffer reuse additionally requires producer completion, final-reader completion, and required synchronization. A CPU function returning does not establish GPU completion. Host-device placement and transfers belong to the tensor/execution proposal, not allocator-scope syntax.

Remote execution establishes storage lifetimes on each machine. Calls transfer validated values or explicitly specified remote-resource references, never local pointers. Sender data remains valid through any encoding and transport operation that accesses it. Local reference counts cannot serve as distributed ownership accounting.

## Native boundaries and safety

Bounds checks remain unless proven unnecessary, including device indexing. Allocation sizes and offsets require checked arithmetic. Storage is initialized before publication as an ordinary value, and failed construction cleans up exactly once.

Foreign wrappers specify whether a pointer is borrowed or retained, how callbacks extend use, and what completion ends that use. Raw pointers and unchecked native access belong to a separately designed unsafe boundary. Automatic lifetime management cannot make an incorrectly implemented foreign library safe.

Optional expert memory controls can be considered later only for a demonstrated requirement, such as a fixed memory budget or a specific external/device resource. They are not part of the core, and no allocation keyword is reserved by this design.

## Validation before implementation commitment

1. Returned arrays, escaping closures, and views stay valid after their creating function returns.
2. Two readers can share immutable input without invalidation or observable mutation.
3. Intermediates are reclaimed safely after their final uses or eliminated by fusion.
4. Separately compiled helpers preserve backing-storage lifetime information.
5. Partial failure and deep cleanup release dependencies safely and exactly once.
6. Supported recursive/cyclic constructions have a tested reclamation strategy.
7. CPU/device cancellation and foreign callbacks do not allow premature reclamation.
8. Allocation strategy changes preserve the same observable program behavior.

Measure full execution time, allocation count, peak retained bytes, cleanup latency, and scheduling overhead against equivalent native baselines. Internal arena allocation may retain temporary bytes longer than individual cleanup, so it must not be assumed to reduce peak memory.

## Next design step

The [numeric rules](numbers.md) and [errors/effects](errors.md) are accepted. [Resources and task lifetimes](execution.md) are accepted. [Variants and error payloads](variants.md) and [tensor types and shapes](tensors.md) are accepted. Review [placement and transfers](placement.md) next. The implementation follows those semantic decisions.
