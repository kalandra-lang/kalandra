# Native local function modules

The native CPU compiler and checker load explicit local module graphs. The [Apple Metal compiler](native-metal-modules.md) also loads function modules, including shared numerical helpers and mixed CPU/GPU coordinators within its existing value/type subset. Functions are private to their declaring file unless marked `export`.

`math.k`:

```kalandra
fn scale(value: int, factor: int):
  value * factor

export fn double(value: int):
  scale(value, 2)
```

`main.k`:

```kalandra
from "./math.k" import double as twice

fn main():
  values = [1, 2, 3] -> map(twice)
  values -> tensor() -> reduceSum()
```

Compiling `main.k` produces an executable that returns `12`. An imported function has the same ordinary call, pipeline, type-checking and worker behavior as a local function. This is the first local function-module subset; explicit offline package mappings are also supported; installation/version resolution and broader declaration forms are separate work.

## Imports and visibility

Use `from "./file.k" import name`, optionally with `as alias`. Commas separate multiple imports, for example `from "./math.k" import double, total as sum`. Import declarations are top-level declarations. Their location among other declarations does not introduce runtime initialization or change function visibility.

Paths require an explicit `.k` suffix and a `./` or `../` prefix. They resolve relative to the importing file's canonical location, independently of the process working directory. UTF-8 filenames are supported. Bare names can select [explicitly mapped local package entries](local-packages.md), supplied with `--package name=path.k`. Absolute import paths, extension searches, directory entry-point searches and string escapes in paths are not implemented. Package installation and version resolution are separate work.

Named `export fn` declarations and [exported record types](type-exports.md) can be imported. A dependency's private helper stays private, even when the importing file has a helper with the same name. Imported aliases are local to the importing module. A function always resolves its own helpers and imports in its declaring module, including calls inside anonymous map callbacks. There is no accidental lookup in a caller's module.

Duplicate local function/import bindings, duplicate aliases, missing exports and attempts to import private functions are diagnosed. Numeric conversion names and `Bool` cannot be used as import aliases. Existing name rules still apply: local values can shadow function names, but they do not become callable functions. User-defined or imported functions can explicitly shadow ordinary non-reserved builtins.

Ordinary imports are not automatically re-exported. Use [explicit re-exports](reexports.md), such as `export from "./math.k" import total as sum`, to expose selected functions or record types through a public entry module. Namespace imports, wildcard imports, export lists and top-level value declarations are not part of this subset. Record types can now be explicitly exported with `export type`; imported aliases preserve nominal identity.

## Types, workers and entry points

The complete source graph shares one checked function IR. Each import refers to the original function identity; aliases do not copy its implementation or erase its types. Concrete numeric types, arrays, tensors, shape requirements, Bool, return inference and the existing TaskError channel keep their current rules. Already typed values do not convert implicitly at a module boundary. Synchronous functions can now accept and return [CPU records](records.md), including nested records. Callers can infer an imported record value's original nominal type, read its fields and derive updates; the type name stays private unless the declaration is explicitly exported and imported. [CPU workers](record-workers.md) can also capture and return those imported record values; Apple Metal function-module integration is supported within the [documented numerical subset](native-metal-modules.md).

Imported functions can be named map callbacks, called from anonymous callbacks, and dispatched directly or through nested CPU workers. Captures and retained results use the existing ownership and task machinery. Dependencies are compiler inputs; they are not dynamically loaded by a running program.

A compiled program still requires its own root-file `main`. A dependency's `main`, or an import alias named `main`, does not become the program entry point. Internal test interfaces remain private and are not a public library ABI. Source `export` controls cross-file visibility; supported object/static/shared-library packaging still requires its separate ABI and lifecycle work.

The placement checker and Metal compiler accept loader-resolved function imports, aliases and re-exports. Unresolved in-memory imports remain errors. Imported GPU kernels retain the same numeric, placement and transfer restrictions as single-file kernels; this does not enable imported record/error types, strings or arbitrary callbacks on GPU. See [Metal modules](native-metal-modules.md) for supported scope. Native distributed execution still requires the agreed design discussion.

## Loading, diagnostics and build outputs

Each canonical source path is loaded once per compilation. Diamond dependencies and symlink aliases share that module identity. Independent compilations read the graph afresh; there is no persistent module cache. Hard-link paths are not promised to share a module identity.

Cycles are rejected in this first subset, including cycles through symlinks. There is no partial module initialization or order-dependent cycle behavior. The default graph limits are 128 files and 32 import edges of depth. Depth checks also cover previously loaded shared subgraphs. The 16 MiB source budget includes all files and their composition separators, while token/expression limits apply to the combined graph. These are implementation resource limits, not language size semantics.

Every file is parsed independently before composing the graph for checking. This prevents one file from closing another file's unfinished syntax. Source storage retains original files and maps composed byte spans back to them. Parser/type diagnostics include the original filename, line, byte column and excerpt. Multi-file emitted MLIR includes source locations. Runtime fault payloads preserve the original line/column; adding file identity to those payloads remains separate diagnostics work.

The compiler protects every imported source from output replacement, including executable, LLVM and MLIR destinations and aliases through symlinks or hard links. Graph, parse and semantic failures preserve existing requested output artifacts.

The development commands are:

```sh
build/native/native/kalandra-check path/to/main.k
build/native/native/kalandra path/to/main.k -o path/to/program
```

`kalandra-parse` remains a single-file syntax tool; parsing import syntax alone does not load dependencies. In-memory users of the frontend must call `loadModules` before checking a graph. `check(parse(source))` rejects unresolved imports.

## Implementation

The parser retains import names, aliases, paths, export flags and each function's module identity. The loader canonicalizes and bounds the dependency graph, validates each file, then composes source storage while retaining original-file mappings. Semantic resolution uses module-local function/import tables and gives dependency functions distinct internal names. Existing typed calls, continuation planning, checked MLIR/LLVM lowering and task storage remain shared. Module composition does not add a second numerical backend or change allocation ownership rules.

## Qualification

On Apple M2 Max, all 90 focused checks passed, including generated execution in debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. The module suites compiled 92 source programs across those variants and ran 1,200 worker harness invocations. Coverage includes all six numeric types, imported Bool workers, shape contracts, scoped named/anonymous callbacks, direct/nested tasks, retained array results, cached awaits, empty arrays, recovery, cancellation, deadlines, admission and allocation failures. Frontend checks cover canonical diamonds and symlinks, parent-relative/UTF-8 paths, private/missing/duplicate bindings, graph limits including cached-subgraph depth, cycles, original-file diagnostics and entry isolation. Rebuild freshness, executable/LLVM/MLIR source protection through symlinks/hard links, and failed-build artifact preservation also passed. Existing syntax/semantic/collection/numeric/control frontends, representative collection runtime checks, source/task/storage/continuation and selected Metal regressions passed. All 640 tracked numerical artifacts and 20 numerical/checked-ABI/runtime/storage sources remain unchanged. This is focused qualification, not a full-suite claim; the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-local-modules-2026-10-18.json) records scope and hashes.

[Native project manifests](native-projects.md) provide an explicit, versioned root entry and local package mappings for compiler/checker invocations; standalone source builds retain their existing behavior.
