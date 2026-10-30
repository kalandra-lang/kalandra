# Installing native editor tools

The `NativeEditorTools` CMake component installs four native commands independently of the checkout's build directory: `kalandra-check`, `kalandra-fmt`, `kalandra-parse` and `kalandra-lsp`. This is the first native tool installation slice for macOS Apple Silicon, not a complete compiler distribution.

## Direct installation

Build the component's tools, then select a prefix you control:

```sh
cmake --build build/native --target native-editor-tools
cmake --install build/native --prefix "$HOME/.local" --component NativeEditorTools
```

Add that prefix's `bin` directory to PATH. The [VS Code extension](native-vscode.md) can then use its default `kalandra-lsp` executable name, or you can set **Server Path** to the installed binary. No source files or project manifests are installed implicitly.

Installation honors CMake's `DESTDIR` staging mechanism and `GNUInstallDirs` destinations. The component name matters: this installs only the four tools and their documentation/license/build metadata. It does not install `kalandra`, generated-program runtime archives, GPU tooling, development headers or the VSIX. The [CPU compiler component and bundle](native-cpu-install.md) now install the compiler/runtime separately. The [Metal compiler bundle](native-metal-install.md) adds GPU resources separately; the VSIX is packaged independently.

## Archive

```sh
cmake --build build/native --target native-editor-tools-package
```

This writes:

```text
build/native/native-editor-tools/
  kalandra-native-editor-tools-macos-arm64.tar.gz
  kalandra-native-editor-tools-macos-arm64.tar.gz.sha256
```

The archive contains one `kalandra-native-editor-tools` directory with `bin`, licenses, a standalone README, `share/kalandra-native-tools/toolchain.json` and a `SHA256SUMS` file covering the installed files. Extract it into a directory you control and add its `bin` directory to PATH. The archive layout requires the default relative `bin` and `share` destinations; direct installs can use other configured install directories.

Packaging stages and strips copies of the tools without modifying the build binaries. It ignores an inherited `DESTDIR`, preserves the selected CMake configuration in metadata, validates the requested configuration, and publishes the archive only after successful staging/archiving. An unsuccessful packaging attempt preserves an existing archive. Archive and file checksums detect accidental changes; they are not signatures or publisher authentication. Archive timestamps, ownership, ordering and permissions are now normalized (`SOURCE_DATE_EPOCH`, default zero), with independent-staging repack checks. Independent source-build reproducibility, signing and notarization remain release work.

## Runtime dependency and scope

An additional [standalone editor archive](native-distribution-qualification.md)
now bundles LLVM and its non-system dependencies using package-relative loader
paths. Build `native-editor-tools-standalone-package` for that artifact. The
external-dependency description below applies to `native-editor-tools-package`
and direct component installs. Neither artifact has a publisher signature or
notarization; the standalone package records its measured minimum macOS version.

The checker and language server still link to **LLVM 23.1.3** from the pinned toolchain. The LLVM runtime is external: the archive neither bundles it nor changes its loader paths. The dependency must remain available at the paths used when linking; moving the tools does not relocate LLVM. The installed metadata identifies the required LLVM version and the actual build configuration. A stripped Debug build remains a Debug build.

No Kalandra checkout, Bun, Node or Python is needed to run these installed commands. The formatter/parser/checker retain their source contracts, and the language server does not run application code. CMake and the compiler toolchain are needed to build/install/package from source; Python is also needed to create normalized archives; platform inspection tools are used by acceptance tests.

This package has been tested on the development macOS ARM64 machine with its pinned LLVM runtime. It does not qualify a clean-machine, self-contained release, another operating system/architecture, an application compiler/runtime bundle or a Homebrew/Marketplace release. No registry upload or machine-wide installation is performed by the package build target.

## Acceptance

```sh
cmake --build build/native --target native-editor-tools-package
ctest --test-dir build/native -R '^native-editor-tools-install$' --output-on-failure
```

The acceptance test installs into paths containing spaces and a separate `DESTDIR`, checks the exact tool set, packages and extracts a relocated archive, removes its original packaging prefix, verifies all file/archive checksums and metadata, and audits ARM64 binaries and their external/system loader dependencies. It checks an explicit package-mapped project, syntax and formatting from a working directory outside the checkout, then runs the complete independent stdio acceptance suite against the relocated language server. It also verifies packaging failure preserves an earlier archive and stripping leaves the original build binaries unchanged.

A separate [real VS Code host test](native-vscode.md#qualification), supplied the relocated server through `KALANDRA_LSP`, verifies that installed tools work with the installed extension's editor features. The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-editor-tools-install-2026-10-20.json) records these actual runs and archive hashes.
