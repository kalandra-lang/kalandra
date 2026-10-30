# Scoped offline package resolution

The compiler library now accepts an explicit, already-resolved package graph
through `loadPackageModules`. This implements the import/type-identity foundation
of [N3-A](native-packages.md#implementation-sequence-and-gates). The graph API is separate from
[exact local dependency installation](native-package-install.md), which now builds
these graphs for compiler/checker and Metal source commands. Public locks remain
planned.
[Library manifests](native-library-manifests.md) now supply metadata, public entries
and local mappings through `NativePackage::node`. The existing
`loadModules`, `--package` and `[packages]` workflows keep their previous global
local-file mapping behavior.

## Ownership and imports

`PackageNode` supplies an opaque resolved `identity`, a filesystem `root`, a
public source `entry`, dependency aliases (`PackageDependency`) and optional
local file mappings (`ModulePackage`). All paths are explicit filesystem paths;
relative paths are interpreted against the process working directory, as in the
existing local-mapping API. A future manifest/lock resolver is responsible for
constructing these paths from each manifest's location.

Node zero owns the workspace entry. Other nodes represent managed libraries.
Each bare import uses only the importing owner's dependency edges and local
mappings. Following a dependency edge switches to that node's owner; following
a relative source import or local file mapping retains the current owner.
Libraries never inherit their caller's aliases. An alias cannot appear twice in
one scope, including across dependencies and local file mappings.

A module is identified by its owner node and canonical physical path. Two aliases
of the same node therefore share declarations, records and enum families. Two
nodes remain distinct even if their entries point to the same physical file.
An application and a transitive library can use different versions of geometry
without flattening either one's aliases. Passing one version's nominal record or
enum to the other version is rejected by the existing semantic checker.

Resolved identity is supplied by the caller, not guessed from metadata versions.
A graph must use one node per identity: duplicate identities are rejected rather
than merging potentially inconsistent dependencies. This API does not verify Git
commits or snapshot integrity; those belong to the installation/lock resolver.

## Confinement and bounds

Managed source modules must remain inside their owner's canonical root, including
public entries, relative imports and local mappings. Canonical containment checks
reject parent-directory and symlink escapes before reading the escaped source.
The workspace retains its existing explicit parent-relative local-file behavior.
This is a compiler import boundary, not an operating-system sandbox or a defense
against concurrent filesystem mutation. Snapshot installation/integrity controls
remain separate work.

Before reading source files, the loader validates node identities, roots, entry
spellings, aliases, target indices and dependency cycles, including unused
edges. Dependencies cannot point back to workspace node zero. Canonical source
containment is checked when a module is visited. Unused source entries/mappings
are not loaded. `M007` reports invalid scoped graphs and confinement failures;
existing source errors retain their original filenames and locations.

`ModuleLimits.maxFiles` independently bounds graph nodes, total dependency/local
mapping edges and instantiated source modules. `maxDepth` bounds dependency and
source-import paths, including longer paths through previously visited shared
nodes. `SyntaxLimits.maxSourceBytes` bounds combined instantiated source text.
Two owners using one physical source consume two module/byte budget entries.

Source overlays remain keyed by canonical physical path. An unsaved buffer applies
to all owners of that file without merging their nominal declarations. The loader
preserves physical paths in composed source parts, so downstream diagnostics and
source/output collision checks can still identify every visited source file.
No files are written by loading or checking a graph.

## Qualification and next work

Focused acceptance covers private aliases, two dependency versions, shared record
and enum identity, distinct owners of the same physical source, re-exports,
unsaved overlays, original diagnostics, canonical escapes, cycles and budgets.
A compiled application calls geometry v2 directly and geometry v1 through a worker
in telemetry, producing `[12, 11]`. Generated code is exercised in debug,
optimized, address/undefined-sanitizer and thread-sanitizer configurations with
one and four workers. Sanitizer configurations also instrument the generated LLVM
functions for address/thread checks. Existing local-module, package and type-export
acceptance guards the original mapping API.

Exact-commit resolver/activation, managed output protection and
[public HTTPS transport](native-remote-git.md) are implemented.
[Public exact-commit locks](native-package-lock.md) and frozen/offline modes are implemented.
[Branch/tag resolution](native-git-refs.md) preserves pins. Authenticated/SSH transport, editor integration and scoped Metal execution
qualification remain follow-ups in
[N3-A–D](native-packages.md#implementation-sequence-and-gates).
