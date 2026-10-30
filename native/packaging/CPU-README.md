# Kalandra native CPU compiler

This package adds `bin/kalandra` and its private CPU runtime archives to the
native editor tools. Add `bin` to PATH, then compile a `.k` file or explicit
native project manifest:

```sh
kalandra new my-app
kalandra --project my-app/kalandra.toml -o program
./program
```

For an existing project, compile with `kalandra --project path/to/kalandra.toml -o program`,
or pass a `.k` source file directly.

`new` creates an offline two-module parallel-map starter. Its parent directory
must exist, and its destination must not exist; existing files/directories and
symlinks are never replaced or merged. No packages are fetched or hooks run.

The compiler locates `lib/kalandra/native` relative to its actual executable,
including when invoked through a symlink or PATH. Keep this directory with the
compiler when moving the package. Missing resources fail without falling back
to development build files. Normal and ASan/UBSan (`--sanitize`) runtime archives are included.
macOS and Linux also include separate TSan (`--thread-sanitize`) archives;
Windows rejects that unsupported mode before changing an output. `-O0` and `-O2` select the
existing native compilation modes.

Standalone archives include private LLVM/MLIR and Clang 23.1.3 dependencies,
compiler resources, licenses and an inventory. External-toolchain archives
retain their configured LLVM/Clang dependency paths. Check
`share/kalandra-native-tools/toolchain.json` for the package's platform and
`llvmRuntime` value. Ordinary generated programs link the private CPU runtime
statically and execute without the compiler package or LLVM.

The supported macOS minimum is 26.0. Compilation requires Apple's SDK/linker on macOS, the system C/C++ development
files on Linux (Ubuntu 24.04 / glibc 2.39 baseline), or Microsoft Visual Studio
C++ Build Tools and the Windows SDK on Windows 11 x86-64. On Windows, run the
compiler in a Visual Studio developer shell and name executables `program.exe`.
Windows ASan applications also need the package's
`lib/kalandra/toolchain/clang/lib/windows` directory on PATH while running.
Sanitizers instrument host execution; they are separate from ordinary builds.

The CPU variant is an alternative to the mixed CPU/Metal macOS package. Its
archives are compiler-private resources, not a public C/C++ embedding ABI.
Signed and notarized macOS DMGs are separate deliveries from reproducible unsigned
archives. Read the package inventory, metadata and licenses before redistribution.

Local or public HTTPS Git dependencies can be installed explicitly with `kalandra install --project path/to/kalandra.toml`. Installation requires system Git; subsequent project builds verify the activated cache offline. Remote installs verify TLS and require public repositories. Commit IDs and supported branch/tag names are accepted; names are locked to exact commits and unchanged requests keep their pins. Commit the generated `kalandra.lock`. Use `--frozen` to populate a cache from a matching lock without rewriting it, or `--offline` to require verified cached sources without invoking Git. Use `kalandra update name [name ...] --project path/to/kalandra.toml` to refresh selected root aliases from a matching lock; unrelated roots and unchanged-owner private pins stay fixed. Check and rebuild after updating. SSH and authenticated repositories are not supported yet.
