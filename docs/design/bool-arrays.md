# Immutable CPU Bool arrays

`[Bool]` is a homogeneous immutable array of Boolean values. Nonempty literals infer their element type; empty literals need an annotation or callback context.

```kalandra
fn positive(value: int) -> Bool:
  value > 0

fn main() -> [Bool] ! TaskError:
  flags = try parallel:
    try await spawn map([-1, 0, 2], positive)
  flags -> map(fn(flag): not flag)
```

This returns `[true, true, false]`. `map(flags, fn(flag): not flag)` is equivalent to the pipeline form. A map is sequential; spawning it sends the whole map to a CPU worker.

## Values and operations

Bool arrays support literals such as `[true, false]`, contextual empty arrays, helper parameters/results, conditional length joins, `length(flags)`, checked indexing, anonymous callback captures, imported helpers and retained worker captures/results. Indexes use the same checked int32/uint32/int64/uint64 rules as numeric and record arrays. An element read produces a scalar `Bool`, usable with `not`, `and`, `or`, `==`, `!=` and conditionals.

Named and anonymous map callbacks can take or return Bool, numeric scalars or records. For example, a predicate maps numbers to Bools, a helper with an `if` body maps Bools to numbers, and a record constructor maps Bools to records. The result preserves the input length facts. No implicit conversion exists between Bool and a numeric type. Empty maps do not execute callbacks; `map([], fn(flag: Bool): not flag)` obtains its element type from the annotation. Existing synchronous-callback and capture rules still apply.

The development CLI prints Bool-array results as `[true, false]`, supports `--limit N` and `--all`, and validates the entire result before writing output, including lanes hidden by a display limit.

## Storage and worker boundaries

Each element occupies one byte, with canonical values `0` and `1`. This is an internal representation, not a promise of a public C or wire ABI. Scalar expressions still use Boolean IR values. Generated stores produce canonical bytes; generated reads check the byte before converting it to a scalar Bool.

The compiler reuses invocation-owned storage and the canonical rank-one descriptors used by record arrays. Descriptor checks validate ownership, range, length, offset and stride before access. Numeric and record storage paths keep their existing element identities. Bool arrays have a distinct private task identity from scalar Bool and other collections.

Worker handoff validates every byte before publishing a retained result, polling cancellation every 256 elements. The array backing remains alive through nested tasks, repeated/cached awaits and later users after the source invocation ends. A failed or cancelled copy leaves the destination result unpublished. Partial ownership settles with its invocation. No new allocator or lifetime syntax is exposed.

## Current limits

This adds CPU Bool arrays, not Bool tensors or GPU Bool execution. [Bool-array fields inside records](record-bool-fields.md) are supported. Nested arrays, collection-valued callbacks, escaping closures, explicit parallel mapping and collection comparison remain separate work. Arithmetic and whole-array logical operators are rejected; use `map` for per-element operations. Immutable arrays have no indexed write operation.

## Qualification

On Apple M2 Max, all 123 selected checks passed across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds, including prior scalar Bool, numeric/record arrays, maps, worker ownership, compiler and selected Metal regressions. The new execution suites compiled 124 source programs and ran 256 worker harness invocations. Their 160 success samples contained 174,200 Bool cells at one/four workers and lengths 0, 1, 257 and 4097. Coverage includes canonical bytes, descriptor rejection, retained/cached results, source and scheduler settlement, cancellation/deadlines, admission and allocation failure, bounds and arithmetic faults, recovery, imported helpers, CLI limits, immutable inputs and rejected-artifact preservation. The suites also ran 96 CLI allocation sweeps; storage tests reject malformed descriptors and noncanonical bytes, and check polling and retained display. The documentation example runs in all four variants.

All 640 tracked numerical artifacts and 18 protected numeric-engine/storage/checked-ABI files remain unchanged. The two runtime source/header files add the Bool-array validation and retention helpers. This is focused qualification, not a full-suite claim. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-bool-arrays-2026-10-18.json) records coverage and hashes.
