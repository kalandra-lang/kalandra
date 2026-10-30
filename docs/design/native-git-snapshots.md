# Exact-commit local Git snapshots

The first N3-B slice adds a snapshot API for pinned local Git repositories. It is
a snapshot API foundation now used by the [public installer](native-package-install.md).
The installer adds declarations and complete project activation; the snapshot API
itself does not implement lockfiles. Compiler/checker builds verify installed
snapshots offline and never invoke import/fetch operations. The generic
`installGitSnapshot` / `loadGitSnapshot` APIs now also support
[public HTTPS transport](native-remote-git.md); the local-only wrappers preserve
their original contract and cache identity.

## API and identity

`installLocalGitSnapshot(cache, request, limits)` imports a library into an
existing cache directory. `GitSnapshotRequest` contains a canonical absolute local
worktree or bare repository path and a full lowercase 40-digit SHA-1 **commit** ID.
Branches, abbreviated IDs, annotated tag objects, tree/blob IDs, linked worktrees,
alternates, partial clones and remote transports are not supported by this slice.
SHA-256-format Git repositories require later work.

The cache key is SHA-256 over length-delimited, versioned repository/commit
identity. One published directory contains:

```text
<cache>/<key>/
  snapshot.toml
  source/
    kalandra.toml
    ...tracked files...
```

The returned `NativeGitSnapshot` includes its resolved identity, directory,
source digest and parsed `NativePackage`. The library can construct a node for
the [scoped module loader](scoped-package-resolution.md). Two aliases of one node
share nominal declarations; different commits or repositories remain distinct.
Metadata package name/version never selects or merges resolved identities.

`loadLocalGitSnapshot` verifies and loads an existing snapshot without invoking
Git or accessing the original repository. Its request retains the original
normalized path string even when that repository is unavailable. Content hashes
use relative source paths, so changing the cache location does not change source
integrity. This does not make local repository identities portable across machines.

## Object import and materialization

The importer copies bounded loose objects or pack/index/reverse-index/bitmap files
into a private bare object database. It never reads the source repository's config,
refs, hooks, worktree files, replacement refs or attributes configuration. Only
object storage is copied; alternates and promisor markers are rejected. Linked
worktree indirection and symlinks in object storage are rejected.

A fixed `/usr/bin/git` executes read-only plumbing against that private database.
Each process receives an explicit environment with inherited Git settings removed,
system/global configuration disabled, no interactive credentials, no lazy fetch,
no protocols, no hooks, no filesystem monitor and no pager. Arguments are passed
directly to `execve`, without a shell. The repository's source programs are never
executed. System Git itself remains an external trusted tool.

The requested object must be a commit. `git fsck --strict` validates the copied
objects, and `ls-tree -r -z` plus `cat-file blob` materialize raw tracked contents.
No checkout, archive filters, export-ignore/export-subst transformations, clean or
smudge filters, submodule update or package build script runs. Regular executable
files become ordinary non-executable source files. Symlinks, gitlinks/submodules,
unsafe paths and file/directory aliases on the destination filesystem are rejected.
No repository metadata is included in the published source tree.

The manifest must describe a [native library](native-library-manifests.md). Its
metadata and entry paths are validated before publication; the importer does not
type-check the complete application or infer transitive dependency edges.

## Integrity and publication

A versioned SHA-256 digest covers sorted relative file/directory paths and file
contents, with length delimiters. Empty directories and added files affect the
digest; permissions, timestamps and ownership do not. The snapshot metadata has
an exact schema containing format, repository, commit and digest. Loading rejects
source edits, unexpected metadata entries, symlinks and unsupported filesystem
entries. Ordinary reinstallation verifies an existing snapshot and rejects
corruption rather than silently replacing or repairing it.

Import uses private sibling staging. Darwin's exclusive rename publishes only a
complete source/metadata directory and never replaces an existing path. Competing
imports of the same identity verify that the winner has the same digest. Normal
failures clean staging and preserve already published snapshots. Process death
can leave unused staging directories; there is no garbage collector yet. Files
are closed before rename, but fsync/power-loss durability is not promised.

This API provides snapshot-level publication. [Project installation](native-package-install.md)
now adds complete-graph validation, a guard, manifest-change detection and
[public lock activation](native-package-lock.md). [Branch/tag installation](native-git-refs.md) now selects exact commits for the snapshot API.
The cache is writable by its owner: digest verification detects accidental edits,
not coordinated modification of both source and integrity metadata. It is not an
operating-system sandbox or a defense against hostile concurrent filesystem edits.
The project installer/loader now adds independently locked source digests and
managed output protection.

## Bounds and qualification

Defaults bound copied object-store entries to 8192 and copied bytes to 128 MiB;
materialized source files/directories to 4096 and contents to 64 MiB; Git listings
to 1 MiB; and each Git process to 30 seconds. Output capture is bounded through a
pipe, and rejected or timed-out children are killed/reaped. Callers can lower
these limits. These are per-snapshot/per-command bounds, not an overall graph
installation deadline or a hard child-process memory limit. The [remote transport](native-remote-git.md) adds live object-store monitoring
and inherited per-file limits; it does not promise a hard memory sandbox.

Acceptance covers loose and packed bare stores, exact commit identity, raw blob
semantics, unsafe config/environment isolation, invalid revisions/trees/manifests,
corrupt objects, alternates, size/count/capture limits, nonzero-timeout validation,
source/metadata edits, extra files/directories, symlink rejection, no replacement,
staging cleanup, competing processes and loading after the original repository is
removed. The original local snapshot checks did not inject actual timeout expiry
or hard resource exhaustion; remote transport fixtures separately test stalled
servers and download/source budgets.
Path-alias acceptance is qualified on this macOS filesystem.

A manifest-backed graph uses geometry v2 directly and geometry v1 through a
telemetry worker, with all three libraries imported as snapshots. It produces
`[12, 11]` in debug, optimized, ASan/UBSan and TSan programs with one/four workers.
The snapshot importer and graph checking also run under ASan/UBSan. See the
[qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-git-snapshots-2026-10-20.json).

Exact dependency declarations, complete managed graph activation and
CLI/checker/Metal source consumption/output protection are now implemented.
[Remote transport](native-remote-git.md) and [exact-commit public locks](native-package-lock.md)
are also implemented. [Branch/tag resolution](native-git-refs.md) is implemented. Broader installed tools and editor onboarding remain in the [package workflow](native-packages.md).
