# Nested closure captures in local CPU worker arguments

Current aggregate extension: [proven function-containing CPU worker values](native-worker-callable-aggregates.md) now support records, ordinary enums, function arrays, record arrays and transitive captures. Unproven parameters, expected-error payloads and indirect spawn remain staged. Earlier subset restrictions below describe the preceding increments.

Implemented native subset — 2026-10-20. A closure passed to a statically named CPU worker can now directly capture other proven synchronous function values. This extends [CPU data captures](native-worker-closures.md) under the [worker transport contract](worker-function-transport.md).

```kalandra
fn apply(value: int, transform: fn(int) -> int) -> int:
  transform(value)

fn main() -> int ! TaskError:
  offsets = [20, 20]
  base = fn(value: int): offsets[0] + offsets[1] + value
  transform = fn(value: int): base(value)
  try parallel:
    try await spawn apply(2, transform)
```

The result is `42`. Both closures and the captured array remain valid independently of the submitting invocation. Calling `base` is synchronous; capturing or copying it does not start another task. A [complete factory example](fixtures/worker-nested-closures.k) constructs different nested environments from the same definitions.

## Supported provenance and signatures

Direct function captures can themselves capture supported CPU data or further proven functions. Immutable aliases, conditional/match alternatives and direct named factory results retain conservative provenance checking. Every possible function target and its captures must qualify. Captures can have different signatures, including zero arguments, String parameters and typed expected-error channels. All ten numeric widths use the same transport implementation.

Function-valued parameters forwarded through factories or worker arguments still lack proven provenance. Functions inside captured records/enums/arrays remain staged, as do function-containing worker argument aggregates, unproven function-valued worker results, spawned-map function captures and indirect spawn. These restrictions remain diagnostics; they are not new source-level types or permanent language limits.

## Recursive copying and bounded execution

Generated copy entries are registered before their bodies. Nested captures with equal or mutually dependent signatures reuse those entries, avoiding compiler expansion of the environment graph. Each nested function is checked against the trusted exact-signature target set. Tagged allocation leases validate its environment before reading it. Its environment storage is copied and immutable data backing is retained through the existing checked paths.

Only a fully copied callable is published. Failed nested copies leave the external output untouched, and partially constructed environments remain in the destination allocation ledger until settlement. Distinct captures of the same immutable closure may receive separate environment copies; sharing entire environments is a later optimization.

Copy entries use the existing checked call admission, using physical stack headroom checks on the executing thread. They unwind only the admission they acquired. This bounds recursive traversal, including forged cycles in private metadata, and preserves existing faults and cancellation. Generated copy functions also receive conservative native stack-size validation. Exceeding call admission produces `StackExhaustionFault`; failed storage reservation produces the existing checked allocation fault.

The existing cumulative 1 MiB copied-environment limit and shared scheduler byte reservations apply to the complete copied graph. Retained leaf payloads and allocator overhead are not a global heap bound. Limits are private resource guards, not a source-level nesting or parameter-arity guarantee.

## GPU scope

This is CPU worker transport. It does not introduce GPU function pointers, device-side managed environments, or GPU callback support. The source syntax, type checking and shared IR remain reusable, while device execution is qualified for an explicit supported subset. CPU support for a feature does not require duplicating that feature on GPU before the language can ship. GPU-compatible numeric functions can be considered for static specialization/inlining separately from runtime-selected managed closures.

## Qualification

The [CPU qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-worker-nested-closures-2026-10-20.json) records 32 passing selected CTest checks on Apple M2 Max with LLVM/MLIR 22.1.8. This is focused qualification, not the full project suite. Implementation and tests were frozen before the final run.

New suites compile/run 80 source programs across debug, optimized, ASan/UBSan and separate ThreadSanitizer modes. Private harnesses cover 516 calls after creator destruction, 40 invalid/stopped environment graphs including forged cycles, 20 copy allocation probes and four cases preserving preexisting call admission. They run 144 tasks and 288 cached observations across one/two/four-worker pools, plus quota and cancellation checks. An additional 72 CLI worker runs and 384 allocation-failure probes check execution and cleanup. Both frontend modes accept 37 worker-callable programs and reject ten unsupported boundaries and three forged IR cases.

Existing named/data-capturing worker arguments, ordinary and block closures, callable aggregates, recursion, expected errors and module execution retain their selected regression coverage.

Current result extension: [proven function-valued CPU worker results](native-worker-function-results.md) now support cached awaits, use after group release and onward submission. Containing values and unproven provenance remain staged.
