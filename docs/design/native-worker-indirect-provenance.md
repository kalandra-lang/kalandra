# Indirect-call provenance for CPU worker values

Implemented native subset — 2026-10-20. Function values and function-containing aggregates produced by synchronous indirect calls can now cross statically named CPU worker boundaries. This extends [parameter forwarding](native-worker-parameters.md); [indirect spawn](native-indirect-spawn.md) now builds on this proof for ordinary CPU execution.

```kalandra
fn double(value: int) -> int:
  value * 2

fn identity(transform: fn(int) -> int) -> fn(int) -> int:
  transform

fn apply(value: int, transform: fn(int) -> int) -> int:
  transform(value)

fn main() -> int ! TaskError:
  factory = identity
  transform = factory(double)
  try parallel:
    try await spawn apply(21, transform)
```

The result is `42`. Aliases, higher-order parameters, runtime-selected factories, callable fields and indexed factory arrays retain their possible function origins. Factories may return closures with owned data captures. Functions returned from local workers can still be used after producer/group completion and submitted onward.

Ordinary synchronous maps also preserve provenance through their element and capture parameters and returned elements. This includes named callbacks, function-valued callback aliases and anonymous callbacks that create closures. A completed map result can supply a named worker argument; spawning the map operation itself with function-containing inputs or captures remains staged.

## Analysis and safety

A bounded monotone worklist propagates possible function identities through immutable values, aggregate construction/projection, branches/matches, direct results, parameter sources and completed task results. Discovering a possible indirect target connects its explicit parameters to that call's arguments and its result to the call result. Lifted captures and map parameters have their own source edges. Recursion and higher-order call chains converge over shared expression identities.

Callable edges require exact structural signatures and nominal record identities. The analysis follows values rather than treating every function with the same signature as a possible target. A local factory does not itself have to satisfy worker capture restrictions: those checks apply to the value actually transported and its transitive captures.

Unknown producers and source-less parameters cannot disappear from a partially known set of targets. An unresolved callee conservatively invalidates incoming function-containing parameters of compatible referenced functions as well as its own result. Missing targets, invalid metadata and analysis-budget exhaustion reject transport. The flow graph and subsequent capture walk each have a private 300,000-step work budget; these are compiler resource guards, not language parameter-count limits.

The analysis remains context-insensitive: every possible caller contributes sources. Aggregate projections conservatively join fields/elements, and branches are not specialized for a known condition. Creating separate instances of the same closure expression does not give them independent proofs. An unused higher-order helper with unresolved incoming values can therefore also prevent a proof elsewhere. More precise context, field and reachability analysis is future compiler work.

The existing runtime still checks exact function signatures, validates copy layouts, copies owned environments and preserves retained backing lifetimes. No worker runtime or callable ABI changes are needed for this milestone. [Function-containing spawned maps](native-worker-callable-maps.md) now use these proofs. [Function-containing spawned collections](native-worker-callable-collections.md) now use these proofs too. [Function-containing worker error payloads](native-worker-error-functions.md) now extend these proofs through failure producers, handlers and narrowed families. GPU callbacks and distributed execution remain separate work.

## Qualification

The focused qualification passed **65/65 selected CTest checks**, with frozen implementation and test sources. Sixteen checks interrupted by host sleep passed on rerun. This is not the full project suite. The execution catalog covers aliases, nested closure factories, higher-order and recursive helpers, runtime-selected targets, callable fields/arrays, synchronous maps, cached worker results, fallible factories, unrelated targets with identical signatures, and local-only factory data that must not cross the worker boundary.

Rejection cases retain unsupported captures behind aliases, branches and maps, plus unresolved incoming factories. Forged-IR checks include a partially known callee with an unknown branch and an out-of-bounds callee. Rejected source programs must preserve existing executable, LLVM and MLIR output files. See the [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-worker-indirect-provenance-2026-10-20.json) for the final checks and results.
