# Exact dependency installation

The native tools support `[dependencies]`, explicit `kalandra install --project`,
and a locked graph for compiler/checker/Metal source builds. Dependencies use
local or public HTTPS Git repositories with exact commits or supported branch/tag
requests, always locked to exact commits. Numeric-loopback
HTTP is supported for local development. See [remote transport](native-remote-git.md)
for URL/TLS/download rules and [package locks](native-package-lock.md) for the file
format, activation and frozen/offline contracts.

## Declare and install

An application declares aliases independently of library metadata names:

```toml
format = 1

[project]
entry = "src/main.k"

[dependencies]
geometry = { git = "https://example.com/team/geometry.git", rev = "0123456789abcdef0123456789abcdef01234567" }
```

Replace the illustrative URL and ID with a repository and full lowercase 40-digit
commit ID. Local repositories use canonical absolute paths. A managed library
uses the same `[dependencies]` format. Abbreviated IDs, revision expressions, unsupported
remote protocols, relative local repository paths, unknown request fields and
alias conflicts with `[packages]` are rejected. Declaration parsing does not open
Git repositories or install anything.

```sh
kalandra install --project my-app/kalandra.toml
kalandra-check --project my-app/kalandra.toml
kalandra --project my-app/kalandra.toml -o my-app/program
```

Installation requires an application `[project]` entry. Libraries are installed
transitively through their public `[package]` entries, and may supply example
applications using both tables. Each library resolves imports through its own
private aliases, local mappings and dependency versions. Different source
identities retain distinct nominal types; repeated aliases of one identity share
a node. Workspace/CLI `--package` mappings cannot override managed aliases or leak
into library scopes.

## Lock and cache

Generated state is kept beside the manifest:

```text
kalandra.lock
.kalandra/
  install.guard
  snapshots/<identity>/snapshot.toml
  snapshots/<identity>/source/...
```

Commit `kalandra.lock`. Keep `.kalandra/` out of version control. The public lock
records root requests, exact identities, private dependency edges, relative
public entries and source digests. It is the sole graph activation point; builds
capture one complete lock generation and verify all selected snapshots offline.
There is no private activation index to synchronize with it.

```sh
kalandra install --project my-app/kalandra.toml --frozen
kalandra install --project my-app/kalandra.toml --offline
```

Frozen installation requires a matching lock, may import missing locked snapshots,
and preserves the lock exactly. Offline installation also requires a complete
verified cache and never invokes Git. Neither mode bootstraps a missing lock or
rewrites source/manifest/lock files. Normal installation may create or update the
lock after validating the complete graph. Existing source pins remain checked,
including when the manifest changes. [Branch/tag requests](native-git-refs.md) retain existing owner-local pins when
refs move; new/changed requests resolve explicitly during normal installation.
Version solving remains separate work.

Changing manifest bytes, even comments, requires explicit normal reinstallation.
Source edits alone do not. Compilation/checking never fetches, installs, repairs
cache files or activates a graph. Missing/stale/malformed locks, changed source
pins and corrupt snapshots fail before output publication. Relocating a project
with its cache works; remote identities can also reinstall a fresh cache using the
committed lock. Absolute local repository identities remain machine-specific.

## Boundaries and qualification

The installer holds a project guard and atomically publishes the complete lock
last, after rechecking captured manifest/lock inputs. Failed resolution leaves an
old lock intact; unused snapshots may remain. No package programs/hooks or source
checkout filters execute. New cache directories are private. Process interruption
can leave staging; no fsync/power-loss durability, hostile-filesystem sandbox or
automatic pruning is promised. See the detailed [lock activation contract](native-package-lock.md).

Project outputs reserve the lock/cache namespaces and protect persistent files
against direct, symlink and hard-link replacement. Old/unused snapshots remain
protected. Private cache staging is skipped during traversal because a concurrent
installer can remove it; paths explicitly inside the cache remain forbidden.
Default whole-graph budgets are 64 packages, depth 32, 32768 source entries,
128 MiB of source and 1 MiB of lock data. Snapshot and compiler limits also apply.

The [native language server](native-language-server.md#installed-dependencies)
reads the installed scoped graph for diagnostics, completion and navigation.
It does not install dependencies; cached library sources remain read-only.
Formatting workspace source remains independent of dependency installation.

Acceptance covers private scopes, transitive versions, workers, JSON/stdin
checking, rollback, guards, manifest/lock edits, stale/corrupt state, output aliases,
offline builds and relocation. Generated workers run in debug, optimized,
ASan/UBSan and TSan modes with one/four workers. Remote TLS/HTTP fixtures and
relocated CPU compiler checks cover installation and offline use. Metal checks
cover numerical imports and source emission/protection without new GPU execution.

Qualification records: [initial installation](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-package-install-2026-10-20.json),
[remote transport](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-remote-git-2026-10-20.json),
and [public locks](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-package-lock-2026-10-20.json).
[Installed Metal acceptance](native-metal-install.md#installed-numerical-libraries)
now adds private-version numerical calls through CPU workers and actual GPU
execution, with frozen/offline cache restoration and generated-app independence.
[Named root updates](native-package-update.md) now refresh selected aliases from a matching lock.
Remaining work includes authenticated/SSH transport,
broader host/platform qualification and crash recovery.
