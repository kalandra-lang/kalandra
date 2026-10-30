# Native collection size queries

The native CPU subset provides `length` for numeric arrays and `shape` for numeric tensors. They are ordinary builtin calls, including pipeline forms; no new punctuation or type decorators are involved.

```kalandra
fn describe(values: Tensor[float32, ?, ?]) -> [int]:
  shape(values)

fn main():
  values = tensor([[1, 2, 3], [4, 5, 6]])
  dimensions = shape(values)
  dimensions[0] * 10 + dimensions[1]
```

`dimensions` is `[2, 3]` and the program returns `23`. These are runtime extents, so the same helper accepts a tensor whose rows and columns are known only when the program runs.

## Results and categories

| Call             | Input                                  | Result                                                  |
| ---------------- | -------------------------------------- | ------------------------------------------------------- |
| `length(values)` | Completed CPU numeric array `[T]`      | Element count as `int` (`int64`)                        |
| `shape(values)`  | Completed CPU numeric `Tensor[T, ...]` | Immutable `[int]` containing extents in dimension order |

Both accept all six implemented numeric element types. The result does not inherit the input's precision or signedness. Metadata uses the existing nonnegative signed 64-bit extent range. It is not a floating-point approximation and does not introduce a universal Number type.

`length` counts array elements, including zero for an empty array. `shape` returns one extent per tensor dimension. A rank-zero tensor contains one scalar but has no dimensions, so its shape is `[]`; `length(shape(value))` is zero. An empty matrix shaped `[2, 0]` still returns `[2, 0]`. Array length is runtime data rather than part of the source `[T]` type; a shape result's array length is nevertheless known internally from the tensor rank.

Array and tensor categories remain explicit. `length(tensor)` and `shape(array)` are rejected. Use `shape(tensor)[axis]` for an individual extent, with ordinary checked array indexing. `length(shape(tensor))` obtains its rank. Already typed metadata does not implicitly convert to `uint` or another numeric type.

Empty arrays still need element context:

```kalandra
fn main():
  values: [float32] = []
  values -> length()
```

This returns zero. A bare `length([])` cannot establish the array's element type and remains a diagnostic. Calls require exactly one argument. Normal function name resolution applies: a declared function with either name is resolved before the builtin, and an ordinary local value with that name is not callable.

## Evaluation and memory

The input expression evaluates once before querying its dimensions. Querying a literal or helper result does not suppress its element expressions, arithmetic faults or expected-error handling. A tensor's extent/byte metadata is validated through the existing checked path before publishing a query result. Invalid extents produce Shape faults; an unrepresentable nonempty byte count produces an Allocation fault.

Neither query scans, converts or copies the input elements. `length` itself allocates no storage. `shape` constructs a small managed array of int64 dimensions, independent of the input's backing. That array remains valid after the source tensor's function or worker finishes. Rank-zero shape arrays follow the existing empty-array ownership rules. Allocation failure and cancellation publish no successful or partial shape array and settle intermediate owners normally. Allocation faults retain the runtime's existing reporting behavior; this does not add a new allocation ABI.

An input expression or worker capture may have its own allocation/ownership costs. Querying dimensions does not waive those existing value-passing rules.

## Functions, callbacks and workers

Both queries work in ordinary helper graphs and numeric map callbacks with lexical collection captures. They can also be dispatched directly as CPU tasks:

```kalandra
fn main() ! TaskError:
  values = tensor([[1, 2, 3], [4, 5, 6]])
  dimensions = try parallel:
    work = spawn shape(values)
    first = try await work
    again = try await work
    again
  length(dimensions)
```

This returns `2`. Repeated awaits reuse the completed shape result; it remains available after the parallel group closes. `spawn length(array)` returns a typed int64 scalar. As usual, resolve pending task handles and handle expected errors before using their values as query inputs; `shape(try await work)` is valid for a tensor task, while `shape(work)` is not.

## Lowering and placement

Typed IR uses `CollectionQuery` with explicit Length/Shape kind and input. Semantic checks enforce the source category and exact result identity. Capability checks reject malformed references, unsupported kinds, rank/result metadata changes and non-CPU inputs before backend lowering.

CPU MLIR lowering reads dimensions, checks the source extent/byte range, and either returns the array extent or writes the ordered dimensions into a new int64 array. Continuation kernels and imported callback/helper graphs remap the input explicitly. Both operations use the existing checked invocation, allocation, retention and task machinery.

This implementation handles CPU collections. Resident GPU inputs are rejected; there is no implicit download. Device-side queries and host metadata access without transferring a resident value need their own placement integration. The tensor constructor still requires its shape list at the call site: use `tensor(data, [dimensions[0], dimensions[1]])` rather than passing a stored shape array directly. It does not add general String/record queries, runtime-variable tensor rank, slicing or device-side reshaping. [CPU tensor-to-tensor reshape](tensor-reshape.md) is now available separately. Native distributed execution remains behind its agreed design discussion.

## Qualification

On Apple M2 Max, all 79 focused checks passed across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. The query suites exercised 11,328 raw metadata/allocation cases, including 4,928 checked faults, and 1,272 worker harness runs. Coverage includes all six numeric input types, ranks zero/one/two/three/eight, empty and large dimensions, exact metadata in eight FP environments, null element payloads proving metadata-only access, malformed descriptor guards, shape allocation failures, cancellation and retained arrays after settlement. Direct/nested worker calls, captures, cached awaits, expected-error handling, source diagnostics and earlier compiler/runtime/selected GPU regressions also passed. All 640 tracked numerical artifacts and 20 numerical/checked-ABI/runtime/storage sources are unchanged. This is focused qualification, not a full-suite claim. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-collection-queries-2026-10-18.json) records scope and hashes.
