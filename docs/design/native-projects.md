# Native project manifests

Use [`kalandra new directory`](native-starter.md) to generate a two-module parallel starter, or write an explicit manifest as below.

Use an explicit native TOML manifest to record the root source file and local library entries. Both the compiler and checker accept `--project path/to/kalandra.toml`.

`geometry.k`:

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
  try parallel:
    try await spawn total(Point {x: 20, y: 22})
```

`kalandra.toml`, in the same directory:

```toml
format = 1

[project]
entry = "main.k"

[packages]
geometry = "geometry.k"
```

Check, compile and run:

```sh
build/native/native/kalandra-check --project path/to/kalandra.toml
build/native/native/kalandra --project path/to/kalandra.toml -o program
./program
```

The program prints `42`. Entry and library paths resolve from the canonical manifest directory, independently of the compiler's working directory. Output paths supplied on the command line still resolve from the working directory. A symlink to a manifest uses the original manifest's directory as its base.

## Format and validation

`format` is the integer `1`. `[project]` is required for application builds and has exactly one supported field, `entry`, containing a relative `.k` path. `[packages]` is optional; its keys are the existing case-sensitive local package names and its values are relative `.k` entry paths. Use `/` separators. Parent-relative paths are allowed for application-only manifests. A manifest can also declare a [library package](native-library-manifests.md); its public entry and local mappings must remain inside its canonical root. The manifest itself must have a `.toml` extension; its filename need not be `kalandra.toml`.

Comments, quoted or literal strings and other standard TOML syntax are parsed by [toml++ 3.4.0](https://github.com/marzer/tomlplusplus/tree/v3.4.0), using its TOML 1.0 mode. Unknown fields, unsupported format versions, wrong value types, duplicate TOML keys, absolute paths and retired source extensions are errors. Schema diagnostics include the manifest path and source location. The default manifest limit is 256 KiB and 128 package entries; the frontend API exposes smaller limits for callers.

This native format is separate from the retired JavaScript prototype's manifest. Prototype `[build]` and dependency tables are not silently accepted or migrated.

## Build behavior

Select a source file or a project manifest, not both. Explicit `--package name=path.k` options can add mappings; duplicate names across the command line and manifest are errors, with no implicit override. Command-line mapping paths retain their working-directory base, while manifest mappings retain their manifest-directory base.

There is no ancestor search or implicit project discovery. Each invocation reads the manifest and source graph afresh. Loading a manifest validates its configuration but does not open `.k` source files: the existing module loader loads the root and reachable dependencies afterward. Unused valid package mappings do not load files.

The resulting CPU graph uses the same declarations, type checking, source diagnostics, bounded module loading and worker ownership as explicit source builds. The manifest and every source dependency are protected from executable, LLVM and MLIR output replacement, including aliases through symlinks or hard links. Invalid configuration preserves existing output files.

[Apple Metal builds](native-metal-modules.md) also accept these manifests with `--target metal`, retaining their numerical and placement restrictions. [Library manifests](native-library-manifests.md) now add validated `[package]` metadata, public entries and optional example applications. The internal [scoped offline graph API](scoped-package-resolution.md) implements package-local import/type identity. [Exact dependency installation](native-package-install.md) now adds `[dependencies]`, complete graph activation and offline compiler/checker/Metal source loading. The [native package workflow](native-packages.md) now includes [public HTTPS transport](native-remote-git.md); [public exact-commit locks](native-package-lock.md) and frozen/offline modes are implemented; [branch/tag resolution](native-git-refs.md) is implemented; authenticated/SSH transport and editor integration remain. Registry access, version solving and native library ABI output remain separate work. The manifest is compiler input, not a runtime dependency of the compiled application.

Qualification covers frontend schema/size/count checks, canonical path bases, symlinks and fresh reads, plus debug, optimized, ASan/UBSan and TSan execution of manifest-driven workers and mapped libraries. It includes configuration failures, output preservation, direct/symlink/hard-link manifest protection and this complete example. See the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-native-projects-2026-10-18.json).
