# Native release automation

The `Native release` GitHub Actions workflow builds macOS 26+ Apple Silicon
mixed CPU/Metal, CPU-only and editor packages and a separately versioned VS Code extension from a version tag.
It does not publish npm packages, the website or the extension Marketplace.

- `v1.0.0` creates a normal GitHub release and marks it latest.
- `v1.0.0-alpha.1`, `v1.0.0-beta.1`, `v1.0.0-rc.1`, and other valid SemVer
  prerelease identifiers create GitHub **prereleases**, never latest stable.
- Build metadata alone, such as `v1.0.0+build.1`, is stable. Invalid SemVer tags
  fail validation, including numeric identifiers with leading zeros.

The root `VERSION` file is authoritative for the native toolchain. It currently
contains `0.99.1`; CMake uses its numeric core as `PROJECT_VERSION` and embeds the
full SemVer string in CLI tools, LSP `serverInfo` and installed `toolchain.json`
metadata. LLVM and the VS Code extension retain their independent versions.

Before a release, update `VERSION` and author `docs/releases/<version>.md` with a
heading for that exact version. Review and test the commit, then tag that exact
commit `v<version>` and push the tag. The tagged workflows reject any mismatch
between the tag and `VERSION`, or missing/mismatched release notes, before their
expensive builds. The publisher uses that authored Markdown rather than a fixed
packaging blurb. The workflow checks out the tagged commit, checks the version parser,
installs the pinned native dependencies, builds Release packages, runs installation
and relocation acceptance, and builds/tests the editor package. It uploads the CPU
bundle, the smaller editor-tools-only bundle, the VSIX and their checksum sidecars.
Additional standalone mixed Metal, CPU and editor archives bundle private LLVM/Clang dependencies
and licenses. The build job independently compiles the source twice and requires byte-identical
standalone archives, as well as independent staging repacks. A separate `qualify-standalone` job downloads
artifacts without installing LLVM or building native code; publication depends on
its isolation checks. N5 qualification is complete for macOS 26+ ARM64 CPU/Metal,
Linux x86-64/ARM64 and Windows x86-64/ARM64. CPU cross-compilation is qualified in all 20 directed pairs between these five platforms. See the [N5 checklist](native-distribution-qualification.md).

The `notarize` job then signs staging copies of all three standalone bundles, repeats
their runtime acceptance with hardened signatures, submits signed DMGs to Apple,
staples tickets and assesses Gatekeeper acceptance. Tagged publication requires
this job to pass and validates all 31 assets, checksum sidecars and accepted
receipts before creating a release. Configure the publisher credentials using
the [CI signing instructions](native-ci-signing.md). A native branch rehearsal
requires repository variable `KALANDRA_NOTARIZE=true` and publishes no release.

The VSIX retains its independent version from `native/editor/vscode/package.json`.

Only the publishing job receives repository write permission. It creates a draft
with assets, then publishes after successful upload. A failed upload leaves a draft;
inspect it before retrying. Reruns do not overwrite an existing release or its
assets. Delete a failed draft explicitly if a retry is appropriate. Cancellation
is disabled for concurrent publication runs of the same tag. Branch rehearsals
are grouped by commit so an obsolete runner cannot block qualification of a new
commit; branch runs never publish releases.

Manual workflow dispatch runs the same build/qualification and uploads workflow
artifacts, but does not publish a release. GitHub requires the workflow to exist on the
default branch before manual dispatch is available; then select the native branch
for a rehearsal before the first native release tag. It creates no tag. Existing historical tags are not replayed.

The macOS Metal DMG is the primary mixed CPU/GPU distribution. CPU-only and
editor-only packages are alternatives. Standalone bundles contain their private
open-source toolchain; Apple SDK/linker and the pinned Apple Metal compiler remain
platform prerequisites. Their signatures and notarization are produced only on
GitHub Actions. The supported macOS minimum is 26.0.

Linux and Windows qualification workflows produce standalone CPU archives for
x86-64 and ARM64, and execute downloaded packages on fresh runners. Release assembly
collects successful same-commit native/selected-tag archives and projects runtime-only
datasets from all five CPU platforms into each host's standalone compiler package.
Private linkers execute on the host. Target runtime data excludes foreign compiler
executables, dynamic libraries and system SDKs. Bundling precedes macOS signing
and notarization, so receipts identify the final distribution archives.

The cross-platform workflow independently assembles every host distribution twice,
then builds all 20 directed source/destination pairs from the combined host packages
and executes the results on native destination runners. Publication requires this
workflow to succeed for the exact release tag as well as the native platform gates.
Missing, failed or different-commit results block release creation.

Linux targets Ubuntu 24.04 / glibc 2.39 x86-64 and ARM64; Raspberry Pi OS Trixie
userspace is also qualified for modern 64-bit Pi systems. Windows targets Windows 11
x86-64 and ARM64 with Microsoft C++ Build Tools and the Windows SDK. Windows TSan,
Windows ARM64 sanitizers and all cross-toolchain sanitizer combinations are explicitly
unsupported. The Windows workflow provisions its pinned LLVM/MLIR SDK automatically
and caches it; a cache miss rebuilds matching MLIR from verified official source.
`Native platform preflight` can warm that cache. Windows SDK transfer is restricted
to temporary job-local plaintext and recipient-specific authenticated ciphertext;
SDK libraries and private keys are excluded from public distribution artifacts.

No tag or release is created simply by committing these workflows.
