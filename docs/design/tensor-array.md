# CPU tensor-to-array construction

`array(values)` constructs an immutable numeric array from a completed CPU tensor. It preserves the element type and flattens the tensor in logical row-major order:

```kalandra
fn main():
  matrix = tensor([[1, 2], [3, 4]])
  values = array(matrix)
  values -> map(fn(value): value * 2) -> tensor() -> reduceSum()
```

This returns `20`. `values` contains `[1, 2, 3, 4]`; `matrix` remains available as a tensor. The pipeline form `matrix -> array()` is equivalent to `array(matrix)`.

## Types and shapes

The constructor accepts all six implemented CPU numeric types: `int32`, `uint32`, `int64`, `uint64`, `float32` and `float64`, including the `int`, `uint` and `float` aliases. It returns `[T]` for a `Tensor[T, ...]`. An expected array type does not convert an already typed tensor's elements. Use an explicit numeric conversion when changing element type.

A rank-zero tensor produces a one-element array. A tensor with any zero dimension produces an empty array. The flattened length is the product of the source dimensions; known lengths and rank-one dynamic extent identities remain available to shape checking. Runtime dimensions and byte products are checked before allocating or reading elements.

Arrays and tensors remain separate source categories. There are no implicit conversions, and `array` does not accept an existing array, scalar, task handle, unhandled fallible result or resident GPU tensor. Transfer a resident tensor to CPU explicitly first. This is a CPU operation; mixed-target integration remains separate work.

## Ordering and ownership

The tensor argument evaluates once, before construction. Its failures retain their original locations. Construction reads logical coordinates in row-major order, respecting the input descriptor's strides and offset. Non-contiguous, transposed-layout and negative-stride borrowed inputs follow the same ordering. Every element preserves its exact bits, including floating-point signed zeros, subnormals and NaN payloads.

The initial implementation makes an ordered copy into managed array storage. Borrowed input storage cannot escape as an owned array result. Results remain valid after the source function or worker finishes. Allocation failure or cancellation publishes no partial result and settles intermediate owners. Shared-storage construction is later optimization work under the same immutable semantics.

## Helpers, callbacks and workers

Construction works in ordinary helper graphs, synchronous map callbacks with captured tensors, and direct CPU tasks:

```kalandra
fn main() ! TaskError:
  values = try parallel:
    work = spawn array(tensor([[1, 2], [3, 4]]))
    first = try await work
    again = try await work
    again
  values[3]
```

This returns `4`. Repeated awaits reuse the completed immutable result. Worker captures and results retain their array/tensor categories and concrete numeric identities. Nested helpers and TaskError recovery use the existing scheduler and ownership rules. Ordinary argument evaluation precedes dispatch; use a helper to construct the source tensor inside a worker.

## Implementation

Typed IR represents construction as `TensorToArray(input)`. The CPU backend reuses checked shape/count validation and logical copying from tensor reshaping, with a rank-one array result whose length is the source element count. Continuation kernels and imported helper/callback graphs remap the source operand. Capability validation rejects invalid operand IDs, changed element identities, invalid categories and non-CPU values before lowering. Numerical engines, the checked invocation ABI and ownership runtime are unchanged.

## Qualification

On Apple M2 Max, all 91 focused checks passed across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. The new constructor suites ran 15,176 raw copy/metadata/ownership cases with 1,590,912 exact lane checks and 1,200 worker harness runs. Coverage includes all six numeric types, input ranks zero/one/two/three/eight, empty arrays, gapped row/column-major and negative-stride input, exact floating-point bits in eight environments, source independence, allocation-failure sweeps, retained results and active million-cell cancellation. Helpers, captured-tensor callbacks, direct/nested workers, cached awaits, post-group results, map/tensor/reduction pipelines, fault locations and diagnostic artifact preservation also passed. Earlier shape/reshape, frontend, source/task/storage and selected Metal regressions passed. All 640 tracked numerical artifacts and 20 numerical/checked-ABI/runtime/storage sources remain unchanged. This is focused qualification, not a full-suite claim; the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-tensor-array-2026-10-18.json) records scope and hashes.
