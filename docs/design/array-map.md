# Sequential native array mapping

The native CPU subset implements `map(values, transform)` and its pipeline form for [immutable numeric arrays](arrays.md), [Bool arrays](bool-arrays.md), [record arrays](record-arrays.md), and [String arrays](string-arrays.md). The transform is a named function or an inline anonymous function with one numeric scalar, Bool, record or String parameter and a numeric scalar, Bool, record or String result.

```kalandra
fn double(value: int) -> int:
  value * 2

fn doubled(values: [int]) -> [int]:
  values -> map(double)
```

`doubled([1, 2, 3])` produces `[2, 4, 6]`. The input remains unchanged and usable. `map([1, 2, 3], double)` means the same thing.

## Types and evaluation

A named or explicitly annotated callback parameter gives literal array arguments their element context. Otherwise the input array establishes the anonymous parameter type. Already typed arrays must match that parameter exactly; map does not implicitly convert their elements. The callback result determines the output element type, which may differ from the input:

```kalandra
fn asFloat(value: int) -> float:
  float(value)

fn converted(values: [int]) -> [float]:
  map(values, asFloat)
```

All 100 input/output combinations of the ten implemented CPU numeric scalar types can be represented by ordinary named functions, including [narrow integer arrays](narrow-arrays.md). Explicit conversion still follows the checked numeric conversion contract. Results remain homogeneous arrays of the same length as the input, including its private dynamic length identity. An expected result type does not change the callback's declared/inferred result type.

Callbacks execute sequentially in input order, once per element, with exactly one explicit element argument. Map never supplies an implicit index or collection argument. Empty arrays execute no callbacks; `map([], double)` obtains element context from `double` and returns an empty `[int64]`.

A runtime arithmetic fault stops the map at the first failing element. Its source location points to the failing callback/helper operation. No partial array result is published. Inputs remain unchanged, and all intermediate/output allocation owners settle on failure. Loop execution uses the existing cooperative cancellation and checked allocation machinery.

## Record elements and results

```kalandra
type Reading:
  value: int
  samples: [int]

fn main() -> [int] ! TaskError:
  readings = map([1, 2, 3], fn(value): Reading {value: value, samples: [value]})
  updated = try parallel:
    try await spawn map(readings, fn(reading): reading with {value: reading.value + 10})
  map(updated, fn(reading): reading.value)
```

This returns `[11, 12, 13]`. Maps can convert numbers into records, extract numbers from records, update records immutably, or return a different declared record type. Nested record fields and numeric array/tensor fields use the existing record ownership rules. A nested anonymous callback can capture its enclosing callback's record parameter.

Record identity is nominal: a callback expecting `Reading` rejects a different record declaration with the same fields. Empty inputs require a named callback or an annotated anonymous parameter to establish their element type. Output length facts are preserved across changes in element type. The expected output type does not choose a different record or numeric result type.

Record-result storage uses the same checked target layout as record arrays. Each callback result is stored in a fresh output array; retained immutable numeric field backing may be shared. Construction and iteration remain sequential. Worker boundaries retain both the output record array and its nested collection backing. Failure or cancellation publishes no partial output. Allocation lifetime optimization remains separate work.

## Anonymous callbacks and captures

```kalandra
fn shifted(values: [int], offset: int) -> [int]:
  values -> map(fn(value): value + offset)
```

`map(values, fn(value): value + offset)` is equivalent. The input establishes the element parameter type. Optional annotations can make it explicit: `map([], fn(value: int) -> float: float(value))`. An empty input without an annotated callback parameter has no element type to infer and is rejected.

