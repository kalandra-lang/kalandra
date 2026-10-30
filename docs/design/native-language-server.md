# Native language server

`kalandra-lsp` is the first native editor protocol implementation. It uses the C++ compiler's checker, [source overlays](native-source-overlays.md) and [diagnostic ranges](native-diagnostics.md); it does not invoke the JavaScript prototype or generate/run application code.

The [unfinished-code completion contract](native-tooling-completion.md) describes the current editor recovery and N4 exit checks. Narrow recovery limits in the implementation records below describe earlier stages and are superseded by that contract.

## Build and connect

```sh
cmake --build build/native --target kalandra-lsp
build/native/native/kalandra-lsp
```

Configure your editor's language-server client to launch the executable with `.k` documents associated with the `kalandra` language. It communicates over framed stdin/stdout, so running it in an ordinary terminal waits for protocol input. A [native VS Code client](native-vscode.md) now packages the editor connection and `.k` highlighting as a local VSIX. Set its Server Path to this development executable; the [native editor-tools component](native-tools-install.md) can also install it under a prefix, while full compiler/runtime distribution remains work.

Optional arguments:

```sh
build/native/native/kalandra-lsp --project path/to/kalandra.toml
build/native/native/kalandra-lsp --placement --project path/to/kalandra.toml
```

The explicit project manifest is loaded at startup and supplies its entry, local package mappings and managed dependency declarations. No ancestor discovery, dependency installation, unsaved manifest editing or automatic configuration reload is provided. Without it, each open file is checked as a root with relative imports. With it, open files and the project entry are checked using the same open-buffer replacements. `--placement` enables shared CPU/Metal placement checks; it does not guarantee GPU lowering or hardware capability.

## Installed dependencies

Install the configured project's dependencies before opening it in the editor:

```sh
kalandra install --project /path/to/project/kalandra.toml
kalandra-lsp --project /path/to/project/kalandra.toml
```

Diagnostics, hover, definition, references, scope completion and rename validation
use the same locked package graph as the compiler. Each library resolves its own
imports through its own dependency versions; application aliases do not replace
private library aliases. Unsaved workspace sources, including an invalid saved
entry replaced by a valid buffer, participate before parsing.

Analysis reads the installed graph without running Git, contacting repositories,
installing dependencies or rewriting the lock/cache. The lock and snapshot
integrity are checked again for each graph load. A missing/stale/invalid lock or
modified snapshot prevents typed results; notification checks log the loader
error, symbol queries return no result and rename/reference searches fail when
they cannot load their required graphs. There is no fallback to an old typed
graph. For projects with managed dependencies, changing the startup manifest
requires restarting the server; unsaved manifest editing is unsupported.

Installed `.kalandra` sources are read-only. Definition jumps can open reachable
library sources, and hover/definition/references retain their package owner within
the application's entry graph. They are not checked as standalone application
files. Unreachable library files have no independent semantic browsing context.
Opening unchanged installed text is supported; changing it in a buffer prevents
analysis until that buffer is restored or closed. Completion probes and formatting
edits are not offered inside installed sources. Rename can change workspace
bindings and explicit local aliases, but cannot rename installed declarations or
produce edits to installed sources. Edit the original library project and install
a new revision instead. Workspace discovery skips `.kalandra` entirely.

The [package-editor qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-packages-2026-10-20.json)
covers framed normal/sanitized requests and a relocated editor-tools archive.
It includes two versions of one library, private import navigation, unsaved entry
replacement, workspace completion/references/rename, read-only cache handling,
manifest/lock/source corruption, and an application/cache moved after installation.
The fixture removes the original repositories before editor analysis and verifies
that all project/cache files remain unchanged. These are protocol tests on macOS
ARM64; they do not add a real-editor or GPU execution qualification.

## Supported protocol slice

The server implements the [Language Server Protocol 3.17](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/) lifecycle and advertises incremental document synchronization (`TextDocumentSyncKind.Incremental`) and UTF-16 positions, plus `hoverProvider: true`, `definitionProvider: true` and `referencesProvider: true`:

- `initialize`, `initialized`, `shutdown`, `exit`.
- `textDocument/didOpen`, `didChange`, `didSave`, `didClose`.
- `textDocument/publishDiagnostics`, including open-buffer versions, compiler codes, source names and severity.
- `textDocument/hover` for checked token types and function signatures.
- `textDocument/completion` for prefix-filtered names in the current lexical/module scope.
- `textDocument/formatting` for verified, syntax-preserving whole-document edits.
- `textDocument/definition` for resolved source declaration locations.
- `textDocument/references` for resolved occurrences in workspace/open/project source graphs.
- `textDocument/prepareRename` and `textDocument/rename` for validated workspace edit proposals when supported by the client.

Changes can replace a UTF-16 range or supply full replacement text without a range. Apply changes in their array order against the evolving buffer: later ranges refer to the text after earlier changes. Full replacements and ranged edits can be mixed; the compiler checks the final buffer once. Empty change arrays are valid no-ops that advance the version.

Range lines and columns are nonnegative protocol integers. Positions past a line's length clamp to its end, without consuming its line ending. LF, CRLF and standalone CR delimit editor lines; language acceptance of the source is still decided by the lexer. Newline characters can be replaced by ranges spanning adjacent lines. Positions inside a supplementary scalar's UTF-16 surrogate pair, nonexistent lines, reversed ranges and malformed range fields are rejected. An optional deprecated `rangeLength` must match the replaced UTF-16 units, including line-ending units, and requires a range.

Each notification is transactional: text and version are committed only after every edit and budget check passes. An invalid later edit leaves earlier edits in that notification unapplied. Versions must increase; duplicate/older changes are ignored. Invalid notifications are logged with `window/logMessage` rather than receiving request responses. A client that has sent an invalid change should resynchronize with a full replacement at a newer version. Unsupported requests receive a method-not-found response. Before initialization and after shutdown, request handling follows the lifecycle state.

Opening or changing a file rechecks all open roots against the current set of unsaved buffers. Changed dependency signatures therefore recheck their callers. Diagnostics can point into unopened disk dependencies as well as open buffers; duplicates from overlapping roots are removed. Correcting errors or closing roots clears previous publications. Closing a dependency replaces its unsaved contents with its disk version for remaining open callers. Published open-document diagnostics carry that document's version; closed/disk documents do not.

Checks are synchronous and process messages in order. There are no background checks that can finish out of order. This first implementation does not debounce, cancel a running check or skip already queued older work. Version checking prevents a received older buffer from replacing a newer one; asynchronous stale-result suppression is future work.

The server accepts local UTF-8 `file:` URIs, including percent-encoded paths and `localhost`. Remote authorities, other schemes, invalid escapes, NUL/line breaks, queries/fragments and non-`.k` paths are rejected. Resolvable symlinks/normalized paths share compiler identity, so the same file cannot be opened twice under aliases. Publications preserve the client's open URI and clear previous URI spellings when ownership changes.

Errors with reliable source ranges appear as diagnostics. Source-less loading/configuration failures are logged instead of inventing source ranges. Compiler recovery and diagnostic limits still apply: this is not an exhaustive error list for every malformed graph.

## Type information on hover

For example, hovering `double` at its declaration or a direct call can show:

```kalandra
fn double(value: int64) -> int64
```

Hover also covers binding/parameter declarations and reads, named function references, function-valued bindings and calls, record field declarations/reads, nominal type declarations, imported function/record names and aliases, and numeric/Bool/String literals. Imported aliases at direct call sites follow the compiler's resolved target and display the local call spelling. Closure captures stay private; displayed parameters exclude lifted environments. String literal contents are omitted from hover text.

Types come from a newly checked source graph using all current open buffers and the configured package mappings. This includes contextual numeric inference and current unsaved dependency signatures. Tensor shapes show fixed extents or `?`, without private inference identities. Error channels show named families and canonical union members, preserving the distinction between a factory's errors and its returned callable's errors.

The server returns `MarkupContent`, respecting the client's first supported plain-text/Markdown preference and using plain text by default. Markdown signatures are fenced as Kalandra code. The returned range covers the hovered token in its original file using UTF-16 positions. Whitespace, comments, punctuation, unopened documents and unmapped positions return `null`. Malformed request parameters return an invalid-params response. Invalid lines or positions inside surrogate pairs return `null`.

A graph with any parse/type error returns no hover information because the checker does not publish partial typed IR. Previous successful types are not reused. Imported error-alias hover presentations and general operator/expression selections are not yet mapped. Checked builtin call hovers are covered below. This is a conservative semantic hover implementation, not name-based guessing.

Hover rechecks synchronously per request; it does not cache a typed workspace or provide a latency guarantee. Presentation is bounded to 4,096 visited type/error nodes, 32 formatting levels and 16 KiB of text. A presentation that exceeds these limits returns `null`. The [hover qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-hover-2026-10-20.json) records semantic and framed-protocol tests for normal and sanitized builds.

## Go to definition

`textDocument/definition` returns a standard `Location` containing the original source URI and the declaration identifier's UTF-16 range. It supports binding/parameter reads, named function references and direct/pipeline calls, record field reads, and the mapped declarations themselves. Imported function calls follow the compiler's resolved identity through aliases, package entries and re-export chains to the original declaration. Same-spelled local functions cannot redirect these jumps.

Calls through a function-valued binding jump to that binding's declaration. They do not guess a runtime function target from a name or possible-call set. Captured references, including captures inside nested closures, retain their original lexical definition separately from the compiler's diagnostic/capture locations. Shadowed names jump to the binding selected by the checker.

Definition and hover share checked token resolution. Both use the current unsaved source graph, fail closed when checking fails, and perform fresh synchronous analysis per request. Closing a dependency restores its disk contents and destination positions. The server preserves the URI spelling of an open target document; unopened targets use canonical percent-encoded file URIs. Jumps do not write files or require the destination to be open.

Unmapped tokens, literals, whitespace, keywords, builtins without source declarations, generated helper locations and failed graphs return `null`. The source document must be open under its registered URI. As with hover, malformed parameters return invalid-params, while invalid lines or split-surrogate positions return `null`. Wildcard patterns and builtin cases without source declarations have no source destination. Definition links and workspace symbols are separate work.

The [definition qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-definition-2026-10-20.json) covers normal and sanitized semantic/protocol checks, including shadowing, nested captures, cross-file aliases/re-exports, package mappings, Unicode URIs, unsaved destination changes, close-to-disk behavior and hover regressions. These are protocol tests, not a real-editor qualification claim.

## Import declaration origins

The compiler retains resolved import metadata for each source name and optional explicit alias, including re-exports and unused imports. Each entry points to the original declaration through the checker's existing export resolution. Function and nominal record IDs support hover; error-family aliases retain their own declaration identity without pretending to be nominal records. These are source-tool metadata, not a second module resolver or runtime behavior change. Invalid graphs still publish no partial typed module.

