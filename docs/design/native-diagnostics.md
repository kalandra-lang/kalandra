# Native checker diagnostics

`kalandra-check` can return a versioned JSON response for editors and build tools:

```sh
build/native/native/kalandra-check --diagnostics=json main.k
build/native/native/kalandra-check --diagnostics=json --project kalandra.toml
build/native/native/kalandra-check --diagnostics=json --placement main.k
```

This checks source files on disk, including imported modules and explicitly mapped local packages. [Unsaved source overlays](native-source-overlays.md) add root-buffer stdin checking and a multi-buffer loader API. `--placement` uses the shared CPU/Metal placement checker; it does not prove that a program can be lowered to Metal or executed by the available hardware.

## Response format 1

The command writes one JSON object followed by a newline to stdout for a handled success or failure. It writes no terminal diagnostics to stderr in this mode. Exit status is `0` for a successful check, `1` for a source, project or loading failure, and `2` for invalid command-line usage. Process crashes and resource exhaustion are outside this response contract.

A successful response looks like:

```json
{
  "formatVersion": 1,
  "positionEncoding": "utf-16",
  "ok": true,
  "diagnostics": [],
  "summary": { "functions": 1, "values": 1 }
}
```

The summary counts functions and typed expressions in the checked source graph. It is `null` on failure. Each failure entry has these fields:

| Field       | Meaning                                                                       |
| ----------- | ----------------------------------------------------------------------------- |
| `severity`  | `"error"` in this version                                                     |
| `code`      | Existing lexer/parser/module/semantic code, or a command-level code below     |
| `message`   | Human-readable explanation; consume `code` for classification                 |
| `file`      | Original source filename, fallback input filename, or `null` when unavailable |
| `byteRange` | Half-open UTF-8 byte offsets `{ "start": n, "end": n }`, or `null`            |
| `range`     | Half-open `{ "start": { "line": n, "character": n }, "end": ... }`, or `null` |

Lines and characters are zero-based. Characters count UTF-16 code units, so a supplementary Unicode scalar such as an emoji occupies two units. CRLF is one line ending; standalone CR also advances the editor line. This range convention does not change which line endings the lexer accepts. Invalid UTF-8 bytes are replaced with U+FFFD in JSON strings and counted individually when locating invalid source. Offsets inside a valid multibyte scalar map to its beginning for the editor range; the byte range remains a byte range.

Composed module offsets map back to the original file before ranges are calculated. Ranges are clipped to that file, including end-of-file and malformed spans. File paths follow the loader's naming; consumers should not assume every path is absolute or a URI.

Command-level codes are `CLI001` for usage, `PRJ001` for project-manifest failures, and `IO001` for an unstructured loader failure. Existing structured module errors retain their `M...` codes. Failures without source storage, including manifest and missing-root errors, have null ranges. A missing dependency can still point at the import in an available source file. Manifest error messages may contain textual positions, but these are not structured ranges.

`--diagnostics=json` can occur before or after the source path. It cannot be combined with `--dump-ir` or `--dump-plan`; these have separate stdout formats. Without the flag, the existing terminal and dump output remains available.

## Qualification and next tooling work

Normal and ASan/UBSan tests cover Unicode and CRLF, original module paths, span bounds, JSON escaping, invalid UTF-8, batched positions, project/package inputs, placement checking, usage errors and default terminal behavior. See the [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/diagnostics-2026-10-20.json).

This is a checker protocol foundation. Unsaved source overlays and [native language-server diagnostics](native-language-server.md) are implemented; incremental edits and checked type hover are also supported, while completion/navigation, formatting and distribution remain separate work. The JavaScript prototype's language server does not implement this native language.
