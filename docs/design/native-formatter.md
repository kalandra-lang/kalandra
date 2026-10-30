# Native source formatter

`kalandra-fmt` and the native language server share a C++ formatter for `.k` source. Formatting uses the native lexer and parser, independently of imports, project setup, type checking, backends or installed hardware.

## Command line

```sh
cmake --build build/native --target kalandra-fmt
build/native/native/kalandra-fmt path/to/main.k
build/native/native/kalandra-fmt --check path/to/main.k
```

The default prints formatted source to stdout. `--check` prints nothing and returns 0 for an already formatted file or 1 when changes are needed. Read/syntax/verification failures return 1, with diagnostics on stderr and no formatted output. Invalid arguments return 2. Both modes leave the source unchanged. The initial command accepts one file; writing in place, stdin, recursive discovery and configurable style are not implemented.

## Style and preservation

The fixed style uses two spaces per block level, compact calls/indexing, spaces after commas and colons, spaces around binary operators, compact unary signs, and spaces inside nonempty record braces. Existing physical statement/expression line breaks and interior blank lines remain. Continuation indentation is normalized conservatively. Code trailing whitespace and blank-line whitespace are removed; line endings become LF, and nonempty output ends with one newline. Empty/whitespace-only files become empty, and comment-only buffers are supported even though compiling a complete source unit requires a declaration.

Comments keep their complete text, order and physical line relative to code, including intentional trailing spaces. Inline comments receive two separating spaces; comment indentation follows the enclosing known layout where possible. String literals, escape spellings, numeric spellings, punctuation, parentheses, declaration order and imports are preserved. The formatter does not wrap long lines, add/remove parentheses, rename identifiers or rewrite expressions.

Before returning output, the formatter lexes and parses the candidate again. Every non-layout token must retain its exact spelling, and the entire token-kind sequence—including newline, indent and dedent tokens—must match. The native parser derives grammar structure from these tokens rather than horizontal adjacency. Name-expression source payloads are additionally compared, including the current multi-token device-selector spelling. Formatting must not silently repair a significant source payload such as `devices . gpu` into a different name.

If canonical indentation changes layout, a second candidate retains original indentation while applying the spacing rules. If neither candidate verifies, formatting fails with `F001` and no output. Ordinary syntax/UTF-8/limit failures retain their existing compiler diagnostics. This is preservation-oriented formatting, not syntax repair or a complete AST reprint. Tabs remain invalid input rather than being silently converted.

Input and output obey the supplied compiler limits (normally 16 MiB source, 500,000 tokens, 200,000 AST nodes and depth 128). At most two candidates are attempted. Parser, token and output budgets can refuse formatting; synchronous processing is bounded but is not an editor responsiveness guarantee.

## Editor integration

The native server advertises `documentFormattingProvider: true` and handles `textDocument/formatting`. The document must be open under its registered URI. The server formats that current, unsaved buffer and returns one whole-document UTF-16 edit, or an empty edit array for unchanged/invalid source or an unopened document. Verification/output-limit refusal (`F001`) returns RequestFailed (-32803). Malformed parameters return InvalidParams (-32602).

The request requires a positive integer `options.tabSize` and boolean `options.insertSpaces`. Style is fixed: these and optional trimming/newline preferences do not override the rules above. The server does not write files, mutate its buffer or publish probe diagnostics. The editor applies the returned edit and sends the corresponding document change. Requests are synchronous; cancellation, versioned formatting edits, range formatting, format-on-type and concurrent/stale-result guarantees are not implemented. [Initial VS Code acceptance](native-vscode.md#qualification) now covers applied edits and idempotence; broader editor/platform qualification remains separate.

## Qualification

Normal and ASan/UBSan tests check exact output, idempotence, native fixture coverage, comments/strings, CRLF/Unicode, unary signs, calls/indexing, nominal construction, multiline collections, source/output limits, significant selector spelling and invalid source. Independent CLI checks verify stdout/check modes and unchanged files. Stdio protocol checks independently apply a Unicode/CRLF edit, simulate the editor's follow-up change, and verify no disk/buffer mutation, repeated requests, malformed options, closed documents and formatting without resolved imports.

The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-formatter-2026-10-20.json) lists the actual selected checks and source hashes. This does not qualify additional operating systems, an editor extension or every possible layout.