Definition works on both sides of `from "./library.k" import scale as twice`, following the original `scale` declaration. Function hover uses the selected local spelling; nominal record aliases show their selected name. Error-alias declaration navigation is supported, but their hover presentation remains future work. Re-export tokens follow the original source, including chains and configured packages.

References include both the imported source-name token and an explicit alias token, including unused bindings. These are occurrences of the underlying declaration; `includeDeclaration` controls only the original declaration, not whether alias tokens appear. Metadata keeps the two token roles and the re-export flag distinct for alias-preserving rename. A reference list alone still does not determine safe rename edits.

The [import-origin qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-import-origins-2026-10-20.json) covers normal and ASan/UBSan compiler, semantic and protocol checks for imported functions, records, error aliases, re-exports, unused imports, metadata roles, current buffers and original URI/range mapping.

## Type annotations and record construction

The checker retains source-token metadata while resolving annotations and constructing/updating records. Navigation and references therefore use the same nominal types and field indices as compilation, including imported and re-exported types. This metadata is deduplicated by source span and published only with a successful checked module; invalid annotations or constructors provide no partial navigation.

Nominal annotation tokens navigate to the original type declaration. Coverage includes parameter/result annotations, typed bindings, record fields, array element annotations, and nested function parameter/result types. Declared error channels and transparent error-alias members retain source identities too. An alias use points to that alias's declaration rather than collapsing to its canonical error family. Builtin numeric/Bool/String/TaskError and tensor annotation tokens provide checked hover information, but have no invented source declaration or reference identity. Array element and tensor element tokens show their element type rather than the surrounding collection.

A record constructor's type name navigates to the nominal declaration. Construction and immutable-update field labels navigate to the actual field declaration and participate in the same references as field reads. Same-spelled fields in different records remain distinct. Nominal tokens show their declaration header on hover; field labels show their checked value type. Enum construction and pattern roles are described below.

The [type-token qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-type-tokens-2026-10-20.json) covers normal and ASan/UBSan semantic/protocol checks, nested callable/array annotations, local/imported constructors and updates, nominal field identity, error aliases, source offsets after incremental Unicode/CRLF edits, builtin annotations and invalid-graph isolation. These are source-tool checks, not new runtime/GPU capabilities or real-editor qualification.

## Enum cases and match patterns

Enum and application-error case declarations, qualified constructors (with or without payloads), and qualified match cases now retain their checked source identities. The family token navigates to its type declaration; the case token navigates to the actual variant declaration. Imported/re-exported families and copied error-union layouts preserve the original case and field sources. Identical case or payload names in different variants/families do not merge. Case hover shows its value type, for example `Some: Choice`.

Constructor payload labels and explicit pattern labels, such as `{value:item}`, navigate to the declared payload field and participate in its references. The explicit `item` token declares a separate local binding. Reads and closure captures of that binding navigate to the pattern binding, respecting arm-local scope and shadowing.

A shorthand pattern such as `{value}` has two roles at one source token: a payload-field reference and a local binding declaration. Primary navigation and reference queries on that token select the local binding; reads point back to it. A reference query from the payload field also includes the shorthand token. The source metadata retains that secondary field role independently; collection deduplicates locations within each queried identity. Rename expands shorthand where needed to preserve the other role, rather than blindly replacing every reference token.

Builtin `TaskError`/`FaultKind` match cases and builtin payload labels provide checked type hover, but have no invented source declarations. Their pattern-bound locals still have ordinary source identities. Wildcards remain unmapped. Invalid, duplicate, non-exhaustive or otherwise failed graphs expose no partial editor identities. This adds source-tool metadata without changing matching, error handling or GPU capabilities.

The [enum/pattern qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-enum-tokens-2026-10-20.json) covers normal and ASan/UBSan semantic and protocol tests for case/field separation, explicit and shorthand bindings, closure captures, shadowing, error unions, builtins, imported enum origins, unsaved URI/range changes and failure isolation. Dedicated editor checks for enum/pattern interactions remain future work; the rename implementation is described below.

## Find references

