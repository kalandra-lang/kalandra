# Installing the native CPU compiler

The CPU distribution combines the four [native editor tools](native-tools-install.md)
with `kalandra` and its compiler-private runtime archives. It targets macOS ARM64;
[Metal distribution](native-metal-install.md) uses a separate compiler/resource bundle.

```sh
cmake --build build/native --target native-cpu-package
```

Extract `build/native/native-cpu/kalandra-native-cpu-macos-arm64.tar.gz` and add
its `bin` directory to PATH. Keep `lib/kalandra/native` beside `bin`: the compiler
resolves resources relative to its actual executable, including symlink invocation.
It never falls back to archives from the development checkout.

```sh
kalandra main.k -o program
./program
kalandra --project path/to/kalandra.toml -O2 -o program
```

The package includes normal, ASan/UBSan and TSan runtime variants. `--sanitize`
and `--thread-sanitize` remain separate builds. The 30 static archives are private
implementation resources, not an installed C/C++ interoperability interface.
Generated ordinary programs have no LLVM/MLIR dependency; sanitized programs also
need the platform sanitizer runtime.

LLVM/MLIR and Clang 23.1.3 remain external at the configured/linked paths, along
with the Apple SDK. CI packages use Homebrew `llvm@23`. Install it with
`brew install llvm@23` and install Apple command-line tools before compiling.
Moving the bundle does not relocate LLVM or Clang. This is not a self-contained,
signed or notarized toolchain, and other platforms are not qualified.

The additional [standalone CPU package](native-distribution-qualification.md),
built with `native-cpu-standalone-package`, includes LLVM/MLIR, their non-system
dependencies, private Clang and compiler resources. It removes the Homebrew
runtime requirement while retaining Apple's SDK/system-linker prerequisite.
It has dependency-isolation acceptance and measured minimum-OS metadata; it is
ad-hoc signed, without Developer ID or notarization.

For a custom prefix, build `native-cpu-toolchain`, then install **both** CMake
components `NativeEditorTools` and `NativeCPUCompiler` with the same `--prefix`.
Direct installation honors GNUInstallDirs and DESTDIR. The packaged archive
requires the default `bin`, `lib` and `share` layout. Check the archive sidecar
and extracted `SHA256SUMS` to detect accidental changes.

`ctest --test-dir build/native -R '^native-cpu-install$' --output-on-failure`
packages, relocates and removes the original stage, compiles from an unrelated
working directory, checks result-entry and sanitizer variants, worker dispatch,
project imports, generated-program dependencies and missing-resource failures.
The independent editor-tools installation test covers the complete relocated LSP
protocol suite. [Tag releases](native-releases.md) run both gates before upload.
