# Native package locks

Native projects now use `kalandra.lock` beside their explicit project manifest.
Commit this file with the application. It selects the complete dependency graph;
`.kalandra/` remains generated, private cache state. The lock is the single graph
activation point. There is no separate private `installed.toml` activation index.

The lock selects exact lowercase 40-digit SHA-1 commits through
[local and public HTTPS Git transport](native-remote-git.md).
[Branch/tag requests](native-git-refs.md) are resolved during installation and stay
pinned afterward. Version-range solving remains separate work.

## File contract

Format 2 records four top-level fields:

- `format`: the integer `2`.
- `manifest_digest`: SHA-256 of the exact root manifest bytes.
- `roots`: the root dependency requests, each with `name`, `repository`, requested `rev`, and selected `commit`.
- `packages`: unique repository/commit records, each with `repository`, `commit`,
  `digest`, `entry`, and `dependencies`.

A package's `digest` is the existing SHA-256 source-tree digest. Its `entry` is a
normalized relative `.k` public entry inside the immutable source snapshot.
`dependencies` uses the same four-field request/pin records as `roots`, preserving
private library aliases and transitive versions. Metadata names or SemVer labels
do not merge distinct repositories/commits. Multiple aliases of the same source
identity refer to one package record.

No cache directories, absolute public-entry paths or host compiler paths are
stored. Remote URL identities and relative entries can be reused by a fresh
checkout. Local Git requests still contain their declared absolute repository
identities; those projects require the same paths or an explicit manifest edit
on another machine. Locks do not make local absolute declarations portable.

Exact-commit requests must select that same commit. Other requests must use a
supported ref spelling; their selected commits remain exact IDs. The earlier
experimental format 1 is rejected rather than implicitly upgraded.

Parsing is bounded and validates the entire graph without opening repositories
or source files. Reject unknown formats/fields, invalid aliases/source identities,
duplicate aliases/packages, escaping or malformed public entries (including Windows drive paths), missing targets,
cycles, unreachable records and excess package/depth/byte limits. Every request
list has at most 128 entries. Defaults permit 64 unique packages, dependency depth
32 and 1 MiB of lock data; compiler module/source limits apply independently.

Serialization orders packages by source identity and request lists by alias.
An unchanged normal reinstall emits identical bytes. The reader does not require
that a hand-formatted lock use this order. Frozen/offline installation preserves
its original comments, bytes, inode and modification time.

## Installation modes

```sh
kalandra install --project my-app/kalandra.toml
kalandra install --project my-app/kalandra.toml --frozen
kalandra install --project my-app/kalandra.toml --offline
```

Normal installation creates a lock if absent. A present lock must parse correctly;
it is never silently discarded or repaired. Unchanged requests retain their
selected commits; unchanged source identities
retain locked digests, public entries and dependency edges, including when
the root manifest changes. Pins are matched within the declaring workspace or
library owner by alias, repository and requested revision. New requests may
import new snapshots, removed requests
leave the new lock, and unused snapshots remain cached. Manifest comment edits
also require explicit installation because the root digest covers exact bytes.

`--frozen` requires a matching existing lock. It may fetch/import missing locked
snapshots, checks their source digests/entries/edges and never rewrites the lock.
This enables a fresh checkout to reinstall from manifest plus committed lock.

`--offline` requires a matching existing lock and every verified snapshot already
cached. It invokes no Git and performs no network operations. It cannot create a
lock, populate a missing cache, repair corruption or change selections. Combining
`--frozen --offline` is permitted and has the same cache-only requirement.

Both modes still validate reachable source imports and use the installation
guard. Offline may create a missing guard file in an existing cache; it does not
publish source or lock data. Compilation/checking never install, fetch, repair
or rewrite a lock in any mode.

## Validation and activation

The installer captures one manifest and prior lock, validates the lock's graph,
then obtains a nonblocking project installation guard. It verifies/imports every
declared library, compares existing source pins against each snapshot, and checks
reachable source syntax/imports before activation. It does not type-check all
possible programs or expand GPU capabilities.

A successful normal install stages a complete lock in the private cache and
atomically renames it to `kalandra.lock` last. Final checks reject manifest edits,
changed/deleted prior locks and a lock appearing during installation. Normal
failure preserves an existing lock unless an external actor changed it; external
edits are never overwritten. Newly published immutable snapshots may remain
unused. A failed rename, including a cross-filesystem cache setup, does not
replace the old lock. No multi-file atomicity or fsync/power-loss durability is
promised. Process death can leave staging; cleanup/pruning remains later work.

A managed build reads one complete lock generation, checks the captured manifest
and root requests, verifies all selected snapshots and compares each public entry
and dependency list to its source manifest. It never trusts a lock path as an
arbitrary filesystem path. Old snapshots remain available when another installer
publishes a newer generation. Managed builds reject missing/stale/malformed locks
and altered snapshots before output publication. Projects without managed
requests continue using the explicit local-mapping path.

CPU executable/LLVM/MLIR and Metal source outputs cannot overwrite the reserved
lock namespace or its symlink/hard-link aliases, including dangling output links
and Metal output directories, even in projects with no managed
dependencies or cache yet. Existing source/cache protections also apply. Digests
check consistency and accidental edits; an owner who edits source and all its
metadata is outside this integrity model.

## Qualification and remaining work

Acceptance covers deterministic output, fresh-cache frozen reinstalls, cache-only
modes with origins/servers unavailable, exact preservation of formatted locks,
strict malformed graph/path cases and bounded parsing under ASan/UBSan. Tests
exercise valid-looking forged digests/entries/roots, stale manifests, concurrent
lock creation/editing, reserved outputs and empty graphs. Existing installer
coverage retains failed graph rollback, held guards, source corruption, alias
isolation, workers and project relocation.

Controlled remote fixtures reinstall a fresh checkout with `--frozen`, verify a
missing-cache offline attempt sends no requests, and check/build after shutting
down both HTTP/TLS servers. Installed CPU compiler and Metal source checks consume
the public lock; Metal source emission is not new GPU execution qualification.

See the [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-package-lock-2026-10-20.json).
The [native editor](native-language-server.md#installed-dependencies) now reads
and validates this lock without installing or rewriting packages.
[Installed Metal applications](native-metal-install.md#installed-numerical-libraries)
now qualify private numerical versions, CPU workers and GPU execution after
frozen cache restoration and project relocation. [Named root updates](native-package-update.md)
now deliberately refresh selected branch/tag requests while preserving unselected
and unchanged-owner private pins. Authenticated/SSH transport, broader platform
qualification and crash durability remain separate work.
