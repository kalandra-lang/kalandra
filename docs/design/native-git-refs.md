# Branch and tag dependencies

Native dependency `rev` requests now accept branch/tag names as well as exact
lowercase 40-digit SHA-1 commits. Installation resolves a new name to a verified
commit snapshot. The [public lock](native-package-lock.md) records both the
requested revision and its selected commit. Compilation and checking consume
that pin without resolving names.

```toml
[dependencies]
geometry = { git = "https://example.com/team/geometry.git", rev = "main" }
telemetry = { git = "https://example.com/team/telemetry.git", rev = "v1.0.0" }
```

These repository URLs are illustrative. Metadata SemVer versions do not select
Git refs, and there is no range solver or implicit latest-version lookup.

## Names and ambiguity

An unqualified request such as `main`, `v1.0.0` or `feature/next` checks exactly
`refs/heads/<name>` and `refs/tags/<name>`. If both exist, fail even if they point
to the same commit. Select `refs/heads/main` or `refs/tags/main` explicitly when
both names are present. Other `refs/` namespaces are not supported.

Names use ASCII letters, digits, `_`, `-`, `.`, and `/` separators, with a maximum
request length of 1024 bytes and components of at most 255 bytes. Empty/dot-leading
components, trailing dots/slashes, `..`, `.lock` suffixes, revision expressions,
patterns, whitespace and shell syntax are rejected. `HEAD` is not an implicit
request. Hex-only names require an explicit heads/tags prefix so abbreviated or
uppercase object IDs are not mistaken for revisions. Full lowercase commit IDs
always mean exact commits; qualify an identically named ref explicitly.

Lightweight and annotated tags are supported, including nested annotated tags.
The selected source must ultimately be a commit. Tree/blob tags fail before a
snapshot or graph is activated. Tag signatures are not verified. Remote annotated
tags use the server's advertised peeled target, followed by verified exact-commit
snapshot import; no signed-tag authentication is implied.

## Stable pins belong to their declaring owner

An existing request retains its commit when its owner, alias, repository and
requested revision are unchanged. Moving a branch/tag does not refresh that pin
on ordinary reinstall. Manifest comment edits and unrelated dependency additions
do not refresh existing selections either.

A workspace owns its root aliases. Each immutable library source identity owns
its private aliases. Thus an old library can retain `geometry = main` at one
commit while a new library commit selects the current `main`. Newly added aliases
are new requests; an alias rename or an explicit revision change can select a
newer commit without altering another alias's existing pin. Multiple newly
resolved requests for one repository/revision use a consistent selection within
that installation. Multiple aliases resolving to one repository/commit share
one nominal package identity.

The lock records each edge independently and one source record per immutable
repository/commit. It rejects inconsistent graphs, and source verification checks
that locked request spellings still agree with each library's source manifest.
Cycles introduced by mutable names fail before lock activation.

Normal install resolves only genuinely new/changed requests. Frozen install
requires a matching lock and imports only selected exact commits when necessary;
it does not re-resolve requested names. Offline install requires verified cached
snapshots and invokes no Git. Builds never install or resolve refs. Removed refs
or unavailable origins therefore do not affect a complete cached graph.

A server may refuse a formerly selected commit once it is unreachable or has
been deleted. A missing-cache reinstall then fails; it never substitutes the
branch's new tip. [Named root updates](native-package-update.md) now deliberately refresh selected requests.
An explicit new commit in the manifest is already a deliberate request change.

## Local and remote lookup

For a canonical local repository, the importer copies bounded object storage
into its own private database and reads only the requested loose refs and bounded
`packed-refs` data. Loose refs take precedence over packed values. Ref paths must
use real directories/regular files; symbolic refs and symlink indirection are
unsupported. Tag objects are peeled in the private database, not by executing
Git in the source repository. Source configuration, hooks and programs remain
unused. Packed-ref input is capped by the listing-byte and object-entry budgets.

Remote lookup uses isolated system Git `ls-remote` with exact heads/tags patterns
and peeled tag names. TLS, credential, protocol, redirect, timeout and capture
policies are shared with [HTTP(S) snapshot fetching](native-remote-git.md). Returned
records must contain exact IDs and only the requested ref names; unexpected or
duplicate records and peeled targets without their tag are rejected. A candidate
is not a package until exact snapshot import validates its commit/source/library.

Lookup staging is private and removed on ordinary success/failure. Existing
snapshot/graph budgets, integrity checks, installation guards and final
manifest/lock change detection still apply. Ref resolution and subsequent source
fetch are separate operations: a concurrent ref move or object disappearance may
cause a clean failed install. No cross-server atomic view is promised.

## Qualification

Local acceptance covers loose and packed branches/tags, nested annotated tags,
loose overrides, ambiguity, explicit namespaces, noncommit targets, unsafe inputs,
symlink/symbolic refs, bounded listings and ignored source Git configuration.
It moves both a root branch and a private tag, adds unrelated/new owners and
aliases, and checks that retained versus new selections behave independently.
Real ref-dependent graph cycles and ambiguous new requests preserve the old lock.

Controlled HTTP/TLS servers exercise actual ref advertisements and commit fetches,
annotated tags, ambiguity, missing/noncommit refs, TLS verification, bounded
captures and stalled lookup expiry. Cached reinstall makes no requests after a
ref move. A fresh frozen checkout fetches the old locked commit despite the new
branch tip. After servers stop, it checks/builds and executes from the lock.

Importer/parser and owner-local graph pinning paths run under ASan/UBSan. Generated CPU workers run in debug,
optimized, ASan/UBSan and TSan modes with one/four workers. Existing CPU/Metal
source and installed compiler acceptance remain covered; this adds no actual GPU
execution claim. A separate public GitHub branch lookup/extraction probe reaches
the expected missing-native-manifest rejection in `octocat/Hello-World`; it is
a transport check rather than a hosted native-library end-to-end result. See the [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-git-refs-2026-10-20.json).
Authenticated/SSH transports, arbitrary revision expressions, signed tags,
symbolic local refs and broader platform qualification remain outside this slice.
[Named root updates](native-package-update.md) and
[package-aware editor integration](native-language-server.md#installed-dependencies)
now build on these owner-local pins.
