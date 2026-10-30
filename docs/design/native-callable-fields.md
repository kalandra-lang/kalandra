# Function-valued record and enum fields

Implemented native CPU subset — 2026-10-19. Records and enum payloads can store named functions and managed closures with explicit structural signatures. This extends [native function values](native-function-values.md) and [managed closures](native-closures.md).

```kalandra
type Config:
  apply: fn(int) -> int

fn make(offset: int) -> Config:
  Config {apply: fn(value): value + offset}

fn main() -> int:
  config = make(20)
  config.apply(22)
```

The [standalone fixture](fixtures/callable-fields.k) returns `42`. The field type supplies the anonymous parameter and result types. `config.apply(22)` reads the function value and calls it; there is no implicit receiver or `self` argument. The closure keeps its captured `offset` valid after `make` returns.

## Construction, updates and calls

Field signatures use the existing exact function-type rules, including numeric widths, nominal record/enum identities, fixed tensor extents and canonical expected-error channels. Missing fields and incompatible initializers are rejected. Imported types retain their defining module identity and declared signatures.

A field can be read into another immutable binding, passed to a higher-order function, returned, or called directly. Pipelines insert the input as the first argument, including `21 -> config.apply` and `21 -> config.apply()`. Returned factories and nested callable results use the same grouping and independent error-channel rules as other function values.

Record updates produce a new record value:

```kalandra
second = config with {apply: fn(value): value * 2}
```

The original `config` keeps its original callback. Copying a function field copies its code/environment reference; it does not execute the function or mutate its captures. Nested records and ordinary record arrays can carry these fields within the same invocation. [Direct arrays of functions](native-function-arrays.md) can also be stored in fields. Existing array indexing, sequential mapping, slicing and concatenation preserve them.

## Enum and error payloads

Enum variants declare function fields just like record fields:

```kalandra
enum Choice:
  Empty
  Some:
    callback: fn(int) -> int
```

Construct `Choice.Some {callback: fn(value): value * 2}` and bind `callback` inside the matching `Choice.Some {callback}` arm. Payload access outside a matching branch remains rejected. Different variants may declare different signatures. Inactive payload slots are not invoked.

Expected-error payloads can also contain function fields or nested records containing them during synchronous execution. Error unions and aliases preserve the original field signature. A callback field declared as `fn(int) -> int ! InputError` requires the caller to handle, propagate or return that expected error, even when one particular callback always succeeds. [Direct anonymous error declarations](anonymous-errors.md) are supported in its initializer.

## Storage and lifetime

Each function field stores the existing private code/environment pair inline. Closure environments and captured managed payloads remain owned by the invocation until settlement. Returning a containing record from a creator, deriving an updated record, selecting an enum variant, or copying a record array does not end the captured storage's lifetime.

Pure field operations continue through MLIR; expected-error control uses the existing synchronous checked entries. This increment introduces no new runtime layout or allocator mechanism. Inline layout budgets count both pointers in a function field; callable type budgets and signature validation also apply to fields.

This does not establish a public callback ABI. Worker handoff uses independently owned destination environments, so a qualified copied value may outlive its source invocation. Earlier environment reclamation and allocation/copy elimination remain [optimization work](../roadmap.md#optimization).

In compilation units containing recursive values, [managed recursive environments](native-recursive-storage.md) retain captured backing transitively. Checked entry wrappers validate environment ownership, payload size and code identity before loading captures. Function arrays, fallible CPU signatures, workers and error channels now share that ownership path. Device-valued paths remain excluded; the acyclic behavior described above is unchanged.

## Worker boundary

A worker can create and invoke a record or enum containing functions locally, then return a completed value without function storage. A coordinator can also hold such a record across an ordinary worker await.

[Proven CPU callable aggregates](native-worker-callable-aggregates.md) now cross arguments, captures and results. [Spawned maps](native-worker-callable-maps.md), [collection operations](native-worker-callable-collections.md), and [function-containing application-error payloads](native-worker-error-functions.md) are supported through the same checked ownership transfer. Unknown origins and unsupported transitive captures remain rejected.

## Remaining work

- Resumable function values and placement-aware callbacks; [indirect synchronous CPU spawn](native-indirect-spawn.md) is implemented.
- [Recursive records/enums](native-recursive-storage.md) now support synchronous scalar callable fields, including captured environments and nominal recursive signatures. Function arrays are also supported; recursive worker/error/placement paths still need separate qualification.
- Delimiter-aware indented callbacks and the existing direct static-map control-flow restriction.

GPU callbacks, distributed execution and public C/C++ integration retain their separate roadmaps.

## Original stage qualification

All 80 selected checks pass on Apple M2 Max. New execution suites compile 156 programs across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. Relevant existing frontend, function-value, closure, map, module, record, enum, error-union and worker/error regressions pass in their selected debug and ASan/UBSan modes.

Each frontend variant checks 26 distinct accepted programs and 22 rejected programs, plus nine forged-metadata cases and inline layout budgets. Coverage includes exact numeric, nominal, tensor-shape and error-channel signatures, direct and nested fields, immutable updates, variant matching, returned factories, both field pipeline forms, imported identities and all ten scalar widths.

Private lifetime probes invoke returned-record callbacks 256 times after creator exit, exercise 36 context-allocation cases, 32 creator-allocation cases and 12 body-fault cases, and check stopped/failed contexts without publishing partial values. CLI sweeps run 960 allocation-failure processes with exact successful output and balanced failed cleanup. Worker-local use, local error recovery and coordinator-held callbacks additionally run with pools of one, two and four threads. Rejected initializers preserve existing executable and IR artifacts.

The [qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-callable-fields-2026-10-19.json) records selected checks, targeted reruns, per-variant probes and source hashes. It qualifies the implemented macOS ARM64 storage subset, not function transport, direct function arrays, a public callback ABI, performance improvements or other platforms.
