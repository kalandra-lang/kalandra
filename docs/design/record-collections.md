# Numeric collections inside records

Native CPU records can store immutable numeric arrays and tensors, including inside nested records. Array fields support all ten implemented numeric scalar types, including [8/16-bit integers](narrow-arrays.md). [Tensor fields](narrow-tensors.md) now also support all ten types; [checked narrow CPU reductions](narrow-reductions.md) are supported. [Checked narrow CPU matrix multiplication](narrow-matmul.md) is supported. [Checked CPU tensor numeric conversions](narrow-tensor-conversions.md) support all ten element types.

```kalandra
type Batch:
  values: [int]
  weights: Tensor[float32, ?, ?]

fn shifted(batch: Batch) -> Batch:
  batch with { values: map(batch.values, fn(value): value + 10) }

fn main() -> [int] ! TaskError:
  original = Batch { values: [1, 2, 3], weights: tensor([[float32(1), float32(2)]]) }
  result = try parallel:
    work = spawn shifted(original)
    first = try await work
    again = try await work
    again
  result.values
```

The result is `[11, 12, 13]`. The original array remains `[1, 2, 3]`. Record construction and updates use named fields; the initializer category and element type must match the declaration. An array does not implicitly become a tensor. Fixed tensor dimensions are checked, with runtime guards when the initializer's dimensions are dynamic. A dynamic declaration describes each instance independently: two `Batch` values need not have equal dimensions.

[Bool-array fields](record-bool-fields.md) are also supported, including nested records, immutable updates, captured records and worker transfer. They reuse the array backing rather than the numeric collection pack/unpack path.

Rank-zero tensors and empty collections work too. Numeric collections remain numeric: this does not add Boolean tensors, recursive managed records, or GPU records. [Arrays of these finite records](record-arrays.md) are now supported outside record fields. [Anonymous numeric callbacks can capture these records](record-closures.md).

## Ownership and execution

The first implementation favors explicit copies over storage-sharing optimization:

- Constructing or replacing a collection field copies its logical elements into owned row-major storage. An unchanged nested record can share its immutable field storage within the invocation.
- Reading a collection field copies it into an ordinary compiler-managed array or tensor. Its existing operations and function-return rules then apply.
- Passing records to CPU workers recursively retains every collection field before publishing the packet. Returning and observing records uses the same retention rule, including repeated awaits and shared backing allocations in multiple fields.
- Record-owned backing stays alive until invocation settlement. The scalar CLI entry releases these owners after computing its result. Collection CLI results use the existing retained-display boundary. Worker completion, errors and cancellation use the task runtime's settlement rules.

A helper can return a record without leaving dangling storage. An observed worker result survives release of its task group and scheduler while its observing invocation or retained allocation lease remains alive. Retention failure never publishes a partially copied record; invocation settlement releases any owners acquired before the failure.

This is a correctness baseline. Repeated construction inside a long-running invocation can retain temporary field storage until that invocation completes. Earlier last-use release, avoiding read copies, and shared immutable storage/views belong to the tracked optimization work; they are not guarantees of this implementation.

## Private representation

Each collection field stores an inline descriptor with allocated/aligned pointers, zero offset, dimensions and canonical row-major strides. Nonempty descriptors must reference a sufficiently large allocation owned by the source invocation. Empty descriptors use null pointers and zero strides and require no field payload allocation. Rank-zero tensors contain one element.

Runtime validation rejects negative dimensions, noncanonical descriptors, overflow, unknown allocations and invalid element types before dereferencing collection data. Fixed field dimensions are additionally checked by generated code. Six numeric types reuse the same descriptor, copy and retention machinery; transport does not convert elements or perform floating-point arithmetic.

The existing finite-record layout checks still reject inline cycles. Collection descriptors count their pointer/offset/dimension/stride words toward the expanded layout budget, rather than treating a high-rank field as a single scalar. Current compiler limits allow at most 128 tensor dimensions. These internal layouts and runtime symbols are not a supported C/C++ or network ABI.

## Qualification

The record collection suites cover six element types, nested helper and worker returns, immutable updates, repeated awaits, static/dynamic shapes, empty and rank-zero fields, TaskError recovery, allocation failure, and exact raw-bit transport. Dedicated runtime tests exercise malformed descriptors, duplicate owners, leases after settlement, and failed retention cleanup. Execution is checked in debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. All 130 focused checks passed on Apple M2 Max, including 144 compiled source programs and 400 worker harness runs across the four variants. The harnesses checked 1,536 raw-bit success samples and 3,072 failed-observation publication guards, plus cancellation, deadlines, admission rejection and allocation failure. All 640 tracked numerical artifacts remain unchanged. The checked invocation ABI and runtime have intentional additions for record storage; the other 17 tracked numeric/storage sources are unchanged. This is focused qualification, not a full-suite claim. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-record-collections-2026-10-18.json) records scope and hashes.
