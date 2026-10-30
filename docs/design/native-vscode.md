# Native VS Code integration

The native editor client in [`native/editor/vscode`](../../native/editor/vscode) packages the C++ language server for VS Code. It associates `.k` files with Kalandra, provides TextMate syntax highlighting and bracket/comment rules, and connects diagnostics, hover, definition, references, rename, completion, signature help and formatting to `kalandra-lsp`.

## Build and install

From a native Kalandra checkout:

```sh
cmake --build build/native --target kalandra-lsp
cd native/editor/vscode
bun install --frozen-lockfile --ignore-scripts
bun run package
```

The VSIX is written to `build/native/editor/kalandra-native.vsix` relative to the repository root. In VS Code, run **Extensions: Install from VSIX**, choose that file, and set **Kalandra Native: Server Path** in User Settings to the absolute path of your built `kalandra-lsp`. An executable name on PATH also works. Open a trusted local workspace and a `.k` file. No server installation/build/download happens inside the extension.

The VSIX contains a bundled client and its runtime dependency licenses; installing it does not require npm or Bun. VS Code runs that client in its own extension host. Bun is used only to build/package/test this adapter, not to compile Kalandra. The native server and its LLVM libraries are not bundled: an initial [native editor-tools installation/archive](native-tools-install.md) is now available, while complete compiler/runtime and bundled-LLVM distribution remain N5 work. The initial qualification uses macOS Apple Silicon. Version `0.1.0` identifies this separate editor package, not a compiler language release, and this package has not been published to the Marketplace.

## Configuration and lifecycle

- **Server Path** is a machine setting. It accepts an absolute executable path (including spaces) or one executable name on PATH, without shell command parsing. Workspace settings cannot replace this machine-scoped value.
- **Project Path** optionally supplies one explicit `.toml` manifest to the server for entry/package mappings. A relative path requires exactly one local workspace folder; multi-root windows require an absolute path. Leave it empty for standalone roots with relative imports. A single server/manifest serves the window; per-folder manifests and ancestor discovery are not implemented.
- **Placement** adds the server's placement-checking flag. It does not run code or assert that all checked GPU programs can lower.

Configuration changes and workspace-folder changes serialize a stop/start of the server. **Kalandra: Restart Native Language Server** also restarts it after a rebuild or manifest-content change. **Kalandra: Show Native Language Server Output** opens its log. A failed startup reports settings/output actions without blocking activation on a notification. A failed/stopped client can be restarted; process crashes do not cause an automatic retry loop. Deactivation disposes the client.

Language-server execution requires Workspace Trust, with a runtime trust guard as well as manifest capabilities. The server launches directly with ordinary stdin/stdout; the adapter does not add Node-server transport arguments. Only local-file `.k` documents are selected. Virtual workspaces and the browser extension host are unsupported. Remote-host and additional-platform behavior remain unqualified.

## Editor behavior and limits

All semantic features use native compiler information. The adapter does not expand supported language/backend behavior or introduce prototype semantics. See the [server contract](native-language-server.md) for failed-graph behavior, reference/rename workspace boundaries, bounded completion, resource budgets and synchronous analysis. Configuration/package changes require a restart because the native server loads its manifest at startup.

Format Document uses the [shared native formatter](native-formatter.md). VS Code can minimize the server's full-document replacement and normalize a no-change result to an absent edit list. User source stays unsaved until the editor saves it. The extension does not autoformat on save or edit disk files itself.

TextMate highlighting covers native keywords, numeric and builtin types, operators, numbers, Strings/escapes, comments and call spellings. It is lexical coloring rather than compiler semantic tokens; a function-colored token is not proof of a valid call. Theme colors and ligatures follow the editor's preferences. Debugging, semantic tokens, inlay hints, incomplete-call recovery and other editors remain follow-ups.

## Qualification

```sh
cd native/editor/vscode
bun test
KALANDRA_VSCODE_EXECUTABLE='/Applications/Visual Studio Code.app/Contents/MacOS/Code' bun run test:editor
```

The test script uses an existing VS Code executable, packages the actual VSIX, checks its file allowlist and bundled notices, and installs it with the VS Code CLI into a temporary extension directory/profile. It then launches the installed files in the real Extension Development Host with isolated multi-root fixtures. Normal user settings/extensions are not changed. `KALANDRA_VSCODE_CLI` can select a different CLI, and `KALANDRA_LSP` can select another native server build. Test artifacts/results remain under ignored `build/native/editor`.

