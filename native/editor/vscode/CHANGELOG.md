# Changelog

## 0.3.0

Companion editor package for Kalandra 0.99.4. Install the matching native language server from the same GitHub release.

- Declaration grammar highlighting for `export "C" direct fn` native interop declarations.
- Native language-server import-path completion for `std/net` alongside `std/gpu`, `std/stream` and `std/process`, plus diagnostics, completion, hover, signature help, references, rename and formatting for cooperative GPU kernels, streams, subprocesses, TCP networking and C header interop.
- Crash-free workspace diagnostics and editor recovery when unrelated function signatures contain unresolved types or error channels.

## 0.2.0

Companion editor package for the Kalandra 0.99.3 language milestone. Install the matching native language server from the same GitHub release.

- Grammar coverage for explicit returns, local mutation, for/while control flow, yield, generic parameters, traits and associated members.
- Primitive names i8–i64, u8–u64, f32/f64, bool, unit and never.
- Native diagnostics, completion, hover, references, rename, formatting and navigation for the expanded language and standard-library APIs.
- Existing native-server path, project manifest and placement-check settings remain available. The installed extension needs no npm installation.

The extension has its own version; compiler and language-server versions are reported by the installed native tools. This release distributes a VSIX through GitHub Releases.

## 0.1.0

Initial native Kalandra VS Code client, TextMate grammar, native language-server integration and packaged runtime dependency notices.
