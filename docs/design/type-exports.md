# Native record type exports

A module can export a record declaration with `export type`. Import it by name, optionally with an alias, using the same syntax as function imports.

`jobs.k`:

```kalandra
export type Job:
  names: [String]

export fn decorate(job: Job) -> Job:
  job with {names: map(job.names, fn(name): name + "!")}
```

`main.k`:

```kalandra
from "./jobs.k" import Job as Batch, decorate

fn process(batch: Batch) -> Batch ! TaskError:
  try parallel:
    try await spawn decorate(batch)

fn main() -> [String] ! TaskError:
  job = Batch {names: ["north", "south"]}
  changed = try process(job)
  changed.names
```

The program prints `["north!", "south!"]`. Imported types work in constructors, parameter and return annotations, nested record fields, arrays, maps, immutable updates and local CPU worker calls. All existing field-type and layout rules still apply.

An alias names the original nominal declaration. Two aliases of the same type are interchangeable; identically shaped records declared in different files remain different types. Canonical module paths retain the existing diamond/symlink identity rules. Runtime representation, storage ownership and worker layout identity are unchanged by an import or alias.

Record declarations are private unless explicitly exported. Importing a function that returns a private record still allows inference, field access and updates under the existing rules; it does not expose that record's type name. An exported record may contain a private record field without exporting the private name.

Functions and types share the module declaration/import namespace: a type alias cannot collide with a local function, local type or another imported name. Reserved type/control names cannot be used as type aliases. Imports resolve before record fields and function signatures, so declaration order does not affect type lookup. Ordinary imported names are not automatically re-exported. [Explicit re-exports](reexports.md) can expose selected exported types/functions through another module.

Record type exports remain CPU-only. The common module loader accepts explicit relative `.k` paths and [local package mappings](local-packages.md); [Apple Metal function modules](native-metal-modules.md) do not add imported record types to its numerical subset. Type-only imports participate in the same graph limits, cycle checks, original-file diagnostics and source-output protection. Namespace imports, export lists, package installation/version resolution, GPU record/type support and public native library interfaces remain separate work.

The focused qualification covers debug, optimized, ASan/UBSan and TSan execution; aliases and nominal identity; imported nested and collection fields; captures, maps and worker results; malformed/failed transfers; visibility/collision rejections; and this two-file example. See the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-type-exports-2026-10-18.json).
