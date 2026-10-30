# Bool-array fields in immutable CPU records

A record field can have type `[Bool]`, including inside a nested record. It follows the same type, indexing and sequential mapping rules as an ordinary [Bool array](bool-arrays.md).

```kalandra
type Selection:
  flags: [Bool]

fn invert(selection: Selection) -> Selection:
  selection with {flags: map(selection.flags, fn(flag): not flag)}

fn main() -> [Bool] ! TaskError:
  original = Selection {flags: [true, false, true]}
  changed = try parallel:
    try await spawn invert(original)
  changed.flags
```

This returns `[false, true, false]`; `original.flags` is unchanged. An empty field receives its type from the declaration: `Selection {flags: []}`.

## Composition

Bool-array fields work with nested records, immutable updates, synchronous helpers, conditional/recovery results and local modules. Records containing these fields can be elements of record arrays and parameters/results of named or anonymous map callbacks. A callback can capture a whole record and read or map its Bool-array fields, including through a nested callback. Numeric arrays/tensors and scalar fields can coexist in the same record.

Each field read has fresh dynamic length metadata, independent of a particular constructor call. Bool fields are not implicitly numeric, and a `[Bool]` field cannot receive `[int]`, a scalar Bool, a tensor, or a different collection category. Bool tensors, record-array fields, recursive managed records and GPU records remain separate work.

## Storage and lifetime

A Bool-array field stores an immutable descriptor. Construction and reads reuse its existing backing; they do not copy the bytes. Two fields can refer to the same array. This storage choice exposes no mutable reference or pointer identity in the source language.

Record construction, field access and updates use the existing invocation-owned Bool-array storage. Recursive worker retention validates the descriptor and every byte, retains its backing, and polls cancellation with the existing Bool-array rules. This also applies to records stored in record arrays or captured by callbacks. Nested fields remain alive after source settlement, through cached awaits and after a worker group ends.

The destination record is published only after every field has been retained successfully. If a later field is malformed or allocation fails, the destination stays untouched and partially retained owners settle normally. No runtime allocation, ownership or scheduler API is added by this extension. Numeric collection fields keep their existing storage behavior. Native layout and task identities remain private compiler/runtime interfaces.

## Qualification

On Apple M2 Max, all 39 selected checks passed, including debug, optimized, ASan/UBSan and separate ThreadSanitizer execution for this extension. Its four generated suites compiled 72 source programs and ran 256 worker harness invocations. Their 160 success samples contained 174,200 unique Bool backing cells shared between fields, with exact numeric sibling bits, lengths 0/1/257/4097 and one/four workers. Tests cover nested records, immutable updates, record arrays, captured records, six mixed numeric types, imported helpers, empty fields, recovery and bounds faults, cancellation/deadlines, admission, partial retention, cached results, source/scheduler settlement and rejected-artifact preservation. CLI allocation sweeps ran 480 cases; the documentation example runs in every variant.

Two legacy frontend rejection assertions were updated for the supported field type and passed on rerun; no compiler/runtime changes followed the initial regression run. All 640 tracked numerical artifacts and 20 protected numerical/runtime/storage/checked-ABI source files remain unchanged. This is focused qualification, not the full registered suite or a new GPU execution claim. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-record-bool-fields-2026-10-18.json) records the scope, rerun and hashes.
