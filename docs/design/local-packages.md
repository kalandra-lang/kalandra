# Explicit local native packages

A native build can assign names to local library entry files. The source uses the name; the compiler invocation supplies its location. This is an offline composition mechanism, not a package installer or registry.

`geometry/public.k`:

```kalandra
export type Point:
  x: int
  y: int

export fn total(point: Point) -> int:
  point.x + point.y
```

`main.k`:

```kalandra
from "geometry" import Point, total

fn main() -> int ! TaskError:
  point = Point {x: 20, y: 22}
  try parallel:
    try await spawn total(point)
```

Check and compile using the same mapping:

```sh
build/native/native/kalandra-check main.k --package geometry=geometry/public.k
build/native/native/kalandra main.k --package geometry=geometry/public.k -o program
./program
```

The program prints `42`. Repeat `--package name=path.k` for multiple libraries. The checker accepts mappings before or after the source argument; the compiler takes its source first, followed by options. The frontend API accepts `ModulePackage` entries through `loadModules` too.

Names are case-sensitive ASCII identifiers with optional hyphens: the first character is a letter or underscore, followed by letters, digits, underscores or hyphens. Entry paths must end in `.k`; relative mapping paths resolve from the compiler's working directory. A mapped file's own relative imports resolve from that file's canonical directory, as usual. A name selects exactly one entry file: there is no extension search, directory-main convention or implicit filesystem/package-cache lookup.

Mappings apply to the whole compilation graph, including library dependencies and explicit re-exports. Dependency names must also be explicitly supplied by the caller. This flat invocation-level namespace is an initial foundation; independently versioned/private dependency graphs require a later package design.

An entry can export functions/types directly or re-export selected declarations from internal files. All visibility, nominal type identity, callback scope and worker ownership rules are unchanged. Mapping two names to the same canonical file does not duplicate its declarations. Relative and package imports of that file share one identity.

Unknown package names, malformed/duplicate mappings, missing used files and graph cycles are errors. Unused valid mappings do not load files. Mapping count is bounded by the configured file limit; the existing graph file/depth/source/token limits also apply. Original-file diagnostics and executable/LLVM/MLIR source-output protection cover package files and their dependencies, including symlink and hard-link aliases.

Each target keeps its own capability restrictions. [Apple Metal builds](native-metal-modules.md) accept the same explicit mappings for supported numerical function modules and CPU/GPU coordinators. Package mappings do not enable native distributed execution or public shared-library output. [Native project manifests](native-projects.md) can now record the root entry and these local mappings using manifest-relative paths. Dependency installation, version solving, lockfiles and network fetching remain separate work. The JavaScript prototype's package system is not used.

Qualification covers debug, optimized, ASan/UBSan and TSan execution, transitive mapped dependencies, aliases/re-exports, callbacks and local workers, nominal diamonds, missing/invalid inputs, bounded graphs, source protection and this two-file example. See the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-local-packages-2026-10-18.json).
