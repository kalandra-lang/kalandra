# Exact-commit remote Git dependencies

The native installer now accepts public HTTPS repositories alongside local Git
paths. Source imports, package-local scopes, nominal identity, graph activation
and offline builds follow the existing [installation contract](native-package-install.md).
Snapshot transport fetches exact commits.
[Branch/tag lookup](native-git-refs.md) selects commits for new requests;
[public locks and frozen/offline modes](native-package-lock.md) keep reinstalls pinned.

```toml
[dependencies]
geometry = { git = "https://example.com/team/geometry.git", rev = "0123456789abcdef0123456789abcdef01234567" }
```

The URL and full lowercase 40-digit SHA-1 commit above are illustrative. Install
explicitly with `kalandra install --project path/to/kalandra.toml`. The selected
server must support smart HTTP Git transport and permit fetching that exact
commit. Branch/tag declarations use the [revision rules](native-git-refs.md) and
resolve to exact commits. Abbreviated IDs and SHA-256-format repositories are not
supported. The current graph requires native library manifests at repository roots.

## URL and trust policy

Public `https://` URLs use system certificate verification. Credentials in URLs,
query strings, fragments, percent escapes, uppercase hostnames, IPv6 literals,
empty/dot path components and trailing slashes are rejected. DNS labels are ASCII;
repository path components support ASCII letters, digits, `-`, `_`, `.`, and `~`.
Ports must be canonical nonzero decimal values; omit the default HTTPS port 443.
These are explicit supported spellings, not a general-purpose URL normalizer.

`http://127.0.0.1:<port>/path` is also accepted for local development and deterministic
transport tests. Other plaintext HTTP hosts, `localhost` spellings, SSH/scp,
`git://`, `file://` and external helper transports are rejected. Redirects are
disabled, including HTTPS redirects; use the repository's direct URL.

Authentication, credential helpers and interactive prompts are disabled. Inherited
Git configuration, proxies, askpass programs and TLS environment overrides are
not passed to Git. There is no public CLI option to bypass TLS verification or
read ambient credentials. The embedding API's optional
`GitSnapshotLimits.certificateAuthorityFile` accepts an explicit canonical regular
CA file for private trust environments and controlled TLS tests; verification
remains enabled. Default CLI installs use system trust only.

Remote libraries may declare other supported remote dependencies. They cannot
declare absolute local Git dependencies and cause installation to inspect an
arbitrary local repository. Applications and explicitly selected local libraries
can still use a mixture of local and remote dependencies. Private `[packages]`
entries in a remote library remain confined to its verified source snapshot.

## Fetch and publication

The installer creates its own private bare object database and invokes fixed
system Git with explicit arguments and an isolated environment. Only the selected
HTTP(S) protocol is allowed during fetch; later object-reading commands allow no
transport. Fetch requests one exact commit with depth one, without tags,
submodules, worktree checkout, fetch-head publication, automatic maintenance or
commit-graph writes. Git validates received objects. Raw tree/blob extraction
then applies the same library checks, source limits, digest and exclusive snapshot
publication as [local snapshots](native-git-snapshots.md).

The download has the existing per-command time limit (30 seconds by default).
The parent samples private object-file count and aggregate bytes while the child
runs, checks them again after completion, and kills the child process group on
failure/timeout. An inherited file-size limit bounds each fetched file; default
aggregate object budget is 128 MiB with 8192 entries. Aggregate live sampling can
observe transient overshoot between polls; it is not a hard disk quota. These
limits do not promise a hard CPU or memory sandbox for system Git or its helpers.
Source/listing and whole-graph budgets still apply after fetch. No partially
downloaded database is published, and ordinary failures remove staging.

Frozen installs can populate a fresh cache from a matching committed lock.
Offline installs require every snapshot and never fetch.

Remote keys and nominal identities include the exact validated URL and commit,
with a distinct versioned remote domain. Local cache keys remain unchanged.
Different repository URLs are distinct identities even if they host identical
objects; no redirect or alias discovery merges them. Published snapshots retain
only source and metadata, not fetched Git databases, credentials or certificates.

Existing snapshots are verified before any fetch. Compiler/checker/Metal source
builds never contact servers; neither does reinstalling a completely cached exact
graph. Missing objects, server errors, TLS failures and unsupported requests fail
clearly without replacing an existing active graph. The generic transport failure
message deliberately does not echo server diagnostics or potential secrets.
Content digests detect cache edits; they do not authenticate repository authors.

## Qualification

Controlled smart HTTP and HTTPS servers exercise actual system Git transport,
including trusted/untrusted certificates, hostname mismatch, redirects, authentication challenges,
malformed packets, missing commits, a stalled server, object byte/entry limits,
source/listing limits, symlink rejection and remote-to-local dependency rejection.
Tests inject hostile ambient Git/proxy/TLS settings and verify that they are not
used. Failed graph extension preserves the previous public lock and temporary snapshots
are cleaned after normal failures.

After shutting down both servers, acceptance reloads and reinstalls snapshots,
checks the transitive application and executes CPU workers in debug, optimized,
ASan/UBSan and TSan modes with one/four workers. The importer/resolver also runs
under ASan/UBSan. The debug fixture installs and relocates the CPU compiler, uses
its public installer, and builds an offline worker application. Remote numerical
imports are checked through offline Metal source emission; this is not new GPU
execution qualification.

See the [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-remote-git-2026-10-20.json).
A separate read-only public GitHub probe fetched and extracted an exact commit
from `octocat/Hello-World` using system TLS, then correctly rejected its missing
native library manifest without publishing a snapshot. This is a transport probe,
not a hosted native-library end-to-end result.

Controlled servers and that probe do not establish compatibility with every hosted Git service,
proxy, certificate setup, authentication method or Git version. Broader public-host qualification,
authenticated/SSH transport, broader ref-host qualification, crash recovery
and broader installed Metal execution remain separate qualification work.

## Named updates over HTTP(S)

The [named update command](native-package-update.md) uses the same isolated
transport policy as installation and ref lookup. A selected branch or tag is
resolved again; an unselected root and an unchanged library's private branch pin
remain fixed, even when they use the same repository and requested ref. A cached
selected snapshot does not let a mutable update skip current remote resolution.

TLS failures, authentication challenges, redirects, malformed advertisements,
unavailable servers and invalid newly downloaded source graphs leave the prior
active lock unchanged. Ordinary checking/building and frozen/offline installation
still consume that prior selected graph without contacting the server. Update
does not add authentication, inherited credential helpers, SSH, redirects or a
CLI TLS bypass. An explicitly supplied CA remains an installer API facility.

[Remote-update qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-remote-update-2026-10-21.json)
uses controlled loopback HTTP and HTTPS servers, a generated fixture certificate
and no external service or user credentials. It checks selected branch/tag moves,
unselected aliases/private branch pins, actual trusted/untrusted TLS behavior,
failed activation and relocated installed CPU compiler updates. After both
servers stop, old frozen and newly updated graphs compile and run their CPU
workers with the expected distinct versions. These checks do not qualify public
hosting, authenticated transport or another platform.
