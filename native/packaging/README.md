# Kalandra native editor tools

This package contains the native source checker, formatter, parser and language
server for the platform recorded in `share/kalandra-native-tools/toolchain.json`:

- `bin/kalandra-check`: check source or an explicit native project manifest.
- `bin/kalandra-fmt`: print formatted source, or use `--check` without writes.
- `bin/kalandra-parse`: validate native syntax.
- `bin/kalandra-lsp`: provide editor services over stdin/stdout.

Extract the package to a directory you control and add its `bin` directory to
PATH. The VS Code extension can use `kalandra-lsp` on PATH, or its absolute path
in the **Kalandra Native: Server Path** user setting. Native source files use `.k`.

```sh
kalandra-check --project path/to/kalandra.toml
kalandra-check path/to/main.k
kalandra-fmt --check path/to/main.k
kalandra-parse path/to/main.k
```

Standalone distributions include their private LLVM runtime dependencies and
licenses. External-toolchain variants retain the configured LLVM 23.1.3 loader
paths. Read `share/kalandra-native-tools/toolchain.json` for the platform,
configuration and `llvmRuntime` contract. Keep the complete prefix together.

`SHA256SUMS` covers installed files, and archives have `.sha256` sidecars.
Signed, notarized and stapled macOS DMGs are separate from unsigned archives.
The editor-only variant has no compiler or generated-program runtime; CPU and
mixed Metal variants include these same editor tools with their compiler.
The VS Code extension is distributed separately.

No source checkout, Bun, Node or Python is needed to run these native tools.
They preserve the compiler's documented language, synchronous analysis and
resource limits. The language server does not execute application code.

Project: https://github.com/kalandra-lang/kalandra
Licenses and attribution: `share/licenses/kalandra-native-tools`.
