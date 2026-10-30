# Immutable CPU records

The native compiler supports named immutable records with numeric scalar, `Bool`, [nested record](nested-records.md), [numeric collection](record-collections.md), [Bool-array](record-bool-fields.md), and [String](record-string-fields.md) fields. This builds on the [accepted core design](core.md#3-construct-a-record-and-derive-another).

```kalandra
type Point:
  x: int
  y: int

fn shift(point: Point, distance: int) -> Point:
  point with { x: point.x + distance }

fn main():
  original = Point { y: 3, x: 2 }
  moved = shift(original, 10)
  moved.x + original.x + moved.y
```

The result is `17`. `original.x` is still `2`; `moved.x` is `12`.

## Construction and updates

A declaration gives every field an explicit type. Supported fields are `int32`, `uint32`, `int64`, `uint64`, `float32`, `float64`, and `Bool`, including the ordinary `int`, `uint`, and `float` aliases. A field can also use another declared record type; nested updates and finite-layout rules are described in [nested records](nested-records.md). Literals receive the field's declared type; already typed values require the same type or an explicit numeric conversion.

Construction uses named fields in any order. Every declared field must appear exactly once. Unknown, duplicate, missing, or incompatible fields are compile errors. `point.x` reads a field, and `point with { x: 10 }` creates another value of the same nominal type. Omitted update fields retain their values; `point with {}` is valid. Assigning to `point.x` is rejected.

The base of an update is evaluated once, before its replacements. Initializers and replacements are evaluated once in their written order, rather than declaration order. An arithmetic or bounds failure stops evaluation through the existing checked failure path. Parentheses make a derived value's field access explicit, for example `(point with { x: 10 }).x`.

There are no implicit field conversions, positional constructors, default field values, inheritance, record arithmetic, or record equality in this subset. Compare individual fields when needed.

## Functions, branches and modules

Records can be bound, passed to CPU functions, returned with inferred or explicit types, and selected by conditionals. [CPU worker captures and results](record-workers.md) also support these records, including nested tasks and TaskError recovery. Record declarations are visible throughout their declaring file, independent of declaration order. Functions and import aliases cannot collide with a local record type name. A local value may shadow a constructor name; constructing through that shadowed name is diagnosed.

Record identity belongs to its declaration, not its field layout or spelling. Two declarations named `Point` in different files are different types. An imported function can return its module's record, and callers can infer that type, read its fields, derive an updated value, and pass it back to functions expecting the original declaration. [Explicit type exports/imports](type-exports.md) also allow consumers to name that original declaration. Local declarations do not become visible merely because another function was imported.

Records can be used inside synchronous numeric map callbacks. [Anonymous numeric callbacks can capture whole records](record-closures.md), including nested collection fields. [Immutable record arrays](record-arrays.md) now support construction, indexing, length and CPU worker transport. [Sequential mapping](array-map.md#record-elements-and-results) now accepts record elements and record-valued results.

## Native lowering and current boundaries

[Nested records](nested-records.md) now extend these values through several inline levels. The typed IR carries nominal declaration identities and canonical field indices separately from written evaluation order. CPU lowering represents these numeric/Boolean/nested record values as native structs using MLIR's LLVM aggregate operations. Records with numeric/Boolean leaves require no managed heap allocation. [Numeric collection fields](record-collections.md) use owned backing storage and copies on construction and reads. [Bool-array fields](record-bool-fields.md) share their existing immutable backing. The underlying struct layout does not erase source-level nominal type checks. This internal representation is not a published C/C++ ABI or serialization format.

[CPU worker integration](record-workers.md) now supports record captures/results, nested workers, repeated awaits, and record-valued TaskError recovery. GPU placement remains unsupported for record-containing source graphs, and the checker/backend diagnose that combination. User variants/errors and GPU integration remain separate work. Native distributed execution still requires the agreed design discussion.

A program's `main` must return an already supported scalar, `Bool`, array, or tensor result; direct record display is not implemented. Record helper results can feed those outputs, as in the example above.

## Qualification

The initial synchronous milestone is recorded below; [worker integration](record-workers.md) and [nested records](nested-records.md) have their own subsequent qualification.

On Apple M2 Max, all 101 focused checks passed. Record programs passed in debug, optimized, ASan/UBSan, and separate ThreadSanitizer builds: 104 compiled source programs across those variants. Frontend checks also cover invalid declarations/fields, nominal identity, malformed typed IR, and unsupported execution paths. Runtime checks cover numeric endpoints, floating precision/signed zero, unchanged originals, functions/recursion, conditionals, maps, module composition, fault evaluation order, and failed-build artifact preservation. Existing frontend, collection, worker, ownership, continuation, and selected Metal regression checks passed. All 640 tracked numerical artifacts and 20 numerical/checked-ABI/runtime/storage sources remain unchanged. This is focused qualification, not a full-suite claim; the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-records-2026-10-18.json) records the scope and hashes.

Numeric arrays and tensors can now be stored inside records; see [collection field ownership](record-collections.md) for copy semantics and worker retention.

[String fields](record-string-fields.md) now compose with these record rules, including nested records, immutable updates, record arrays/maps and retained local CPU worker arguments/results. [Direct String arrays](string-arrays.md) and [String-array record fields](record-string-array-fields.md) also compose with these rules.

[Record type exports](type-exports.md) allow consumers of a local CPU module to name the original nominal type in constructors, fields and function signatures.

[Function-valued fields](native-callable-fields.md) now support managed callbacks in records and variant payloads within one invocation. Types containing function storage remain rejected as worker arguments, results, captures or expected-error payloads until function transport is implemented.
