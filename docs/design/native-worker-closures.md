# Captured closures as local CPU worker arguments

Current aggregate extension: [proven function-containing CPU worker values](native-worker-callable-aggregates.md) now support records, ordinary enums, function arrays, record arrays and transitive captures. Unproven parameters, expected-error payloads and indirect spawn remain staged. Earlier subset restrictions below describe the preceding increments.

Implemented native subset — 2026-10-20. [Direct nested function captures](native-worker-nested-closures.md) are now also supported; this page records the CPU data environment foundation. A statically named local CPU worker can receive a synchronous closure with supported immutable data captures. This extends [named function arguments](native-worker-functions.md) under the [function transport contract](worker-function-transport.md).

```kalandra
fn apply(value: int, transform: fn(int) -> int) -> int:
  transform(value)

fn main() -> int ! TaskError:
  offsets = [20, 20]
  transform = fn(value: int): offsets[0] + offsets[1] + value
  try parallel:
    work = spawn apply(2, transform)
    try await work
```

The result is `42`. Calling `transform` inside `apply` is an ordinary synchronous call. Only `spawn` starts another task. The [factory example](fixtures/worker-closures.k) submits two closures from the same definition with different captured values.

## Supported values

Captures support all ten CPU numeric widths, Bool, String, numeric arrays/tensors, Bool/String arrays, and records/enums with supported data fields transitively. Empty arrays and rank-zero numeric tensors are covered. Existing callable signatures and expected-error handling apply.

The compiler proves provenance through references, immutable aliases, conditionals, exhaustive matches and direct named factory results. Anonymous functions, including capture-free ones, are supported. Every possible target must satisfy the capture restrictions; taking a supported branch once is insufficient. A coordinating recipient can retain its closure argument across suspension and invoke it after awaiting nested work.

## Ownership and validation

Each handoff copies the typed environment descriptor/value storage into the child invocation. Immutable collection and String backing is retained through checked ownership paths. The child does not keep the creator's invocation alive. Multiple copies have independent environment storage and share retained immutable data safely; no deep-copy guarantee is part of the language.

Generated copy routines validate the exact signature against trusted compiler-emitted checked entries. For captured targets, the environment must be an exact live allocation with the target's private allocation key and expected byte size/alignment. Validation obtains an allocation lease before reading environment bytes. Numeric capture views additionally validate dimensions, nonnegative offsets/strides and their addressed range against the backing lease. Canonical inline record field validation remains unchanged.

The full callable is published only after cloning and retaining its captures succeeds. Invalid metadata leaves the destination untouched and raises the existing internal fault. Allocation failure, prior faults and cancellation retain their checked behavior. Partial copies remain owned by the destination ledger until settlement.

Copied environment bytes have a private 1 MiB cumulative limit per invocation. Task environment copies also reserve bytes against the existing root scheduler byte limit, released at invocation settlement. These limits bound copied environment storage; they are not a global heap bound or a claim that shared leaf payload bytes and allocator overhead are fully charged. Changing the private limits does not change source-level parameter arity.

## Remaining transport work

Direct proven function captures are supported recursively. Captured records/enums/arrays containing function values remain rejected. Forwarded function parameters, aggregate projections, indirect factory invocation and pending factory results do not yet establish supported provenance. Functions inside worker argument aggregates, unproven function-valued worker results, spawned-map function captures and indirect spawn remain staged. These are temporary implementation restrictions, not new function types.

GPU callbacks, distributed execution and public C/C++ callback interfaces are separate work. Whole-environment sharing and copy elimination belong to later optimization.

## Qualification

The [CPU qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-worker-closures-2026-10-20.json) records 70 passing selected CTest checks on Apple M2 Max with LLVM/MLIR 22.1.8. This is a focused regression selection, not the full project suite. Implementation and test sources were frozen before the run.

The new execution suites compile/run 92 source programs across debug, optimized, ASan/UBSan and separate ThreadSanitizer modes. Private harnesses check 1,028 calls after creator destruction, 36 invalid/stopped copies, 16 allocation probes, 192 scheduled tasks and 384 cached observations across one/two/four-worker pools. They also cover root byte quota, invocation copy limits and cancellation cleanup. An additional 96 CLI worker runs, 288 allocation-failure probes and 12 preserved rejection artifacts pass. Frontend suites accept 28 worker-callable programs and reject ten unsupported boundaries and three forged IR cases in both ordinary and sanitized builds.

Preflight checks caught and repaired mixed Bool/integer environment padding and noncanonical numeric capture view retention. Existing closure, callable aggregate, error, module, map, recursion and scheduler checks pass with the extended argument support.

Current result extension: [proven function-valued CPU worker results](native-worker-function-results.md) now support cached awaits, use after group release and onward submission. Containing values and unproven provenance remain staged.
