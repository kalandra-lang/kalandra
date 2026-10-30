# Native collection results

The development CPU compiler now accepts numeric arrays and tensors as the result of `main`, alongside existing numeric scalars and Bool. An executable displays its completed result directly:

```kalandra
fn main():
  [1, 2, 3] -> map(fn(value): value * 2)
```

Output:

```text
[2, 4, 6]
```

This is a development executable convention, not a general printing effect or a public native library ABI.

## Values and shapes

All six implemented CPU element types work: `int32`, `uint32`, `int64`, `uint64`, `float32` and `float64`, including `int`, `uint` and `float` aliases. Ordinary literal inference, explicit conversions and type checks remain unchanged. Scalar and Bool result formatting is unchanged.

Arrays display as ordered lists. Tensors display flat logical row-major elements together with their complete shape:

```text
tensor([1, 2, 3, 4], [2, 2])
tensor([7], [])
tensor([], [2, 0, 3])
```

The second result is a rank-zero tensor with one element. The last result preserves an empty rank-three shape, which would be lost if displayed simply as `[]`. Rank-one tensors remain distinguishable from ordinary arrays. The CPU display entry handles ranks through the frontend's current 128-dimension bound; this does not expand the scheduler's existing tensor-rank capabilities.

Signed and unsigned integers use exact decimal values, including 64-bit endpoints. Float32 uses nine significant decimal digits and float64 uses seventeen, matching existing scalar result precision. Signed zero and nonfinite values are displayed. Display is not serialization: NaN payloads and source type annotations are not encoded in the text.

## Large results

The default display shows at most 100 elements, with an explicit ellipsis and full element count when truncated. Tensor shapes are always shown in full. This is a display bound, not a collection-size limit.

Options belong to the compiled collection-returning executable:

```sh
./program --limit 10
./program --limit 0
./program --all
```

`--all` displays every element. `--limit` accepts a nonnegative integer count. Invalid or conflicting options fail with usage information before the source program executes. These options do not change computation or introduce new compiler flags.

## Worker results and failures

A CPU worker-produced collection can be returned from `main` after its parallel scope finishes:

```kalandra
fn double(values: Tensor[int, ?, ?]):
  values * 2

fn main() ! TaskError:
  try parallel:
    work = spawn double(tensor([[1, 2], [3, 4]]))
    try await work
```

Output:

```text
tensor([2, 4, 6, 8], [2, 2])
```

Imported helpers, arrays, nested work and repeated awaits keep their existing typed ownership rules. The display boundary validates the full descriptor and payload range before reading elements, even when only a short prefix will be shown. It retains managed backing, settles the execution context, then formats the result without making another payload copy. The retained reference is released when display finishes.

A checked allocation, arithmetic, shape, bounds or task failure prints the existing failure diagnostic to stderr, exits nonzero and prints no collection result to stdout. Collection display does not read borrowed or malformed backing as though it were an owned successful result. Output-stream errors can still interrupt text output, as with ordinary command-line programs.

This milestone covers completed CPU collections. It does not add GPU/mixed-target collection entry points, implicit downloads, record/string display, program input arguments or supported object/shared-library output.

## Implementation

Synchronous MLIR entry wrappers return owned ranked descriptors using the function's checked declared result signature. Task entry wrappers copy successful owned descriptors into the private executable output parameter and zero that parameter before execution, leaving no partial descriptor on failure. Compiler-emitted private metadata identifies element type, rank and array category. One checked display entry handles all supported CPU element types and ranks; per-type/rank numerical engines are not duplicated. The existing checked invocation ABI, scheduler, tensor storage and arithmetic implementations are unchanged.

## Qualification

On Apple M2 Max, all 95 focused checks passed across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. The new suites compiled 264 source programs and ran 488 display validation/format/lifetime cases plus 480 guarded root-entry ABI/ownership cases. Coverage includes all six numeric types, exact integer endpoints and floating-point edge formatting, dynamic shapes, rank-zero and empty tensors, high CPU ranks through 128, imported workers, cached awaits, recovered errors and 1/2/4-worker execution. Allocation/admission failures clear the active result descriptor, preserve surrounding storage and publish no partial stdout; formatting retains data after root/invocation cleanup without another payload allocation. Display limits, option validation before execution, complete payload bounds and Shape/Arithmetic/Bounds/task failure cleanup also passed. Existing frontend, scalar/task/storage/continuation, module and representative collection runtime tests and selected Metal regressions passed. All 640 tracked numerical artifacts and 20 numerical/checked-ABI/runtime/storage sources remain unchanged. This is focused qualification, not a full-suite claim; the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-collection-results-2026-10-18.json) records scope and hashes.