Inline callbacks can use an expression body or an [indented body inside the call](native-delimited-closures.md). [Block closures](native-block-closures.md) can also be bound first and then passed to `map`. Names resolve lexically; its parameter can shadow an outer binding. Only referenced outer values are captured, including through nested anonymous callbacks. Supported captures are completed CPU numeric scalars, numeric arrays/tensors, scalar Bool values, [Bool arrays](bool-arrays.md) and [immutable CPU records](record-closures.md), and [immutable Strings](strings.md#local-cpu-workers-and-captures). [String arrays and String-valued maps](string-arrays.md) are also supported as inputs, outputs and captures. Task handles, unhandled fallible results, device selectors and device-resident tensors cannot be captured.

The compiler lifts a callback into a private function and passes its captures as additional internal arguments. Captures are established once before iteration; collection storage follows the existing immutable ownership and worker handoff rules. This does not introduce a public closure ABI. [Managed function values](native-closures.md) can retain captures after their creator returns. Empty maps do not invoke the callback, even if its body would fail.

Mapped arrays can feed [explicit rank-one tensor construction](arrays.md#explicit-tensor-construction-from-arrays): `values -> map(double) -> tensor() -> reduceSum()`.

## Ordinary helpers and workers

Callbacks may call other synchronous functions, use CPU conditionals, construct temporary numeric collections/tensors and use the supported synchronous numeric operations. Worker kernel extraction imports the callback's synchronous helper graph with remapped function, value and symbol identities. A mapped result can be retained after its function or worker ends, shared by later calls and obtained from cached awaits.

```kalandra
fn background(values: [int]) -> [int] ! TaskError:
  try parallel:
    work = spawn map(values, double)
    try await work
```

This sends one sequential map to a worker. It does not run individual elements in parallel. Mapping inside an ordinary worker function also works, including nested structured tasks around that function. Checked callback faults become ordinary supervised worker faults; existing catch/recovery and cancellation/deadline settlement rules apply.

## Current limits

This slice accepts named source functions and inline anonymous callbacks with lexical captures. [Named CPU function values](native-function-values.md) also supply bound, passed and returned callbacks, including runtime-selected targets. Inline callbacks can capture a compatible completed function reference. [Returned captured closures](native-closures.md) and [bound anonymous block bodies](native-block-closures.md) are supported. Named or anonymous wrappers can expose numeric conversions; numeric type names themselves are not first-class callbacks yet.

Callbacks must be synchronous throughout their reachable helper graph. [Synchronous error handling and variant matches](native-control-maps.md) are supported in direct named/inline callbacks as well as bound function values. Each callback must handle expected errors before returning a completed element. Task coordination and transfers/device selection are rejected even when a particular branch or input is empty. Completed function values are supported elements; tensor and array callback results remain outside this map subset. GPU/mixed-target mapping is gated. Existing restrictions on ordinary recursion in task-enabled modules still apply.

Explicit parallel mapping, additional element types and broader first-class function support remain separate work. There is no public native callback ABI in this implementation.

## Qualification

On Apple M2 Max, 89 focused checks passed across frontend and native/generated execution, including debug, optimized, ASan/UBSan and separate ThreadSanitizer variants. The map suites ran 2,304 identity-map invocations with 3,159,552 exact lane checks, 12,624 mapped conversion cases with 262,144 exact lane checks, and 1,200 worker harness invocations. The conversion cases reuse independent APFloat/integer oracles. Source ordering, empty maps, helper import, temporary callback allocations, retained outputs, cancellation, allocation failure and invalid-program artifact preservation passed. All 640 tracked numerical artifacts remain unchanged. This is focused qualification, not a full-suite claim. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-array-map-2026-10-18.json) records coverage and source/artifact hashes.

The anonymous-callback extension passed 69 focused checks, including debug, optimized, ASan/UBSan and separate ThreadSanitizer execution. Its six numeric identity closures ran 2,304 invocations with 3,159,552 exact lane checks; 1,200 worker harness runs exercised scalar, Bool, array and tensor captures, nested callbacks, cached results, source faults, cancellation and allocation failure. The prior named-map, array, frontend and selected CPU/GPU regressions also passed. The [capture evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-array-closures-2026-10-18.json) records the scope, test reruns and unchanged numerical artifacts.

The record-element/result extension passed 113 selected checks on Apple M2 Max, across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. Its four generated suites compiled 168 source programs and ran 528 worker harness invocations, including 768 raw-bit success samples containing 246,336 input records, plus 640 CLI allocation-failure sweeps. Coverage includes nested fields/captures, empty maps, imported callbacks, cached results, recovery, cancellation, malformed descriptors, ownership settlement and preservation of rejected output artifacts. The documentation example is compiled and executed in every variant. Eight Metal checks initially lacked sandbox hardware access and passed on rerun with hardware access; two obsolete frontend rejection assertions were updated and passed on rerun. No compiler or runtime changes followed the initial regression run. All 640 tracked numerical artifacts and 20 numeric-engine/runtime/storage/checked-ABI files remain unchanged. This is focused qualification, not a full-suite claim. The [record-map evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-record-array-map-2026-10-18.json) records the scope, reruns and hashes.

[String elements and results](string-arrays.md) now support the same named/anonymous mapping model, including retained worker results. [String-array fields in records](record-string-array-fields.md) are also supported.
