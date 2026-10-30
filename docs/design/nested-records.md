# Nested immutable CPU records

A native CPU record can contain another named record, including through several levels. Leaves still use the six implemented numeric types or `Bool`. Field access, immutable updates, helpers and [CPU workers](record-workers.md) preserve each nested declaration's nominal type.

```kalandra
type Scene:
  position: Point
  enabled: Bool

type Point:
  x: int
  y: int

fn move(scene: Scene) -> Scene:
  scene with {
    position: scene.position with { x: scene.position.x + 10 }
  }

fn main() -> int ! TaskError:
  original = Scene { position: Point { x: 2, y: 3 }, enabled: true }
  moved = try parallel:
    try await spawn move(original)
  moved.position.x + original.position.x
```

The result is `14`. `original.position.x` remains `2`; `moved.position.x` is `12`. A declaration may refer to another record declared later in the same file, as `Scene` does here.

## Values, names and updates

A nested field has its declared record type. Supplying a different record with identical fields is a type error. Declaration identity survives imported helper results and worker calls at every level. Importing a type name is still separate module work; a returned nested value can be accessed and updated without importing its private type names.

Read nested fields with `scene.position.x`. To change a nested field, derive its replacement explicitly with `scene with { position: scene.position with { x: 10 } }`. Dotted assignment and dotted update-field names are not supported. This keeps construction and update rules the same at every level: named fields, complete construction, and optional replacement fields of the same types.

If two fields contain the same child value, deriving a change to one does not change the other or the original. Initializers retain written evaluation order. An update evaluates its base before its replacements, including nested updates. Checked failures stop evaluation and cannot publish a partially completed record.

Nested records work inside synchronous numeric map callbacks and with numeric array/tensor worker arguments or results. [Collections stored inside records](record-collections.md) are now supported. [Whole-record captures in anonymous numeric callbacks](record-closures.md) are also supported.

## Storage and finite layouts

Records containing only numeric/Boolean leaves are pointer-free inline values. CPU MLIR and LLVM task frames use nested native structs, with cached target types for shared child declarations. Captures and completed outcomes copy immutable values. No nested backing-storage lease or per-record heap allocation is required; scheduler frames and bookkeeping still use the existing bounded task-memory budget.

An inline record cannot contain itself directly or through a cycle of record fields: that would require infinite storage. The checker reports the field that closes the cycle, including for unused declarations. The native lowering validator applies the same check to typed IR before recursive type construction.

Native layout validation also applies compiler resource budgets: by default, at most 128 record levels and 300,000 expanded numeric/Boolean leaves per record. Repeated embeddings count each child's leaves, even when its target type is cached. This prevents a short sequence of declarations from requesting exponentially large inline storage or printed type descriptions. These are implementation budgets, not new source-language types or allocation controls. Existing syntax and semantic budgets also apply.

[Collection fields](record-collections.md) now have an owned CPU baseline. [Record arrays](record-arrays.md) are also supported outside record fields. [Type-name exports](type-exports.md) are also supported. Recursive structures behind managed storage, general record display and GPU/mixed-target records remain separate work. Native distributed execution still requires the agreed design discussion. The internal struct and task layouts are not a public C/C++ or wire ABI.

## Qualification

On Apple M2 Max, all 113 focused checks passed. Nested-record programs passed in debug, optimized, ASan/UBSan, and separate ThreadSanitizer builds: 100 compiled source programs and 344 trusted nested task-harness runs across those variants. The harnesses checked 4,880 snapshot/cache/layout/settlement cases, including 4,608 exact raw-bit successes at one, two and four workers, with three record levels and mixed Boolean/integer metadata. Coverage includes source packet overwrite, repeated observations, cached outcomes, guarded output, cancellation/deadline, admission/allocation failure and surviving values after scheduler destruction. Frontend checks cover nominal child types, forward declarations, cycles, cached nesting depth, expanded-layout budgets and malformed typed IR. Generated programs cover nested updates, shared children, imported results, maps/collections, recovery and fault evaluation order; invalid cycles preserve existing output artifacts. Prior record/worker/collection/module/compiler suites and selected Metal regressions passed. All 640 tracked numerical artifacts and 20 numerical/checked-ABI/runtime/storage sources remain unchanged. This is focused qualification, not a full-suite claim; the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-nested-records-2026-10-18.json) records scope and hashes.