Unit tests check launch/configuration boundaries, trust gating, serialized restarts, failed-start recovery and shutdown. TextMate/Oniguruma tests tokenize the grammar. Actual VS Code provider commands exercise imports/hover/definition, unopened-caller references with root isolation, rename proposals, partial-name completion, unsaved dependency diagnostics, applied Unicode/CRLF formatting and idempotence, configured package mappings and manual restart. Member-completion acceptance also applies whole-identifier edits for imported record fields, enum constructors and remaining match cases, record/update/enum payload labels, and shorthand/explicit match payload labels. It checks Field/EnumMember kinds, trigger-character requests, supplied-field exclusion and live imported overlays. Applied edits are followed by checked type hover and cleared diagnostics; original fixture files remain unchanged. These tests do not substitute for additional-platform, accessibility/manual UX or Marketplace acceptance.

The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-vscode-2026-10-20.json) records the installed editor version, check list, package and source hashes. The manifest's minimum engine compatibility is not a claim that every supported VS Code version has been tested.

The [member-completion editor qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-vscode-members-2026-10-21.json)
records 14 installed-extension-host checks and seven client/grammar/lifecycle tests
on VS Code 1.142.0, macOS ARM64, with the LLVM 23.1.3 native server. These are
provider-command and applied-edit integration checks, not a claim of manual
suggestion-widget, keyboard-selection or accessibility acceptance.

