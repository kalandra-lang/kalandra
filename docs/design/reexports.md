# Explicit native module re-exports

Use `export from "./file.k" import name` to expose selected exported functions and record types through another module. Aliases become that module's public names. Ordinary imports remain private.

`jobs.k`:

```kalandra
export type Job:
  names: [String]

export fn decorate(job: Job) -> Job:
  job with {names: map(job.names, fn(name): name + "!")}
```

`library.k`:

```kalandra
export from "./jobs.k" import Job as Batch, decorate as process
```

`main.k`:

```kalandra
from "./library.k" import Batch, process

fn main() -> [String] ! TaskError:
  original = Batch {names: ["north", "south"]}
  changed = try parallel:
    try await spawn process(original)
  changed.names
```

The program prints `["north!", "south!"]`. A public module can combine selected declarations from several internal files. The re-exported names also work locally within that public module. Another public module can explicitly re-export them again.

Re-exporting preserves the original declaration. Record aliases do not create new nominal types or duplicate schemas. Function aliases keep their parameter/result types, fallible channel and original helper/callback scope. They work as ordinary calls, map callbacks and local CPU worker targets under the existing rules. A caller's same-named private helper cannot change the re-exported function's behavior.

A source module must explicitly expose a declaration before it can be re-exported. Private types/functions and names available only through ordinary imports cannot be exposed through this syntax. Re-exporting an alias exposes its new name, not the original spelling. Duplicate names and conflicts with local declarations or other imports are errors; there is no last-wins behavior or implicit preference between functions and types.

All imports and re-exports use the existing explicit relative `.k` paths and bounded acyclic module loader. A file containing only re-exports is a valid library module. It does not supply the consuming program's `main`. Canonical diamonds/symlinks preserve source identity; cycles are rejected. Export resolution caches original identities and rejects cycles even in an in-memory forged graph.

Function re-exports also work in [Apple Metal module graphs](native-metal-modules.md), within that compiler's existing numerical and placement subset. Record/type re-exports remain CPU-only. Wildcards, namespace imports, export lists without a source module and package installation/version resolution remain separate work. [Explicit local package mappings](local-packages.md) can select public entry modules by name. The runtime, worker ABI and record layout are unchanged.

Qualification covers four build modes (debug, optimized, ASan/UBSan and TSan), chained and diamond aliases, named callbacks and helper scope, fallible functions, six numeric families, retained workers, malformed/failing transfers, visibility/collision rejections and this three-file example. See the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-reexports-2026-10-18.json).
