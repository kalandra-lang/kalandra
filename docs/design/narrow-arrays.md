# Narrow CPU arrays and record fields

`int8`, `uint8`, `int16` and `uint16` can be elements of immutable CPU arrays and scalar or array fields in records. This extends existing collection and record operations to the four narrow integer types; it adds no new syntax or implicit numeric conversions.

```kalandra
type Batch:
  tag: uint16
  values: [int8]

fn shift(batch: Batch) -> Batch:
  batch with {values: map(batch.values, fn(value): value + 2)}

fn main() -> [int8] ! TaskError:
  original = Batch {tag: 65535, values: [1, 2]}
  changed = try parallel:
    try await spawn shift(original)
  changed.values
```

The result is `[3, 4]`. `original` remains unchanged. Each `int8`/`uint8` element occupies one byte; each `int16`/`uint16` element occupies two bytes. Record layouts retain each field's exact type and target alignment, including padding between mixed-width fields. Record identities remain nominal.

## Existing operations

Construction, typed empty arrays, indexing, length, conditional results, sequential maps, slices and concatenation preserve the element type and existing checks. Named/anonymous callbacks may accept or return narrow values and capture them. Use explicit numeric constructors in callbacks to convert element types, such as `map(values, fn(value): int(value))`; annotations do not convert an already typed array.

Arithmetic inside callbacks follows the checked scalar rules. Overflow and zero divisors produce Arithmetic faults, including in workers. Failed computations do not publish partially built arrays. Arrays do not acquire tensor arithmetic: use explicit collection operations rather than `values + values`.

## Records and workers

Narrow fields compose with nested records, Bool, String, existing numeric fields, owned arrays and record arrays. Field access and immutable updates preserve the exact width. Record-valued callbacks and lexical captures use the same ownership rules.

Local workers retain array storage and owned children through nested tasks, repeated awaits, error recovery and source settlement. Each observed result owns its required storage independently of the source invocation and scheduler lifetime. Copies remain the implementation baseline; shared-storage optimization is tracked separately.

## Display and limits

CPU executables display narrow numeric arrays as decimal numbers, including `-128`, `255`, `-32768` and `65535`. Existing `--limit` and `--all` options apply. Byte values are printed as numbers, not characters.

[Narrow CPU tensors](narrow-tensors.md) now support construction, checked elementwise arithmetic, structural adapters, record fields and retained workers. [Checked narrow CPU reductions](narrow-reductions.md) are supported. [Checked narrow CPU matrix multiplication](narrow-matmul.md) is supported. [Checked CPU tensor numeric conversions](narrow-tensor-conversions.md) support all ten element types. GPU execution remains separately gated. Internal descriptor storage support is not a promise of source-level tensor operation support. Distributed execution and public C/C++ interfaces remain separate roadmaps.

## Qualification

The focused suite checks all four narrow types through borrowed strided array slices/concatenation, exact raw bits, mixed-width record layouts, nested/cached workers, source and scheduler settlement, cancellation, deadlines, admission and allocator failures. Executable examples cover empty arrays, maps, record arrays, immutable updates, overflow recovery, CLI formatting and local package/project imports. The shared worker harnesses check guarded output, failed-observation isolation and retained results after scheduler destruction. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-narrow-arrays-2026-10-18.json) records the final checks and scope.
