# Apple Metal function modules

Native Apple GPU programs can share functions across files. Imports, aliases, explicit re-exports, local package mappings and project manifests use the same module loader as CPU programs. A function keeps its declaring file's private helpers and imports when called on either target.

`math.k`:

```kalandra
fn factor() -> float32:
  2

export fn scale(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  values * factor()
```

`main.k`:

```kalandra
from "./math.k" import scale

fn main() -> float32 ! TaskError:
  try parallel:
    values: Tensor[float32, 3] = tensor([1, 2, 3])
    gpu = devices.gpu()
    input = try await transfer(values, gpu)
    result = try await spawn scale(input) on gpu
    host = try await transfer(result, devices.cpu())
    reduceSum(host)
```

```sh
build/native/native/kalandra-check --placement main.k
build/native/native/kalandra main.k --target metal -o build/app
./build/app/program
```

The result is `12`, printed as float32 bits `41400000` by the development Metal runner. The bundle contains the executable and compiled shaders. Keep them together when moving it; source files and manifests are not needed at runtime.

## Composition and scope

The same exported function can be called synchronously on CPU, spawned in a CPU task, or used as a GPU entry when its body and transitive helpers meet the target's capabilities. Helpers with the same spelling in different files have distinct identities. Dependency functions named `main` do not replace the root entry. Imports preserve signatures rather than introducing a runtime lookup or network call.

Exported mixed coordinators can own parallel scopes, upload inputs, launch GPU work, download results and recover task failures. They can be called directly or spawned from another module. The common continuation planner and scheduler handle suspension and owned results as they do for single-file programs; importing a coordinator does not block a worker while it waits.

Use `--package math=path/to/public.k` for explicit offline package entries, or `--project path/to/kalandra.toml` for a [native project manifest](native-projects.md). For example:

```toml
format = 1

[project]
entry = "main.k"

[packages]
math = "math.k"
```

With this mapping, the import becomes `from "math" import scale`. Compile using `kalandra --project kalandra.toml --target metal -o build/app`. Manifest paths resolve from the manifest directory; relative source imports resolve from their declaring file, independently of the compiler's working directory.

## Target limits and diagnostics

Module support preserves the current Metal compiler subset: explicit function result annotations, float32 scalar and rank-one/rank-two tensor helpers, explicit residency/transfers, and supported elementwise, reduction and matrix operations. Kernel signatures accept [fixed and dynamic extents](native-gpu-fixed-shapes.md), with known mismatches diagnosed and unknown sizes guarded before dispatch. This does not add GPU integer/float64 arithmetic, imported record/error types, managed strings, arbitrary callable values or task coordination inside shaders. CPU-only numerical helpers can coexist with GPU helpers when the existing mixed compiler admits their signatures.

Private/missing exports, duplicate bindings, cycles and loader resource limits follow the [common module rules](modules.md). Semantic and source-capability diagnostics refer to the original file and source line. Runtime fault payloads still carry line/column without a file identity; that remains diagnostics work.

Every loaded source and the selected manifest are protected from replacement by generated output files, including existing hard-link and symlink aliases and shader build intermediates. Graph/checking/capability errors preserve existing output artifacts. Build-tool failures retain the existing Metal build behavior; this is not a transactional bundle publisher.

## Implementation

The Metal command loads the canonical source graph before placement checking. The shared checker resolves function imports in placement mode and assigns module-qualified identities. The CPU bridge projects synchronous helper functions from that graph and removes corresponding coordinator imports from its projection; coordinator identities remain in the placement IR and continuation plans. CPU numerical lowering, GPU arithmetic lowering, shader interfaces and task ownership are unchanged.

Qualification includes execution in debug, optimized, ASan/UBSan and separate ThreadSanitizer builds, with one, two and four workers; imports/re-exports, offline packages, manifests, matrix and reduction kernels, coordinating helpers, failure recovery, source protection, rebuild freshness and source-independent relocated bundles. The guide example is compiled and run from this document. Details are recorded in the [evidence record](../../native/metal-proof/evidence/gpu-local-modules-2026-10-20.json).