[Checked signature help](native-language-server.md#checked-signature-help) now
connects the native function signature and active argument to the VS Code provider.
Its initial acceptance uses an imported function in the installed extension host.
Complete-call guidance requires a parsed and checked graph. [Trailing-parenthesis recovery](native-language-server.md#trailing-parenthesis-recovery) now helps at the end of a buffer when arguments are complete; [Missing-argument guidance](native-language-server.md#missing-argument-guidance) also covers empty first/next slots using validated callee signatures.

[Higher-order signature help](native-language-server.md#higher-order-call-signatures)
now follows imported factory results and indexed callbacks in the installed
extension host. The provider checks structural labels, the second active argument
and exact parameter-label ranges for repeated types. It extends existing calls;
no syntax or runtime behavior changes.

Trailing-parenthesis acceptance also requests help for an unsaved imported call
before its closing `)` is typed. It verifies the signature and active argument,
the unchanged buffer/disk source, and the original `L005` diagnostic remaining
visible until the user fixes the source.

Missing-argument provider acceptance now opens an empty imported call and a
factory-result call after a comma. It checks the named/structural signature and
active parameter while ensuring no argument value is inserted into the buffer.
The compiler still diagnoses the unfinished program.

[Identifier argument editing](native-language-server.md#identifier-argument-editing)
keeps the expected parameter signature available while the final argument name is
being typed. Installed-provider acceptance checks the active parameter and exact
label ranges while the original unknown-name diagnostic and failed hover remain
visible; the buffer and disk source are unchanged.

Installed-editor acceptance also checks [record-field argument guidance](native-language-server.md#record-field-argument-editing): the signature remains available while the final field name is being typed, with the actual unknown-field diagnostic, failed hover and unsaved buffer preserved.

The same installed-editor checks now cover [an empty final record field](native-language-server.md#empty-record-field-arguments): parameter guidance remains visible immediately after the dot while the unsaved source and real syntax diagnostic remain unchanged.

Installed-editor acceptance now applies [name completion inside an unfinished call](native-language-server.md#completion-inside-unfinished-calls), checks that the edit changes only the typed name, and verifies that the actual missing-delimiter diagnostic and disk file remain unchanged.

Installed-editor acceptance now applies an [imported type alias in an annotation](native-language-server.md#type-annotation-completion), verifies the existing Struct item kind, and checks the resulting program and alias-preserving hover without changing disk sources.

Installed-editor acceptance now applies an [imported error alias in an expected-error channel](native-language-server.md#error-channel-completion), checks the existing Struct item kind, and verifies that the resulting fallible signature clears diagnostics and has checked hover.

Installed-editor acceptance also applies a [public function import completion](native-language-server.md#import-name-completion), preserves its explicit local alias, and verifies that the resulting call checks and has hover information without writing disk sources.

Installed-editor acceptance also applies a [relative import-path completion](native-language-server.md#relative-import-path-completion), checks its File item kind and string-only replacement range, and verifies that the aliased call checks and has hover information without changing disk sources.

Installed-editor acceptance now requests a [checked collection builtin signature](native-language-server.md#checked-collection-builtin-signatures) for a piped String slice, verifies all three parameter labels and the active explicit argument, and preserves the buffer and disk sources.

Installed-editor acceptance also checks [a missing concat operand](native-language-server.md#missing-concat-operand-guidance): an unfinished call shows the checked int32 array contract and active right operand while its source, disk file and actual unclosed-delimiter diagnostic remain unchanged.

Installed-editor acceptance also applies [builtin call-target completion](native-language-server.md#builtin-call-target-completion) for `concat` and `float32`, verifies Function item kinds and complete identifier replacement, and checks both resulting calls without writing the source file.

Installed-editor acceptance also requests a [checked tensor numeric conversion signature](native-language-server.md#checked-numeric-conversion-signatures), verifies source/destination widths, fixed rank-two shape and the active input parameter, and preserves the buffer and disk source.

Installed-editor acceptance also requests a [checked matrix multiplication signature](native-language-server.md#checked-tensor-operation-signatures), verifies both operand shapes and the derived output shape, and selects the explicit right operand in a piped call without changing buffer or disk sources.

Installed-editor acceptance also requests a [parallel-map callback signature](native-language-server.md#checked-map-callback-signatures), checks the public one-element callback and outer TaskError channel, and selects the callback in a piped call while hiding captures and preserving source.

Installed-editor acceptance also requests [checked builtin hovers](native-language-server.md#checked-builtin-hovers) for a captured parallel map and numeric conversion, verifies their public signatures and error channel, and preserves disk source.

Installed-editor acceptance also enables Placement in the isolated test workspace, applies [device-selector member completion](native-language-server.md#device-selector-member-completion), checks `devices.gpu(1)` without probing hardware, and restores the setting and source before continuing.

Installed-editor acceptance also requests [checked transfer guidance](native-language-server.md#checked-transfer-guidance) with Placement enabled, verifies GPU-bound and CPU-bound hovers plus the active target argument, and preserves the buffer and disk source without probing hardware.

Installed-editor acceptance also applies [transfer call-target completion](native-language-server.md#transfer-call-target-completion) inside an owning parallel group with Placement enabled, preserves the arguments and explicit GPU index, and checks the resulting call and its existing type hints without changing disk sources.

Installed-editor acceptance also requests [checked device-selector guidance](native-language-server.md#checked-device-selector-guidance), verifies CPU/default-GPU hovers and the explicit uint32 index signature, and preserves source and disk files without probing hardware.

Installed-editor acceptance also applies [a call-name completion before missing closing parentheses](native-language-server.md#call-target-completion-before-missing-parentheses), verifies that arguments and the actual delimiter diagnostic remain, then checks the call after explicitly closing it. Disk sources remain unchanged.

Installed-editor acceptance also applies [final-argument suffix completion](native-language-server.md#final-argument-suffix-completion) to a checked record field in an open call, preserves the receiver and actual delimiter diagnostic, then verifies checked hover after explicitly closing the call. Disk sources remain unchanged.

Installed-editor acceptance also requests [signature help inside a final argument suffix](native-language-server.md#signature-help-with-final-argument-suffixes) before missing closing parentheses, verifies the checked parameters and active second argument, and preserves the buffer, actual delimiter diagnostic and disk source.

Installed-editor acceptance also applies [earlier-argument completion](native-language-server.md#earlier-argument-completion) to a field before another supplied argument, preserves the later argument, verifies ordinary delimiter and missing-third-argument diagnostics, then checks the program after supplying that third argument. Disk sources remain unchanged.

Installed-editor acceptance also requests [earlier-argument signature snapshots](native-language-server.md#earlier-argument-signature-snapshots) before later supplied and missing arguments, verifies all checked parameter labels and the first active slot, and preserves the actual delimiter diagnostic, buffer and disk source.

Installed-editor acceptance also requests [signature help for an empty earlier field](native-language-server.md#empty-earlier-field-signature-slots) before a comma, verifies the first active parameter without exposing the private probe, preserves the actual parser diagnostic and source, then checks after supplying a real field.

Installed-editor acceptance also applies [arithmetic argument completion](native-language-server.md#arithmetic-and-comparison-argument-completion), verifies that the full operand-name suffix changes while operators and the actual delimiter diagnostic remain, then checks the call after explicit closure. Disk sources remain unchanged.