[`textDocument/references`](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#textDocument_references) returns source `Location` entries for the selected checked identity. It covers binding/parameter declarations and reads, nested closure captures, named function references, direct/pipeline calls, function-valued bindings, and record field declarations/reads. Shadowed bindings and fields with the same spelling remain distinct. Imported calls and function references resolve through aliases, re-exports and configured packages to their original declaration.

The search checks eligible discovered workspace sources, open roots within the selected workspace and its configured project entry, using current unsaved buffers. Without configured workspace discovery, it retains the open/project graph search. Each root includes its reachable dependencies. Identity across separately checked graphs uses the canonical original declaration path and byte span, not graph-local numeric IDs or name matching. Results are deduplicated and sorted by canonical path and source position; open documents retain their registered URI spelling. `context.includeDeclaration` is required and controls inclusion of the original declaration. A mapped but unused symbol returns an empty array when declaration inclusion is false.

This is a bounded semantic search with fresh workspace discovery, not a persistent workspace index. Eligible unopened importers in the selected root are searched; excluded paths and other workspace roots are outside its scope. Shorthand patterns still require edits that preserve both roles, so references alone are not sufficient for safe automated rename. Function-valued calls refer to the lexical binding, not every possible runtime target. Unmapped or invalid positions and failed query graphs return `null`, as for definition. If selection succeeds but any searched graph cannot load/check, the request fails with `RequestFailed` (-32803); no partial array is returned. Any erroneous eligible source in the search scope can prevent a search, even if unrelated to the selected symbol. Correct or exclude it from the scope before retrying.

Each graph is checked once during collection and its identifiers indexed once; the implementation does not recheck per token. Queries remain synchronous and uncached. Aggregate composed source scanning is capped at 64 MiB per request (shared dependencies count per root), and unique results at 100,000 locations. Exceeding either budget fails the request rather than truncating it. Existing per-graph compiler limits also apply. No files are modified.

The [references qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-references-2026-10-20.json) records normal and ASan/UBSan semantic/protocol tests, including declaration filtering, shadowing, nested captures, cross-file aliases/re-exports, deduplication, Unicode/CRLF, current unsaved destination positions, registered URIs and failure isolation. This does not qualify real-editor behavior or full-workspace coverage.

## Workspace source discovery

At initialization the server accepts at most 16 local directory roots from `workspaceFolders`, preferring a non-null array over `rootUri`. Empty folders explicitly disable discovery. If no non-null folder array is supplied, a non-null `rootUri` supplies one root. If both fields are absent and `--project` is configured, the explicit manifest's parent directory is the discovery root. Explicit null/no-folder settings suppress that project fallback. Without a root there is no implicit scan of the working directory or source ancestors. Directory URIs share the existing local/localhost, percent-decoding and canonicalization rules; invalid or inaccessible roots return invalid-params without completing initialization. Deprecated `rootPath` is not used.

The query document selects the most specific containing root. Only that root is discovered, and only open files/project entries inside it are added as roots. Other configured roots and open files outside it cannot contaminate that search. A query outside all configured roots searches only its own dependency graph. When no workspace roots are configured, open graphs and the optional project entry retain the previous search behavior. Diagnostics still recheck open buffers independently of reference discovery.

Discovery recursively lists regular `.k` files, with deterministic sorted results. It skips symlink files/directories (including broken links), and skips directories named `.git`, `.hg`, `.svn`, `node_modules`, `build`, `dist`, `target`, `.cache`, `.kalandra`, `.svelte-kit` or `.vercel`. These fixed exclusions apply to discovery, not explicitly open files or imports. There is no `.gitignore` interpretation or configurable exclusion glob yet. Explicit imports and package mappings can reach external dependencies; discovery does not crawl their directories or find reverse importers outside the selected workspace. These are search boundaries, not an import sandbox.

Each request rescans the root and reads current source graphs using open-buffer replacements. New/deleted files and disk edits are visible on the next search; closing a buffer restores its disk contents. No source files are written. There is no filesystem watcher, persistent index, debounce or point-in-time filesystem snapshot. Concurrent filesystem mutation is not a qualified atomic snapshot.

Discovery permits at most 512 source files, 20,000 visited directory entries and 32 nested directory levels per selected root; excluded directory entries and symlinks count toward the entry budget but are not traversed. Missing/unreadable directories, entry-inspection failures, unusable source paths and budget exhaustion fail the request with `RequestFailed`, without a partial array. Existing composed-source/graph/result limits apply after discovery. Every eligible candidate graph must load and check successfully; intentionally invalid fixtures should be outside the chosen root or an excluded directory.

The server advertises workspace-folder support with change notifications disabled. Restart it after changing workspace folders or project configuration. The [workspace qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-workspace-2026-10-20.json) covers normal and ASan/UBSan discovery/protocol tests: unopened callers, fresh disk changes, buffer precedence, URI preservation, root precedence/isolation, nested roots, project/package mappings, external dependencies, exclusions, symlinks, scan limits and failure recovery. These are protocol and filesystem tests, not real-editor or additional-platform qualification.

## Document formatting

The server advertises `documentFormattingProvider: true`. Format Document uses the [shared native formatter](native-formatter.md) on the current open buffer, independently of dependency resolution or type checking. It returns a whole-document UTF-16 edit for the editor to apply, with fixed two-space indentation and LF line endings. Unchanged/invalid source or unopened documents return no edits. Formatting does not write files, update server buffers or publish diagnostics; the editor's subsequent change notification updates the buffer. See the formatter contract for options, verification failures and resource limits. Range/on-type formatting remains work; [initial VS Code acceptance](native-vscode.md#qualification) now covers the whole-document path.

## Scope completion

The server advertises `completionProvider: {resolveProvider: false, triggerCharacters: [".", "{", ",", "/"]}`. Completion returns plain-text items and UTF-16 `textEdit` ranges covering the entire current identifier, including any suffix after the cursor. An empty expression position can also request names. The document must be open under its registered URI; unopened documents and invalid positions return `null`, and malformed parameters return InvalidParams (-32602).

Suggestions include parameters, preceding immutable bindings, eligible closure captures, named functions, nominal constructor names, and imported aliases visible in the current module. Compiler builtin names are additionally offered at call targets, as covered below. The checker supplies the actual lexical frame, so later bindings, other branches, unimported dependency names and shadowed declarations do not leak into the list. Matches use a case-sensitive prefix and deterministic alphabetical order. Results are limited to 256 names; `isIncomplete` asks the client to request again after the prefix narrows. Item details distinguish bindings, parameters, functions and types; these are name suggestions rather than expected-type filtering or inferred signature presentation.

The request replaces only the current identifier (or inserts one at an empty position) in a temporary source overlay, then parses the graph and snapshots scope immediately before the checker resolves that identifier. Current dependency buffers and configured package mappings participate. Probe diagnostics are discarded: the request does not publish diagnostics, change buffers, write files, run code or contact hardware. Normal checking still never publishes partial typed IR. Completion exposes only scope names, without partial/stale inferred types.

Apart from bounded construction/pattern-label probes and trailing-call repair below, completion needs the temporary graph to parse and the checker to reach the query without prior diagnostics. Unsupported missing delimiters, unfinished declarations, failed imports or earlier semantic errors can produce an empty list. Later errors do not invalidate an already captured scope. Ordinary expression names, direct/piped callees and named map callbacks are covered; bare package paths, general pattern bindings and declaration names are not completion contexts yet. Relative import paths are covered below. Public names in explicit imports/re-exports are covered below. Expected-error channels are covered below. Type annotations are covered below. Qualified enum cases in match patterns are covered below. Checked record fields are covered below. Strings/comments do not supply lexical-name suggestions. There is no general parser recovery, auto-import, fuzzy ranking, snippet expansion or completion resolve. Checked signature help is covered below. Existing source/token/graph/checker budgets still apply, and requests remain synchronous.

The [completion qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-completion-2026-10-20.json) records semantic and stdio tests in normal and ASan/UBSan builds. [Initial VS Code acceptance](native-vscode.md#qualification) now covers partly typed names; broader completion interaction remains follow-up work.

## Safe rename proposals

Rename requires a configured workspace containing the query document. The client must advertise `workspace.workspaceEdit.documentChanges`; otherwise `renameProvider` is false and rename requests fail rather than falling back to unversioned edits. With that capability, the server advertises `renameProvider: true`, or `{prepareProvider: true}` when the client also advertises `textDocument.rename.prepareSupport`. `prepareRename` returns the selected token's UTF-16 range and placeholder, or `null` for unsupported tokens. The document must be open under its registered URI.

`rename` returns a standard `WorkspaceEdit.documentChanges` proposal and does not apply it, alter server buffers or write source files. Each open document keeps its registered URI and current version. Closed files use canonical URIs and `version: null`. Standard closed-file edits cannot enforce a source version after the response; client apply/conflict handling still matters. Scratch documents without a containing configured workspace and declarations/edits outside the selected workspace are not renamed. Discovery exclusions and search limits still bound coverage; external reverse importers are not rewritten.

Rename follows surface bindings, whereas reference search groups underlying declarations. Renaming an exported `scale` updates its declaration and unaliased uses/imports. In `import scale as double`, only the imported source name changes; the explicit alias and its uses remain `double`, even if the alias originally had the same spelling as the source. Selecting the alias or one of its uses renames that local alias instead. Re-export aliases establish the same boundary: their published name propagates through unaliased re-exports and downstream import-source tokens, preserving callers' explicit local aliases. Module loader/export resolution is checked before constructing these source relationships.

Record fields, enum cases, payload fields and lexical bindings use their distinct checked identities. Renaming a payload field expands `{value}` to `{amount:value}`, preserving the local binding. Renaming that pattern binding instead expands it to `{value:amount}` and updates its reads/captures. Explicit pattern labels and bindings remain separate. The `main` function entry name is fixed. Builtins without user source declarations, literals, wildcard patterns and other unmapped positions are not rename targets.

New names use ASCII identifier syntax, at most 128 bytes, excluding `_`, control/type/intrinsic spellings and `main`. Invalid names return invalid-params (-32602). Existing lexical bindings that share a grammatical spelling, such as a handler parameter named `error`, can still be renamed; syntax keywords themselves have no binding identity.

The planner snapshots checked source graphs, computes all edits, then reloads and checks every searched graph against virtual replacement buffers. It compares the complete indexed use-to-declaration relationships before and after, mapping source positions through edits and shorthand expansions. A compiling change that captures a different binding is rejected, as are duplicate bindings, export collisions, unresolved imports and other type errors. A source identifier with an affected spelling that cannot be mapped conservatively rejects the operation; some parenthesized references or callback forms may therefore be refused until their source mapping is extended. Failure returns RequestFailed (-32803) with no partial edits. Same-name requests return an empty proposal.

Closed source contents and the discovered root list are checked again before returning edits. Rechecking uses each graph's own replacements, so a workspace rename can span more files than one module graph's file limit. This is not an atomic filesystem snapshot or an apply transaction: concurrent changes after validation remain outside server guarantees. Each analysis/validation pass has a 64 MiB aggregate composed-source budget, 300,000 indexed relationships and 500,000 identifier tokens; proposals permit at most 100,000 edits. Existing per-graph source/file limits apply. Replacement text is constructed in source order; source-position mapping uses per-file prefix offsets, and edit ranges are converted to UTF-16 in batches per file.

The [rename qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-rename-2026-10-20.json) records normal and ASan/UBSan tests for local/captured bindings, silent capture rejection, record/enum fields and cases, shorthand roles, import/re-export alias barriers, unopened callers, 2,001 edits in one source, 131 edited files, Unicode/CRLF ranges, negotiated capabilities, versioned proposals and no source writes. Protocol tests independently apply returned edits to fixture text. Full real-editor, concurrent-filesystem and additional-platform qualification remain separate work.

## Limits and qualification

Protocol headers are bounded to 8 KiB, frame bodies to 32 MiB and JSON nesting to 128 levels. Open buffers share the existing source budget (16 MiB including one separator byte per buffer) and file-count limit (128). Reachable module graphs have their own existing compiler limits. Each notification permits at most 1,024 changes. Transactional editing also has a 128 MiB work budget: charge the original buffer once, each replacement plus one unit, and the current buffer for each ranged edit. This bounds the initial simple scan/copy implementation; large edit batches may require full replacement or smaller notifications. Every intermediate buffer must fit the shared open-buffer byte budget. These are explicit bounds, not responsiveness guarantees. No source files are written.

Malformed JSON with valid framing returns a parse error and leaves the session usable. Invalid, duplicate, oversized or truncated framing ends the process with status 2 because a safe next message boundary is unavailable. A normal shutdown/exit returns 0; exit or EOF before shutdown returns 1. Standard output contains only protocol frames. Standard error is reserved for startup/fatal failures.

Independent Python-stdlib protocol tests launch normal and ASan/UBSan builds, exercise interactive pipe framing, lifecycle, versions, Unicode ranges/URIs, source isolation, dependency changes/clearing, project packages, budgets and malformed input. The [initial qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-2026-10-20.json) covers the first full-sync implementation. [Incremental edit qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-edits-2026-10-20.json) adds sequential/mixed edits, Unicode scalar boundaries, CRLF/CR positions, line-end clamping, transactional failure, deprecated lengths and edit limits. Third-party LLVM libraries are not rebuilt with sanitizers. No real editor or additional operating system is qualified by these protocol tests.

The [native VS Code qualification](native-vscode.md#qualification) now tests the packaged client against the real editor host on macOS ARM64. An [initial native tool install/archive](native-tools-install.md) now supports this editor connection outside the build directory. Additional editor/platform acceptance, complete compiler/runtime distribution, completion recovery, additional member contexts and broader signature-help recovery remain follow-ups. Filesystem watching, persistent workspace indexing and asynchronous scheduling remain separate work.

### Record-field completion

Typing `value.` now suggests the fields of a checked, completed record receiver.
A partial name filters by prefix, and accepting an item replaces the entire member
identifier, including a suffix after the cursor. Results are sorted and use the
same 256-item bound. Fields are marked as LSP Field items; the server advertises
`.` as a completion trigger.

The checker resolves the receiver using current local and imported declarations.
Nested fields, function results, indexed record arrays and captured records use
the same ordinary field-access rules. Unsaved dependency overlays participate,
including package sources through the existing graph loader. A primitive, array,
unsettled task, fallible receiver, invalid field chain or unmatched enum does not
supply record-field suggestions. There are no synthetic properties or guessed
fields. Qualified enum cases are covered below; construction labels are covered below.

Only names escape the optional checker query. The temporary probe may fail normal
field lookup, but checking still publishes no partial typed IR. Prior errors block
suggestions; probe diagnostics never reach the editor. No files, open buffers or
runtime state are changed. Parser recovery remains outside this slice.

[Member-completion qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-members-2026-10-21.json)
covers semantic and framed protocol tests, imported overlays, UTF-16 replacement
ranges and relocated Release editor tools. [Installed VS Code acceptance](native-vscode.md#qualification)
now covers record-field provider results and applied whole-identifier edits.

### Enum-case completion

Typing `Choice.` in an expression now offers that unshadowed enum's declared
cases, including cases that require named payloads. Partial identifiers and
existing payload-construction syntax use whole-identifier edits; completion does
not insert payload fields or snippets. Imported and re-exported type aliases use
their original nominal declaration, and unsaved dependency changes participate.
A local value named `Choice` follows ordinary value/record-field lookup instead.

Qualified `match` patterns also offer cases valid for the checked subject and
family. Cases handled by earlier arms are omitted, and a preceding fallback
leaves no later candidates. A wrong family produces no suggestions. Declared
error families, their aliases/unions, and built-in task/fault patterns follow the
checker's existing match rules. Error unions keep original family identities;
private synthetic union case names are never offered as constructors.

Case items use LSP EnumMember (20), plain insertion text and the existing dot
trigger. Alphabetical ordering, prefix filtering, the 256-item bound, UTF-16 edits,
prior-error gating and no partial typed IR remain shared with record fields.
Pattern bodies/delimiters must still parse: this does not add parser recovery or
complete pattern binding names. Enum values do not acquire direct payload fields.

The [enum-completion qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-enum-completion-2026-10-21.json)
covers constructors, payload syntax, remaining match cases, shadowing, wrong
families, unions, built-in task errors, a 300-case bound, imported overlays and
relocated Release editor tools, with normal and sanitizer protocol/semantic runs.
[Installed VS Code acceptance](native-vscode.md#qualification) now covers enum
constructor items and exclusion of already matched cases, with applied edits.

### Construction-label completion

Named fields are now suggested inside record construction, immutable `with`
updates and a selected enum payload. For example, requesting completion in
`Point { | }` offers Point's fields; `Point { x:1, | }` omits `x`. Fields supplied
elsewhere in the same initializer list, including after the cursor, are excluded.
Enum payload suggestions contain only the selected case's fields, without tag
storage or fields belonging to other cases. Constructor shadowing and update
receiver validation follow the ordinary checker.

Partial labels replace the entire identifier. If a separator is absent, the plain
edit inserts `name: `; an existing colon is preserved. No payload values or snippets
are guessed. In addition to `.`, the server advertises `{` and `,` as completion
triggers. The actual client controls when suggestions appear.

A narrowly scoped temporary probe fills one missing field-label/value slot before
a closing brace or comma so the ordinary parser and checker can inspect its owner.
This also handles an existing colon whose value is still empty. The temporary
placeholder never appears in returned edits, buffers, files or diagnostics. Other
syntax must still parse: missing closing delimiters and unrelated malformed source
are not recovered. Initializer value positions continue to offer lexical names.

Imported declarations and live dependency overlays use the existing source graph.
Earlier semantic errors stop suggestions; no partial typed IR escapes a failed
check. Prefix filtering, deterministic order, the 256-name bound, UTF-16 ranges
and plain LSP Field items remain shared with record-field completion.

The [construction-label qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-labels-2026-10-21.json)
covers empty/partial slots, missing values, existing colons, fields already supplied
before/after the cursor, nested records, immutable updates, selected payloads,
invalid owners, lexical value completion, list bounds, Unicode edits, imported
overlays and relocated Release tools. Pattern payload labels are covered below;
general parser recovery and type-directed value snippets remain separate.
[Installed VS Code acceptance](native-vscode.md#qualification) now covers constructor,
update and enum payload labels, separator behavior and imported overlays.

### Pattern payload-label completion

Inside a selected `match` case, completion offers its checked payload fields.
For example, `Choice.Some {va|}` offers `value` as a shorthand binding;
`Choice.Some {va|:item}` changes only the field label and preserves `item`.
Empty payload slots are supported too. The edit replaces the whole identifier
and inserts only the field name, without a colon or a guessed binding name.
Completion in `value:it|em` does not offer payload labels.

Fields already listed before or after the cursor are excluded by their labels,
not their local binding names. Only the selected case's fields are available.
Imported aliases, current dependency overlays, error-union family identity and
built-in `TaskError` payloads follow ordinary match checking. Unknown cases,
wrong families, unreachable cases and earlier semantic errors stop suggestions.

The existing bounded temporary probe now uses an identifier placeholder so it
can represent either a constructor value or a pattern binding. A missing explicit
binding can be probed before a closing brace/comma; no placeholder is returned or
persisted. Other syntax must parse, including the arm body and closing delimiters.
Sorted prefix filtering, the 256-item bound, plain Field items and UTF-16 edits
remain shared with other field completion.

The [pattern-label qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-pattern-labels-2026-10-21.json)
covers semantic queries, normal/sanitizer protocol runs and relocated Release
tools. [Installed VS Code acceptance](native-vscode.md#qualification) now applies
shorthand and explicit-binding label edits and checks the resulting local reads.
General binding-name suggestions and broad parser recovery remain follow-ups.

### Checked signature help

`textDocument/signatureHelp` returns a single checked signature for the innermost
parenthesized call at the cursor. The server advertises `(` and `,` triggers.
Named functions retain source parameter names; function bindings, parameters and
callable record fields use their structural parameter types. Imports, aliases and
unsaved dependency overlays follow the same graph as hover. Expected-error return
channels are included, while compiler capture parameters are omitted.

The active parameter counts only commas at the current call's delimiter depth.
Nested calls, arrays, records and String contents do not advance the outer
argument. For `value -> combine(other)`, the implicit pipeline value occupies
parameter zero, so `other` selects parameter one. Zero-argument functions have no
active parameter. A client advertising `labelOffsetSupport` receives UTF-16
parameter-label ranges, which distinguish repeated structural types; other
clients receive the standard string labels.

Complete-call guidance requires a parsed and checked graph. A bounded temporary overlay can
close trailing parentheses as described below. At an empty argument slot, a
separate validated-callee snapshot can provide guidance despite the selected
call's argument-count error. Checked collection, numeric conversion, tensor operation and map-family signatures are covered below; other builtin families remain unsupported. Invalid
positions, closed documents and other invalid query contexts return `null`.
The server does not reuse stale signatures or publish partial typed programs. Requesting help makes no source changes or diagnostic publications.

The [signature-help qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-signature-2026-10-21.json)
records normal/sanitizer semantic and protocol tests, hover regressions, relocated
Release tools and installed VS Code provider acceptance. Broader incomplete-call
recovery, additional builtin signatures and editor/platform acceptance remain open.

#### Higher-order call signatures

Signature help also accepts factory results (`make()(value)`), function-array
indexing (`callbacks[0](value)`), indexing a returned array, and direct anonymous
calls. It resolves the callee value from the checked `FunctionInvoke` at the
selected call's exact source span. It does not substitute the factory's own
signature or infer targets from source spelling. Pipeline calls use the checked
outer pipeline span and account for the supplied first argument.

These calls display a structural label such as `fn(int64, int64) -> int64`;
parameter names are not invented for the returned/indexed value. Captures remain
hidden, and expected-error channels belong to the actual invoked callable.
Inconsistent lowered callable signatures fail closed. Invalid indices, noncallable
results, wrong supplied argument types and failed graphs provide no partial or stale signature. Empty slots use the separate signature snapshot described below.
The operation adds editor support to existing language forms; it does not execute
the factory, allocate a runtime closure or change language behavior.

[Higher-order qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-higher-signature-2026-10-21.json)
records semantic/protocol regression tests, imported unsaved overlays, relocated
Release tools and installed VS Code provider checks for factory/indexed calls.
Trailing-parenthesis recovery and missing-argument guidance are covered below;
builtin signature help remains separate work.

#### Trailing-parenthesis recovery

At the end of an open buffer, `combine(1, 2` can now show the checked signature
before its closing `)` is typed. Zero-parameter calls such as `constant(` also
work. Nested trailing calls, factory results and indexed/piped callees follow
the same signature and active-argument rules.

The request first tries the actual graph. If it does not parse, recovery requires
that the root lexer report only unclosed delimiters, every unmatched delimiter be
`(`, no more than eight be open, and only whitespace remain after the cursor.
The server inserts those closing parentheses at the cursor in a temporary root
overlay, reloads current imports/dependencies, and requires a fully successful
parse and check. Existing source/token/checker budgets still apply. At most one
repaired graph is tried; ordinary semantic errors do not trigger repair.

No arguments, values, array/record delimiters or names are invented. Missing
argument slots use the separate validated-callee path below. Invalid argument
types, failed imports, earlier errors, later code,
mismatched delimiters and unclosed Strings remain unrecovered. Read-only package
and project graph constraints are unchanged.

The source buffer, disk files and published diagnostics remain those of the
actual program. In particular, a displayed signature does not clear its original
unclosed-delimiter diagnostic. The [recovery qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-signature-recovery-2026-10-21.json)
records normal/sanitizer semantic/protocol checks, current imported overlays,
relocated Release tools and installed VS Code acceptance of this boundary.
Broader parser recovery remains future work. Missing-argument guidance follows below.

#### Missing-argument guidance

The editor can now show parameters at `combine(`, `combine(1,` and the equivalent
empty slots before an existing `)`. Guidance is requested only immediately after
the call's opening parenthesis or an argument-separating comma, with no argument
expression between the cursor and its closing parenthesis. The bounded trailing
parenthesis overlay supplies syntax when the closing delimiter is not typed yet;
it never supplies an argument value.

The checker resolves the selected callee in its actual lexical/import scope and
checks every supplied argument against its parameter type. Named functions must
have a fully checked definition before their named signature is captured.
Completed function values use their validated structural signature, including
factory/indexed results, fields and pipeline targets. Captures remain hidden;
nominal parameter spellings and expected-error unions retain display metadata.
An unfinished recursive named definition is not a signature source for this path.

This is a separate editor query. It returns only a function signature, optional
parameter names and error-family display names, with no expression graph,
executable IR or partial typed module. The normal check still fails with `S027`.
The snapshot is reset for each check and discarded if any reported diagnostic
is not that selected call's argument-count error. Invalid supplied arguments,
unknown or shadowed noncallable targets and invalid target definitions produce
no guidance. Showing parameters is not validation of the unfinished program.

[Missing-argument qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-missing-arguments-2026-10-21.json)
records normal/sanitizer semantic and protocol checks, ordinary callable-checker
regressions, imported aliases/current overlays, installed Release tools and VS Code
provider acceptance. The source, disk and actual-program diagnostics remain
unchanged. Final bare-identifier editing is covered below. More complex unfinished argument
expressions, broad parser recovery, additional builtin signatures and wider editor/platform
acceptance remain follow-ups.

#### Identifier argument editing

Parameter guidance now stays available while the final argument is a bare name,
such as `combine(1, val)` with the cursor in or at the end of `val`. It also works
with an identifier suffix after the cursor and with a trailing unclosed call.
Named, bound, factory-result and indexed/piped callees retain their existing
signature and active-parameter rules.

For this query only, the parsed argument list is projected to the arguments before
the name being edited. The checker then uses its validated-callee snapshot path:
the callee and earlier arguments must check, while the active identifier is not
claimed to resolve or have the right type. Only that snapshot can supply the hint;
a successfully checked projected graph is never substituted for the actual
program's analysis. No placeholder value, inferred value type or source edit is
created. Ordinary hover, compilation and diagnostics still inspect the actual
argument, including unknown-name and type errors.

The projection requires the final argument to be exactly a bare identifier and
the cursor to be inside that identifier or at its end. Names in earlier arguments,
grouped names and compound expressions are not projected. Record-field argument
editing uses the additional receiver check described below. Extra arguments do
not invent extra parameters. This is signature guidance during an
edit, not validation or completion of the argument itself.

[Identifier-edit qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-identifier-arguments-2026-10-21.json)
records semantic/protocol and hover regressions, current imported overlays,
installed Release tools and VS Code provider acceptance. The checks verify that
the original unknown-name diagnostics and failed hover remain separate from the
signature hint. Broader expression recovery and additional builtin signatures remain open.

#### Record-field argument editing

Parameter guidance also stays visible in a final field argument such as
`combine(1, reading.va)` with the cursor inside or at the end of `va`. The final
field identifier may be incomplete or unknown. Nested record receivers, indexed
records and record-returning calls work when the receiver checks as a completed
ordinary record. Named and structural callees, pipelines and trailing-parenthesis
recovery retain their existing rules.

The query projects only the final argument, then checks the callee, earlier
arguments **and the receiver** before capturing the missing-argument signature.
It does not claim that the final field exists or has the required parameter type.
An unknown receiver, invalid intermediate field, invalid index, non-record value,
unmatched enum payload or any other graph error suppresses the hint. The receiver
is checked in the same lexical frame; this creates no executable or partial typed
program. Existing compiler diagnostics and ordinary hover still analyze the real
field expression, and query-only checking never changes buffers or files.

For a nonempty field, the cursor must be within its final identifier or at its
end. The initial final-argument path is complemented by
[earlier-argument snapshots](#earlier-argument-signature-snapshots). Empty field
slots use the bounded recovery below; arbitrary unfinished expressions remain
unsupported. End-cursor and bounded EOF-only repairs are described below.

[Record-field argument qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-field-arguments-2026-10-21.json)
covers semantic and protocol checks, live imported record overlays, normal hover
isolation, relocated Release tools and installed VS Code provider commands.

#### Empty record-field arguments

Signature help also works immediately after a dot in an argument, such as
`combine(1, reading.)` or `combine(reading., 2)`. The cursor must be directly
after the dot. The next non-layout token must be a closing parenthesis or comma,
or there must be no following token. Existing identifier suffixes, commas and
compound expressions are never overwritten. Ordinary hints for already-valid
code remain unchanged.

Only when the actual source does not parse, the query inserts one private field
identifier into its temporary root overlay. The identifier only locates the
existing [checked field projection](#record-field-argument-editing); it is never
read as an argument value. Final-argument projection removes that argument from
the request AST; earlier-slot snapshots retain its syntax without constructing
a value. The callee, earlier arguments and completed record receiver
must still check; unknown receivers and other graph errors suppress the hint.
No field existence, value type or partially typed program is inferred from the
probe. A recovered probe is required to take the field-projection path; it cannot
fall back to a successful check of an invented value, even if a user has a field
with the same name. It cannot appear in returned signatures, buffers, files, hover
or normal diagnostic publications.

An empty field at the end of an open call can combine with the existing repair of
at most eight trailing parentheses. Only whitespace may follow the original
cursor for that delimiter repair. Lexical errors other than unclosed delimiters,
unclosed arrays/records and remaining syntax errors still reject the query.
Source budgets apply to both the identifier and any inserted parentheses, and
the source graph is loaded at most three times for this request.

[Empty-field qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-empty-field-2026-10-21.json)
covers semantic and protocol checks, imported overlays, relocated Release tools
and installed VS Code commands while retaining the actual syntax diagnostic.

#### Completion inside unfinished calls

Name and record-field suggestions now work at the end of an unfinished call,
including `combine(rea` and `combine(1, reading.va`. Empty final argument slots,
bound functions, factory-returned functions, nested calls and pipelines use the
same completion names and whole-identifier replacement rules as balanced source.
A balanced call that is still missing later parameters can also provide names in
its current argument.

The initial argument-completion and signature-help paths share a trailing-call
delimiter policy: the original cursor must be at the end of the buffer with only whitespace after
it, the only lexical errors may be unclosed delimiters, and at most eight
parentheses can be closed. Arrays/records, mismatches, strings, remaining suffixes
and later source are not repaired. The existing completion probe and any temporary
parentheses count against source budgets. The graph still has to parse after the
repair; this is not general parser recovery. Call-target names and simple final
argument names/members additionally use the bounded EOF repair described
[below](#call-target-completion-before-missing-parentheses) and its
[final-argument extension](#final-argument-suffix-completion).

For a known function whose supplied argument contains the query but later
parameters are missing, the checker validates the callee and supplied prefix,
then visits the current argument with its expected parameter type. Only completion
names escape that query; no partial typed module or guessed later arguments are
created. Unknown/noncallable callees, invalid named bodies and earlier argument
errors block suggestions. Builtins retain their existing individual admission
rules. Completion still offers lexical names rather than promising each suggestion
has the expected type. Normal compilation keeps its original arity diagnostic.

Returned edits replace only the requested name. They never insert the recovery
parentheses into the user's buffer, and ordinary diagnostics/hover continue to
analyze the actual unfinished source. Imported aliases, live dependency overlays,
UTF-16 ranges, prefix filtering and the 256-item bound retain their existing rules.

[Unfinished-call completion qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-open-call-completion-2026-10-21.json)
records normal/sanitizer semantic and protocol checks, signature-help regressions,
relocated Release tools and applied installed VS Code completion edits.

#### Type-annotation completion

Annotations now offer type names in function parameters/results, local bindings,
record fields, enum payload fields, anonymous functions and nested function types.
Empty and partly typed names use the same whole-identifier edits as expression
completion. An imported `Reading as Sample` offers `Sample` in the importing
module; private names and the original unimported spelling do not leak from the
library. Ordinary local variables and functions are not type suggestions, and a
same-spelled value binding does not hide an annotation's type name.

Builtins include the ten concrete numeric types, `int`/`uint`/`float`, `Bool`,
`String`, `TaskError` and the `Tensor` type family. Tensor element positions offer
only numeric names. Array element positions exclude `TaskError` and nested
`Tensor` families under the current array-type rules. `Tensor` remains a plain
name item: users still supply its element and shape arguments. No snippets,
auto-imports or new type syntax are introduced.

A request-local probe must occupy a parsed annotation node. The checker collects
only names from that node's module/type namespace; normal type checking still
runs against the temporary probe. Only names escape; no partial type/module is published, and
normal diagnostics, hover and files remain unchanged. Prior semantic/import errors
can suppress suggestions. As with scope completion, later errors do not invalidate
an already captured list, and a suggested declaration is not a promise that every
use satisfies layout or contextual restrictions.

Type discovery walks owned annotation syntax, including callable parameter/result
types and annotations in nested suites; it does not infer context from punctuation
alone. Ordinary expression requests skip this additional walk. Prefix filtering,
sorted/deduplicated names, the 256-item bound, source/graph budgets and UTF-16 edit
ranges remain shared with existing completion. Other declaration syntax must
parse; general declaration recovery remains separate. Expected-error channels are
covered below.

[Type-completion qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-type-completion-2026-10-21.json)
covers normal/sanitizer semantic and protocol checks, live imported aliases,
context-specific builtin lists, bounded results, relocated Release tools and
installed VS Code edits followed by checked hover and cleared diagnostics.

#### Error-channel completion

An expected-error slot now offers only declared `error` families, transparent
error aliases and `TaskError`. This works after `!` in named and anonymous
functions, inside parenthesized channel unions, in nested callable signatures,
and on the right-hand side of an error-alias declaration such as
`type Failure = ParseError | ReadError`.

Imported families and aliases use their local names. For example, importing
`Problem as Failure` offers `Failure` in the client without exposing a library's
private names. Ordinary records/enums, numeric types, functions and local value
bindings are not error-channel candidates. Same-spelled values do not hide a
family in the type namespace. Aliases on the resolver's current active path are
omitted, including an alias in its own definition. The checker still validates
cycles and all union semantics after the user edits; this is not a cycle solver.
Already listed members can remain candidates because canonical duplicate members
are legal under the existing error-union rules.

The existing parsed annotation walk now also locates explicit error-member spans.
The error resolver captures a bounded, prefix-filtered Type-kind list before
normal resolution of the request-local probe. Only names escape. No error
channel, partial typed module, union expansion or source edit is invented for
the user. Ordinary diagnostics and hover remain based on the actual source.
Other declaration syntax must parse; incomplete declarations, import-path
completion for bare package names, auto-import and snippets remain separate.

[Error-channel qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-error-completion-2026-10-21.json)
covers empty/partial members, unions and aliases, named/anonymous/nested callable
channels, active-alias exclusion, list bounds, imported overlays, relocated
Release tools and installed VS Code edits followed by checked diagnostics/hover.

#### Import-name completion

After `from "./math.k" import`, completion offers the target module's public
functions, records, enums, error families and transparent error aliases. An empty
slot or a partial identifier works, including later comma-separated slots and
`export from` declarations. Explicit re-exports use their published names;
ordinary imports and private declarations are not exposed. Existing local
`as` aliases remain untouched by the edit.

The module graph uses current open dependency buffers and configured package
mappings. Candidates come from registered export headers, with re-exports
resolved through the compiler's existing export resolver. This provides names
and Function/Type item kinds, not a guarantee that a declaration's body or
signature is valid. Later checking still reports missing exports and invalid
programs normally. Unresolvable re-export names are omitted. Previously imported
names remain available because another explicit local alias can be valid.

The request replaces the whole current identifier, preserves its surrounding
import syntax, and applies the existing case-sensitive prefix, alphabetical
ordering and 256-item bound. It does not change source buffers, diagnostic
publications or disk files. Bare package paths and local alias declarations are not
completion sites; relative path completion is covered below. Missing modules and
syntax that the temporary graph cannot parse produce no candidates. There is no automatic import insertion or general
parser recovery.

[Import-completion qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-import-completion-2026-10-21.json)
covers public/private names, aliases and re-exports, list bounds, live dependency
buffers, unchanged diagnostics/files, relocated Release tools and an installed
VS Code completion followed by checked diagnostics and hover.

#### Relative import-path completion

Inside a closed import string, typing a relative prefix such as `./ma` or
`../math/` offers matching `.k` files and child folders. This works in ordinary
imports and `export from` declarations. The edit replaces the string contents,
including any suffix after the cursor, while preserving the quotes, imported
names and local aliases. Files use File completion items; folders use Folder
items and end with `/`. The server advertises `/` as a completion trigger.

Discovery inspects only the directory named by the prefix, resolved from the
importing file's canonical location. It does not recursively search, read candidate
source contents or load their modules. Existing relative-path semantics still
apply, including explicit parent-directory paths; this does not introduce a new
workspace confinement rule. Symlink entries, hidden names, the current source
file, non-`.k` files and names requiring string escapes are omitted. An explicitly
typed parent directory follows the same filesystem canonicalization as imports.
Current open `.k` buffers in that directory participate even when they have not
been saved, including directories that do not exist yet. File and overlay
candidates are deduplicated.

Results use a case-sensitive prefix, alphabetical order and the existing 256-item
limit. A directory traversal examines at most 20,000 entries; exceeding that
budget returns an empty incomplete list rather than an arbitrary partial result.
Filesystem errors produce no candidates. Overlay counts retain the module file
budget. Filename suggestions do not establish that a module exports the requested
names or even parses: ordinary checking validates the user's actual edit.

Only parsed import declarations with closed, unescaped strings participate.
Ordinary strings, comments, bare package names, unterminated strings and unrelated
incomplete syntax do not gain recovery. Queries do not change buffers, diagnostic
publications or disk files, and perform no network or device work.

[Relative-path qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-import-path-completion-2026-10-21.json)
covers filesystem/unsaved candidates, Unicode UTF-16 edits, child/parent paths,
exclusions and bounds, relocated Release tools and an installed VS Code path edit
followed by checked diagnostics and hover.

#### Checked collection builtin signatures

Signature help now specializes `length`, `byteLength`, `shape`, `slice` and
`concat` from a fully checked call. For example, `slice(values, 0, 2)` can show
`fn slice(input: [int32], start: int64, end: int64) -> [int32]` when `values` is
an int32 array. String slicing shows String input/result types; `shape` shows
the tensor's checked element type and extents. `concat` supports arrays, including
Bool, String and nominal arrays; scalar String concatenation continues to use `+`.

The compiler's operation at the exact invocation span supplies argument and
result types. Guidance is not inferred from the callee's spelling: a local or
module callable named `length` still uses ordinary callable resolution. The
existing type printer retains callable/nominal context and hides private dynamic
extent identities. Conflicting lowered signatures at the same source location
produce no hint.

Parameter labels are `input`, `start`, `end`, `left` and `right` as applicable.
Active-parameter selection, nested delimiters, pipeline-supplied arguments and
UTF-16 parameter-label ranges use the existing signature protocol. A final
identifier that already checks remains an actual supplied value. Existing bounded
trailing-parenthesis recovery can also provide these hints when all arguments
are present in its request-local graph.

Complete-call specialization requires a checked argument list and a valid graph.
Missing/unknown/wrongly typed arguments, missing field probes and unrelated graph
errors do not produce a guessed generic signature through that path. The bounded
missing-right-operand path for `concat` is covered below. Other builtin families,
remaining missing-argument builtin guidance and general expression recovery remain separate.
Queries do not execute functions, publish diagnostics, change buffers or write
files. Unsaved imported result types are rechecked for each request.

[Collection-signature qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-collection-signature-2026-10-21.json)
covers normal/sanitizer signature and hover regressions, protocol parameter
ranges and pipeline positions, live dependency types, relocated Release tools
and installed VS Code guidance without source changes.

#### Missing concat operand guidance

When writing `concat(values, …)`, signature help can derive the expected second
array from the checked first operand. For `values: [int32]`, it displays
`fn concat(left: [int32], right: [int32]) -> [int32]` with parameter one active.
The same rule covers `values -> concat(…)`. An expected array result type can
supply the element context for an empty first literal; without that context,
`concat([], …)` cannot establish an element type and provides no hint.

This uses the checker's existing request-local signature snapshot. It checks the
first operand and captures the array element contract before the normal arity
error. Numeric, Bool, String, nominal and callable arrays retain their ordinary
type representations. The second operand and result have unknown lengths:
knowing the left array does not establish the right or concatenated lengths.
No missing operand value, conversion or executable typed module is invented.

Empty second slots, final partly typed identifiers, and final partly typed fields
use the existing editing-hole policy. A field receiver must check as an ordinary
record before guidance is captured. Bounded trailing-parenthesis recovery can
supply this help at the end of an unfinished buffer. Prior errors, invalid first
operands, missing element context, extra arguments and unsupported compound holes
suppress the snapshot. Same-spelled user callables retain ordinary signature
resolution. As with other snapshots, later errors can follow captured guidance;
a hint is not evidence that the whole program checks.

The request preserves ordinary checking and its actual missing-delimiter,
wrong-arity or unknown-name diagnostic. It does not publish probe diagnostics,
write sources, change buffers or execute code. Current unsaved imported types
participate; a changed field contract invalidates an incompatible first array.

[Missing-concat qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-concat-signature-2026-10-21.json)
covers normal/sanitizer signature, hover and ordinary concat frontend checks,
protocol diagnostics and imported buffers, relocated Release tools and installed
VS Code guidance while retaining the unfinished source and its syntax diagnostic.

#### Builtin call-target completion

Partly typed call targets now include the compiler's collection and numeric call
forms. For example, `coSuffix([1], [2])` with the cursor after `co` offers `concat`,
and `floSuffix(1)` with the cursor after `flo` offers `float`, `float32` and
`float64`. The replacement covers the entire current identifier. Direct calls,
piped calls and bare pipeline targets such as `values -> length` share this rule.

The initial names are `array`, `tensor`, `reshape`, `map`, `parallelMap`, `matmul`,
`reduceSum`, `length`, `byteLength`, `shape`, `slice` and `concat`, plus all ten
concrete numeric conversion names and the `int`, `uint`, `float` aliases. Numeric
conversion names come from the same numeric description table used by type
resolution. They use Function completion items at call targets; numeric annotation
completion keeps its existing Type items.

Builtin call forms are not first-class function values, so ordinary expression
positions and callback-value arguments do not receive these additional names.
Lexical/module bindings are collected first and retain their own item kind and
priority when they share a builtin name. The checker decides whether the query is
a call target; completion does not guess that a bare expression will become a
call or append parentheses and arguments.

These are name suggestions rather than overload, expected-type or device-capability
filtering. An offered builtin still needs its ordinary valid arguments, types and
execution context. Placement-specific transfer-call names and device-selector member
completion are covered below. The existing alphabetical prefix filtering, 256-name limit, current
source overlays and diagnostic/source preservation rules apply. Prior diagnostics
can suppress names; later errors do not erase an already captured scope.

[Builtin-completion qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-builtin-completion-2026-10-21.json)
covers direct/piped names, numeric conversions, shadowing and excluded value
contexts in normal/sanitizer builds, relocated Release tools and installed
VS Code edits followed by checked collection and conversion calls.

#### Checked numeric conversion signatures

Signature help now shows the checked source and destination of each numeric
conversion. For example, `float32(1)` displays
`fn float32(input: int64) -> float32`. All ten concrete numeric type-name calls
participate, including identity conversions. The `int`, `uint` and `float` aliases
keep their source spelling in the function name and show their concrete result
widths. This extends editor guidance; it does not change numeric conversion rules.

CPU tensor conversions retain their rank and fixed/dynamic extents in both
parameter and result labels. For example, converting a `Tensor[int64, 1, 2]` to
float32 displays `fn float32(input: Tensor[int64, 1, 2]) -> Tensor[float32, 1, 2]`.
Dynamic extent identities remain hidden as `?`. Numeric arrays remain distinct
from tensors and do not acquire direct conversion support.

The checked `NumericConvert` operation at the exact call span provides the input
and result types; the named destination must agree with the operation. A local
function value named `float32` still uses ordinary callable guidance. Existing
pipeline positions, UTF-16 parameter ranges and bounded trailing-parenthesis
recovery apply. Current unsaved dependency result types are checked afresh.

A complete valid argument list and checked graph are required. Known-invalid
constant conversions, invalid source types, missing/extra arguments, editing
holes and unrelated errors do not supply a guessed signature. A displayed type
signature does not promise that every runtime value fits the destination: the
existing checked numeric conversion rules still determine success or faults.
Requests execute no conversions and change no buffers, diagnostics or disk files.

[Numeric-signature qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-numeric-signature-2026-10-21.json)
covers signature labels for all 100 scalar type pairs using representable inputs,
aliases, fixed/dynamic tensor shapes, invalid casts and lexical shadows, live
imported types, relocated Release tools and installed VS Code tensor guidance.
It qualifies editor behavior, rather than repeating the numerical/runtime backend
qualification for each conversion pair.

#### Checked tensor operation signatures

Signature help now covers `reduceSum`, `matmul` and tensor-to-array `array` calls.
The checked tensor operand supplies its numeric element type, rank and extents.
A reduction displays a scalar result of the same element type; `array` displays
the corresponding flat numeric array type. Rank-zero and empty tensors use the
same printing rules as other checked tensor values.

Matrix signatures show both operands and the derived output shape. For example,
a `Tensor[int64, 1, 2]` multiplied by a `Tensor[int64, 2, 1]` displays
`fn matmul(left: Tensor[int64, 1, 2], right: Tensor[int64, 2, 1]) -> Tensor[int64, 1, 1]`.
Dynamic extents print as `?`, retaining known axes without exposing inference
identities. The ordinary matrix element-type, inner-dimension and runtime shape
checks still apply; a displayed `?` does not waive them.

The shared builtin-signature path recognizes the actual checked `ReduceSum`,
`Matmul` or `TensorToArray` operation at the selected call span. It uses `input`
or `left`/`right` parameter labels. Direct and piped calls retain the existing
active-parameter and UTF-16 label-range rules. A same-spelled local callable uses
ordinary callable guidance. Unsaved imported tensor shapes update both operand
and output labels, while incompatible shapes suppress the hint.

Complete arguments and a valid checked graph are required. Invalid ranks/types,
known incompatible matrix dimensions, missing operands and other graph errors
provide no guessed signature. Existing bounded closing-parenthesis recovery can
supply guidance when all arguments are present. Tensor construction and reshape
signature help remain separate; map-family guidance is covered below, and this does not expand GPU admission,
change numerical semantics or execute tensor operations during editor requests.
Buffers, disk sources and diagnostic publications remain unchanged.

[Tensor-signature qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-tensor-signature-2026-10-21.json)
covers all ten CPU element types for the three signature families, fixed/dynamic
matrix shapes, rank-zero/empty tensors, invalid inputs, live imported shape changes,
relocated Release tools and installed VS Code guidance for a piped matrix call.
These are editor checks; they do not repeat numerical/runtime backend qualification.

#### Checked map callback signatures

Signature help now covers `map` and `parallelMap` with checked input-array,
callback and output types. For example, `map([1, 2], fn(value): value > 0)` shows
`fn map(input: [int64], callback: fn(int64) -> Bool) -> [Bool]`.
Named functions, inline anonymous functions, captured function values and indexed
callbacks use their public one-element callback contract. Captured data and the
internal retained callable used by a function-value wrapper are not parameters
in the displayed callback.

The checked `ArrayMap` operation identifies the input and callback entry. The
entry's first parameter supplies the element type; its checked declared/inferred
result supplies the callback result type. Remaining entry parameters must match
the operation's retained captures and are omitted. This reuses the existing
bounded type printer and module context for numeric, Bool, String, nominal and
callable values. Unsaved imported callback types update the result-array label.

A `parallelMap` hint includes the outer error channel. An infallible callback
can show `fn parallelMap(input: [int64], callback: fn(int64) -> int64) -> [int64] ! TaskError`.
When a callback declares an application error, its own callable type retains that
channel, while the outer array result includes the application channel and
`TaskError`. Scheduling errors are not added to an otherwise infallible callback
signature. Ordinary `map` keeps its existing completed-element callback rules.

Direct and piped calls use the same parameter labels and UTF-16 range protocol;
a piped array selects the explicit callback parameter. Complete valid arguments
and a checked graph are required. Missing/incompatible callbacks, invalid callback
arity, unhandled error channels and unsupported worker captures yield no hint.
Existing bounded closing-parenthesis recovery can apply when the callback is
complete. Same-spelled local callables retain ordinary guidance. Requests execute
no callbacks, schedule no tasks and change no source or diagnostic publications.

[Map-signature qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-map-signature-2026-10-21.json)
covers all ten numeric widths, changed result elements, nominal inputs,
named/inline/captured/indexed callbacks, public error channels and invalid calls,
current imported callback changes, relocated Release tools and installed VS Code
parallel-map guidance with captures hidden. It adds editor coverage without
changing map scheduling, error propagation or GPU/worker admission.

#### Checked builtin hovers

Hovering a supported builtin's name at a valid call now shows its checked,
call-specific signature. This reuses the signature-help types for collection
queries/slicing/concat, numeric conversions, reduction/matrix/array operations,
and `map`/`parallelMap`. For example, hovering `float32` in `float32(1)` shows
`fn float32(input: int64) -> float32`. Parallel-map hovers retain the public
callback and outer error channel while keeping captures and wrapper arguments
private.

Direct calls, piped calls and bare pipeline targets such as `values -> length`
use their actual checked operation spans. The returned range covers only the
builtin identifier in its original source file, including UTF-16 positions after
non-ASCII text. Plain-text and Markdown presentation follow the existing client
preference and formatting limits. Current unsaved dependency types participate.

A local or module callable with the same spelling retains ordinary semantic
hover; function-valued record fields and annotations retain their existing paths.
The builtin path requires a fully checked graph. Invalid/incomplete calls,
comments, punctuation and unsupported builtin families produce no builtin hover.
Missing-argument signature snapshots are not reused as hover types. Builtin hovers
describe the call; declaration navigation remains based on actual source definitions.
Requests do not execute operations, change buffers or files, or publish diagnostics.

[Builtin-hover qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-builtin-hover-2026-10-21.json)
covers normal/sanitizer hover and signature regressions, plaintext/Markdown
protocol results, Unicode token ranges, direct/piped calls, shadows, current
imported buffers and failed graphs, relocated Release tools and installed VS Code
hovers for a captured parallel map and numeric conversion.

#### Device-selector member completion

With placement checking enabled, completing the member in `devices.gSuffix(1)`
with the cursor after `g` offers `gpu` as a Function item. The edit replaces only
`gSuffix`, preserving `devices.`, parentheses and the explicit index. An empty
member after `devices.` offers `cpu` and `gpu`; that explicit namespace prefix can
also be completed before parentheses have been written. Accepting an item inserts
only its name, so a selector still requires an ordinary call such as
`devices.cpu()` or `devices.gpu(1)`.

The parser represents selectors as qualified names. Completion locates the member
within that name, and the checker captures the namespace candidates in placement
mode. A `devices` binding/parameter in the current checker frame suppresses the
list, following the selector's existing shadow check. Placement-disabled queries,
arbitrary receivers, and prior errors provide no device-selector names. The
existing prefix filtering, whole-member suffix edit, source budgets and diagnostic
preservation rules apply. Later errors do not erase an already captured list.

These suggestions name selector forms, not available hardware. No device enumeration,
Metal initialization, dispatch or index-availability check occurs. Actual selector
argument validation and runtime device selection retain their existing behavior.
Current imported graphs use original-file identities; requests leave buffers,
disk sources and diagnostic publications unchanged.

[Device-completion qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-device-completion-2026-10-21.json)
covers placement-mode gating, empty/partial members, explicit indices, local and
parameter shadows, imported source spans and ordinary diagnostics in debug and
sanitizer builds. Relocated Release tools and installed VS Code also exercise the
placement-setting restart and a member-only edit with its index preserved.

#### Checked transfer guidance

With placement checking enabled, hover and signature help on a valid `transfer`
call show the checked input tensor, exact target device, and returned task. For
example, transferring an `int64` tensor of length two to `devices.gpu(1)` shows:

```text
fn transfer(input: Tensor[int64, 2], target: Device[gpu:1]) -> task<Tensor[int64, 2] on gpu:1>
```

The return transfer to CPU memory shows the GPU residency on its input and a
CPU tensor in its task result. Dynamic axes remain `?`, without private extent
identities. Direct and piped calls share the checked operation; in a piped call,
the explicit device argument selects the second parameter. Hover covers only
the builtin name. These are call-specific descriptions, not first-class function
declarations or new annotation syntax.

Guidance requires a fully checked graph and the existing owning parallel group.
Invalid targets, unhandled tasks and unrelated graph errors provide no checked
builtin hint. Placement-disabled programs retain ordinary source-function
resolution for a function named `transfer`. Missing operands do not receive an
invented transfer contract. No hardware enumeration, Metal initialization, source
changes or diagnostic publications occur during these requests. Frontend types
and selector indices describe the source contract; they do not expand GPU backend
element-type admission or establish that a device exists.

[Transfer-guidance qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-transfer-guidance-2026-10-21.json)
covers checked GPU-bound and CPU-bound transfers, exact indices, fixed/dynamic
shapes, direct/piped calls, invalid graphs and placement gating, with debug and
sanitizer suites, relocated Release tools and installed VS Code.

#### Transfer call-target completion

With placement checking enabled, a partly typed call such as
`try await traSuffix(values, devices.gpu(1))` offers `transfer` inside an owning
`parallel` group. The Function item replaces the whole identifier, preserving
arguments, delimiters and the explicit device index. Direct and piped calls,
including bare pipeline targets, use the same checker scope. Bounded existing
end-of-buffer parenthesis recovery can also retain a final pipeline target such
as `try await (values -> tra`. A name before an unfinished argument list also
uses the [call-target EOF repair](#call-target-completion-before-missing-parentheses) below.

The added builtin candidate requires the current checker frame to own a parallel
group and the call to be an ordinary invocation. It is excluded in `spawn` entry
positions, with or without explicit placement, and in first-class value/callback
positions. Placement-disabled code receives no added transfer builtin. Ordinary
lexical/module names retain their existing kinds and priority; a CPU-mode user
function named `transfer` remains an ordinary candidate.

This is name completion, not a claim that the operands are valid or that a device
exists. Accepting the item still requires ordinary tensor, device, task and error
checks. Prefix filtering, whole-name suffix edits, source budgets, current imported
overlays and diagnostic preservation follow the existing completion contract.
Prior errors suppress candidates; later errors do not erase a captured scope.
Requests neither enumerate hardware nor execute transfers or change source files.

[Transfer-completion qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-transfer-completion-2026-10-21.json)
covers call/value/spawn contexts, placement/group gating, local name priority,
pipeline and unfinished-call targets, source edits and live imported buffers in
debug and sanitizer builds. Relocated Release tools and installed VS Code also
apply the identifier-only edit and check the resulting program.

#### Checked device-selector guidance

With placement checking enabled, hover and signature help describe the checked
selector call and its exact device result:

```text
fn devices.cpu() -> Device[cpu]
fn devices.gpu() -> Device[gpu:0]
fn devices.gpu(index: uint32) -> Device[gpu:1]
```

The third example describes `devices.gpu(1)`. The no-argument GPU form selects
index zero; an explicit argument must still be a constant unsigned integer
literal within the uint32 range. Variables, conversions and computed indices
remain invalid. The displayed parameter is the selector's checked literal
contract, rather than the ordinary numeric type inferred for a standalone
literal. These call-specific descriptions do not introduce optional/default
parameter syntax or first-class selector functions. A zero-argument call has no
active parameter; the explicit-index call has one `index` parameter.

Guidance comes from an actual checked `DeviceSelection` operation and the source
call arity. Piped calls account for their implicit first argument, including a
bare pipeline target in hover. Hover covers the full qualified selector name in
its original source file. Current imports participate in checking, and protocol
responses preserve their original-file ranges and parameter label offsets.

The complete graph must check. Placement-disabled code, a shadowed `devices`
namespace, invalid indices/argument counts and unrelated errors provide no
checked selector guidance. Missing or incomplete selectors do not receive a
guessed signature. Requests do not change source buffers, write files, publish
diagnostics, initialize Metal or enumerate devices. An exact index describes the
source selection, not proof that such a GPU exists.

[Selector-guidance qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-selector-guidance-2026-10-21.json)
covers CPU/default-GPU/explicit-index forms, the maximum uint32 index, pipelines,
invalid calls, placement gating and checked imported source identity. Debug and
sanitizer tests, relocated Release tools and installed VS Code verify the hover
and argument hints without GPU execution.

#### Call-target completion before missing parentheses

Completion can now replace a partly typed call name before its argument list has
all closing parentheses. For example, with an imported function named `calculate`,
`calSuffix(1,2` with the cursor after `cal` offers `calculate`. The edit replaces
only `calSuffix`; both arguments and the actual missing-delimiter diagnostic
remain. The user still closes the call. Named functions, builtin call forms,
pipeline calls, checked record members and placement selectors/transfer calls
retain their existing scope, receiver and placement rules.

For this call-target-only case, the temporary source appends at most eight closing
parentheses at EOF. It preserves all original text after the selected name instead
of closing the call at the cursor or removing arguments. The repaired graph must
parse. Call targets require the probe to be exactly the call's name or member;
the [argument extensions](#earlier-argument-completion) below now also permit
supplied argument names/members and [supported arithmetic operands](#arithmetic-and-comparison-argument-completion). Signature help uses the corresponding
[final-argument snapshot policy](#signature-help-with-final-argument-suffixes) below.

Unclosed arrays/records, mismatched delimiters, other lexical errors, malformed
arguments, remaining syntax errors and repairs beyond the eight-parenthesis or
source-size budget are rejected. Invalid record receivers and prior semantic
errors still suppress candidates. Name suggestions do not establish that the
existing arguments satisfy a candidate's parameter types: ordinary checking
validates them when the actual call parses. Current unsaved imported declarations
participate without changing disk sources.

Requests publish neither probe diagnostics nor partial typed modules. They do not
change buffers/files, add real closing parentheses, or provide hover for unfinished
source. Original-file replacement ranges and prefix filtering remain unchanged.

[Open-callee qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-open-callee-completion-2026-10-21.json)
covers direct/nested/piped/qualified/member call names, preserved arguments,
recovery exclusions and bounds, imported overlays, ordinary delimiter diagnostics,
relocated Release tools and an applied completion in installed VS Code.

#### Final-argument suffix completion

EOF-only parenthesis repair now also permits a partly typed simple final argument
when its identifier suffix follows the cursor. For example, `combine(1, vaSuffix`
with the cursor after `va` can offer a lexical `value` binding. A checked record
argument such as `combine(1, reading.vaSuffix` can offer the receiver's `value`
field. The edit covers the complete identifier and leaves receiver text, supplied
arguments and the unfinished call intact. Grouped names and checked function-value
callees use the same path. A supplied first argument can receive completion even
when later parameters have not yet been written.

After temporary parentheses are appended at EOF, the probe must be exactly a
parsed call target or a simple name/member serving as the call's final argument.
Earlier simple argument slots are covered by the
[extension below](#earlier-argument-completion), and supported arithmetic/comparison
operands by the [expression extension](#arithmetic-and-comparison-argument-completion). No
source suffix is discarded or replaced with closing parentheses at the cursor.
The existing eight-parenthesis, lexical-error, delimiter and source-size bounds
still apply; the entire temporary graph must parse.

Ordinary semantic analysis checks the callee and earlier supplied arguments
before reaching the final argument's completion query. Unknown/noncallable
callees, invalid earlier types and unknown record receivers suppress suggestions.
Record member candidates follow current imported types, including unsaved edits.
Lexical names retain the existing policy: an offered name is not a guarantee
that its value satisfies the expected parameter type.

Accepting a suggestion changes only the selected name. Actual unclosed-delimiter
diagnostics remain until the user closes the call; hover still requires valid
source. [Signature help](#signature-help-with-final-argument-suffixes) now has a matching
EOF-only final-name/member path. These requests publish no probe diagnostics,
partial typed modules or file writes.

[Argument-suffix qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-argument-suffix-completion-2026-10-21.json)
covers lexical/member suffixes, grouped and function-value calls, prefix/receiver
checks, imported overlays, compound/non-final exclusions, source preservation and
actual diagnostics in debug/sanitizer and relocated Release tools. Installed VS
Code applies a field-only edit, retains the delimiter diagnostic, and checks the
call after explicit closure.

#### Signature help with final-argument suffixes

Parameter hints now remain available when the cursor is inside a simple final
argument name/member and its call is missing closing parentheses. For example,
`combine(1, vaSuffix` with the cursor after `va` shows the checked `combine`
parameters and selects the second parameter. `combine(1, reading.vaSuffix` does
the same after checking the record receiver. Bound, factory-returned and indexed
callables, pipelines and the existing missing-operand `concat` snapshot retain
their checked public signatures.

The request appends at most eight closing parentheses at EOF in its temporary
root overlay, preserving all original suffix text. This fallback requires the
existing final-name/member projection: it removes only that final argument from
the request-local AST and checks the callee and earlier arguments before capturing
a signature. A field projection also checks its completed record receiver. It
cannot bypass this projection by returning a full checked builtin signature from
the repaired source. No missing argument value or executable partial module is
constructed. The original end-of-buffer recovery path remains available.

Unknown/noncallable callees, invalid earlier arguments, invalid record receivers,
compound expressions provide no new hint. Earlier simple slots now use the
[checked prefix snapshot](#earlier-argument-signature-snapshots) below. Existing
lexical-error, delimiter, source-size and eight-parenthesis limits remain. The
entire temporary graph must parse. Builtins without a validated missing-argument
snapshot do not acquire a guessed parameter contract through this extension.

The active argument and parameter labels follow the actual callee, including
current unsaved imported declarations and implicit pipeline arguments. Returned
labels describe expected parameters rather than the type of the unfinished value.
Actual source diagnostics, hover, buffers and files remain unchanged; requests
publish no probe diagnostics.

[Suffix-signature qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-suffix-signature-2026-10-21.json)
covers checked named/structural/factory/indexed callable signatures, fields,
pipelines, concat snapshots and rejection boundaries in debug/sanitizer builds.
Protocol tests follow live imported parameter changes and label offsets. Relocated
Release tools and installed VS Code retain the real missing-delimiter diagnostic
while showing the checked active parameter without editing source.

#### Earlier-argument completion

Simple argument name/member completion now works before later supplied arguments
in an unfinished call. For example, with a lexical `value` binding,
`triple(vaSuffix, 2` with the cursor after `va` can offer `value`, even if `triple`
requires a third argument. `triple(reading.vaSuffix, 2` can similarly offer a
checked receiver's field. The edit changes only the whole selected identifier;
later supplied arguments stay in place.

EOF-only parenthesis repair now accepts a parsed simple name/member at any
supplied argument position, not just the final one. The complete temporary graph
must parse, and the existing lexical-error, delimiter, source-size and
eight-parenthesis bounds remain. Supported [arithmetic/comparison operands](#arithmetic-and-comparison-argument-completion)
now also use this repair; other compound forms remain separate. Signature help now also has [earlier-argument snapshots](#earlier-argument-signature-snapshots),
with its own checked prefix policy.

For named and function-value calls with missing later required parameters, a
completion request can visit an earlier argument with its expected parameter
type. The ordinary checker validates the callee and supplied arguments preceding
the edit, including implicit pipeline arguments. Unknown/noncallable callees, bad
earlier types and unknown member receivers suppress candidates. Bound/indexed
callables and current imported declarations use the same checks. Builtin forms
retain their own argument admission; this does not invent missing builtin inputs.

Later argument errors do not erase already captured names. Suggestions describe
available names, not proof that all arguments check. No argument or real closing
parenthesis is inserted. A closed call missing a required parameter still reports
its ordinary `S027`; supplying the actual final argument is necessary to check.
Requests preserve buffers/files and actual diagnostic publications, and never
return executable partial typed modules.

[Earlier-argument qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-earlier-argument-completion-2026-10-21.json)
covers direct/bound/indexed/piped calls, balanced/open calls with missing later
parameters, member receivers, prior/later errors and live imported field changes.
Debug/sanitizer, relocated Release tools and installed VS Code preserve both real
delimiter and missing-argument diagnostics while applying identifier-only edits.

#### Earlier-argument signature snapshots

Parameter hints now support simple name/member edits before later supplied
arguments, both in balanced calls and in calls needing bounded EOF parenthesis
repair. For example, `triple(vaSuffix, 2` with the cursor after `va` shows all
three checked parameters and selects the first, even before the third argument
is written. `triple(reading.vaSuffix, 2` additionally checks the completed record
receiver. Named, bound, factory-returned and indexed callables retain their public
signatures; pipelines count the implicit first argument when selecting the slot.

For an earlier slot, the request keeps the edited and later arguments in its AST.
The checker validates the callee and arguments preceding the selected slot, then
checks any projected field receiver and captures the expected signature. It ends
that query path with an internal failure at the call site, so no argument value or
executable partial typed module is constructed. That internal diagnostic is never
published to the editor. Ordinary checking runs separately on the actual source
and retains its real unknown-field/name, type, delimiter and arity diagnostics.
The existing final-argument projection path remains available.

Hints describe the expected parameters, not proof that the edited or later values
check. Later argument errors do not prevent a prefix-only signature snapshot.
Invalid callees, bad earlier types, invalid record receivers, extra arguments,
compound editing holes and unrelated reported graph errors suppress the new hint.
Builtin families keep their individual checked/missing-argument admission; this
does not invent an element or device contract. Empty earlier field slots are
covered below; general parser recovery remains separate work.

Current imported buffers update parameter/result types and error channels. Source
text, later argument spans, buffers/files and real diagnostics remain unchanged.
The existing lexical, source-size and eight-parenthesis repair bounds still apply.

[Earlier-signature qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-earlier-signature-2026-10-21.json)
covers first/middle slots, missing later parameters, named/structural/indexed/piped
callees, checked field receivers, expected-error channels and invalid prefixes.
Direct checker tests verify snapshot-only failure and unchanged ordinary unknown
argument diagnostics. Protocol, relocated Release and installed VS Code checks
verify live imported parameters, label offsets, active slots and source/diagnostic
preservation in balanced and open calls.

#### Empty earlier-field signature slots

An empty field before a comma now uses the same checked signature projection as
a nonempty earlier field. In `combine(reading., 2)`, placing the cursor immediately
after the dot shows the checked `combine` parameters and selects the first. It
also works in middle slots, indexed callable invocations and pipelines, with the
implicit first argument counted normally. A call with later required parameters
not yet written can still show its complete expected signature.

The request inserts one private field identifier into its temporary overlay only
when the actual source does not parse. A following comma is preserved along with
all later source. The parsed field must take the mandatory checked-receiver
projection; it cannot be evaluated as an actual argument, including when the
record happens to contain a field with the private probe's name. The callee,
earlier supplied arguments and completed record receiver must still check.

For missing closing parentheses, EOF-only repair preserves the comma and later
arguments. Existing source-size, lexical-error, delimiter and eight-parenthesis
bounds apply. Unknown/non-record receivers, invalid argument prefixes, existing
identifier suffixes, compound holes, malformed later syntax and unclosed arrays
are not bypassed. No probe name appears in returned signatures, source buffers,
files, ordinary diagnostics or hover.

Actual parser diagnostics remain until the user supplies a real field. The
request does not choose a field, claim a field's type, or return an executable
partial module. Current imported parameter types still update the expected
signature without changing disk files.

[Empty-earlier-field qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-empty-earlier-field-2026-10-21.json)
covers first/middle slots, pipelines, indexed callables, EOF repair, missing later
parameters, probe-name collision, invalid prefixes/receivers and malformed suffixes
in debug/sanitizer tests. Protocol, relocated Release and installed VS Code
checks preserve actual diagnostics/source and verify current imported signatures.

#### Arithmetic and comparison argument completion

Name/member completion now works inside supported arithmetic or comparison
arguments when the call needs EOF parenthesis repair. Examples include
`combine(1, 2 + vaSuffix`, `combine(1, -vaSuffix` and
`choose(1 < vaSuffix`, with the cursor after `va`. Checked record fields work
inside those operands too, as in `combine(1, 2 * reading.vaSuffix`. The edit
replaces only the selected identifier and preserves operators, receiver text,
later operands and supplied arguments. Earlier argument slots and missing later
parameters retain their existing callable checks.

The request follows only the expression path containing the selected name/member,
through numeric unary `+`/`-`, arithmetic `+`, `-`, `*`, `/`, `%`, and comparison
nodes. That recognition path is limited to 64 expression nodes. It does not add
logical/conditional/match bodies, indexing holes or arbitrary parser recovery.
The temporary graph still has to parse after at most eight parentheses are
appended at EOF; existing lexical, delimiter and source-size limits apply.

Ordinary semantic analysis still determines when the query can be reached,
including callee/preceding-call-argument checks and record receiver validation.
Operand checking follows its existing type-inference order. Suggestions identify
scoped names/members rather than proving that all operators, operand types or
later expressions are valid. Current unsaved imported receiver types participate.
No guessed operand value, changed operator or executable partial module is used.

Actual unclosed-delimiter diagnostics and absent hover remain until the source
is completed. Signature help still excludes compound editing holes; this
extension concerns completion only. No source buffers/files are written or probe
diagnostics published.

[Expression-completion qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-expression-completion-2026-10-21.json)
covers all five arithmetic operators, unary/nested/left operands, all six
comparisons, checked record members, earlier slots, invalid call prefixes and
malformed expressions. Protocol, relocated Release and installed VS Code checks
preserve expression text and actual diagnostics while applying identifier-only
edits and following live imported field names.

## Structural unfinished-code recovery

The [N4 completion contract](native-tooling-completion.md) extends the earlier
EOF-only repairs to request-local structural recovery, compound signature slots,
collection/index/control/closure holes and independent broken function bodies.
Checked expression scope completion includes native keyword suggestions. Real
diagnostics, source buffers, original replacement ranges, required semantic
prefixes and the ban on executable partial IR remain in force. The linked
checklist and qualification record define the completed scope and its limits.
