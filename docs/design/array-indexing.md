# Checked immutable array indexing

Read an array element with a zero-based integer index:

```kalandra
fn get(values: [float32], index: int) -> float32:
  values[index]
```

This returns one scalar with the array's element type. The array remains unchanged. Array indexing binds as tightly as function calls: `make()[index] * 2` first calls `make`, then reads its element, then multiplies. Index expressions can perform ordinary checked integer arithmetic.

## Types and bounds

The input must be a completed CPU numeric array. All six implemented element types work: `int32`, `uint32`, `int64`, `uint64`, `float32`, and `float64`. The index must be a completed `int32`, `uint32`, `int64`, or `uint64` scalar; `int` and `uint` aliases work. Floating-point indices, Bool, task handles and unhandled fallible values are rejected. A scalar result annotation can provide the element type of a directly indexed array literal:

```kalandra
fn second() -> float32:
  [1, 2][1]
```

A valid index satisfies `0 <= index < length`. Negative indices do not count from the end. Indices do not wrap or truncate. Known invalid indices are compile-time errors, including indexing a known empty array. Unknown indices are checked at runtime, before the element read. An invalid runtime index produces a Bounds fault with the indexing expression's source location.

The array expression evaluates first, then the index expression, once each. Their existing faults retain their locations and take precedence over the bounds check. The read preserves the element's representation, including floating-point signed zeros and NaN payloads; it performs no numeric conversion.

## Calls, closures and workers

Array reads work in ordinary functions, inline map callbacks and worker helpers. Captured arrays keep their immutable ownership rules. After awaiting a worker-produced array, its elements can be read normally. A failed worker read uses the existing `TaskError.Fault` recovery path with `FaultKind.Bounds`. Cached successful scalar results retain the same value on repeated awaits.

Indexing itself does not dispatch work. Use `spawn get(values, index)` inside a parallel scope to run a helper on a worker. Direct `spawn values[index]` is not an invocation and is rejected.

## Lowering and safety

The typed IR has an explicit `ArrayIndex` with array and index operands. Continuation kernels and imported helper/callback graphs carry both operands. CPU MLIR lowering extends integer indices without losing signedness, validates the input's extent/byte range, and performs an unsigned comparison against the array length before converting to a storage index. This also rejects negative signed indices and the high half of `uint64`. The checked ABI exits before extraction on failure or cancellation.

The private runtime fault code is 5. CLI reporting names it `BoundsFault`; task recovery maps it to `FaultKind.Bounds` and the bounded static message `index out of bounds`. Allocation, ownership, task admission and numerical arithmetic mechanisms are unchanged. Generated CPU continuations also handle a failed group settling between spawns: an invalid dispatch handle follows the existing group join, preserving its recorded failure instead of trying to await that handle.

## Scope

This milestone covers numeric array reads. [CPU tensor scalar indexing](tensor-indexing.md) now uses one index per dimension. Slicing, general element types and immutable element replacement remain separate work. `values[index] = replacement` is never a mutation operation. Raw pointers, custom GPU indexing and unrestricted shared writes are not exposed.

## Qualification

On Apple M2 Max, all 91 focused checks passed across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. The indexing suites exercised 695,808 raw element/index cases, including 52,992 checked Bounds faults, and 1,440 worker harness runs. Coverage includes all 24 element/index combinations, exact floating-point representations in eight FP environments, strided borrowed inputs, invalid metadata, operand fault order, captures, nested workers, cached results, cancellation, failure recovery and allocation-failure cleanup. Repeated failed-dispatch tests cover the settled-group race discovered during qualification. Earlier collection, frontend, source/runtime and selected GPU regressions passed, and all 640 tracked numerical artifacts are unchanged. This is focused qualification, not a full-suite claim. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-array-indexing-2026-10-18.json) records scope and hashes.
