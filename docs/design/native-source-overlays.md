# Unsaved native source checking

Native checks can use unsaved source contents without creating temporary `.k` files or replacing project files. This builds on the [versioned checker diagnostics](native-diagnostics.md) for future editor integration.

## Checker command

```sh
build/native/native/kalandra-check --stdin --diagnostics=json path/to/main.k < buffer.txt
build/native/native/kalandra-check --stdin --diagnostics=json --project kalandra.toml < buffer.txt
```

`--stdin` replaces the root source with all bytes read from standard input. The source path is still required, either explicitly or as the manifest entry: it supplies the filename for diagnostics and the directory for relative imports. The source file need not exist yet. With a manifest, standard input replaces its entry file, not the manifest itself. Local package mappings and `--placement` checking remain available.

Other dependencies load normally from disk. The command reads one complete buffer through EOF, then checks once; it does not watch edits or provide an editor protocol. Empty input is an empty source, including its ordinary parse error. Input is bounded by the normal source budget, reserving one byte for the module graph separator. Oversized or unreadable input produces `IO001`; JSON mode preserves its single-response contract. Default terminal and inspection modes can also use `--stdin`.

## Multiple buffers in the native API

`loadModules` accepts an optional final `std::span<const SourceOverlay>`. Each entry contains a path and replacement text. A caller can provide the root, imported dependencies and local package entry files together. Each analysis copies replacement contents into owned immutable `SourceFile` storage. The returned syntax/checked result keeps its source alive after the supplied buffers are released.

The compiler resolves imports and checks exports using those replacements. Changing an unsaved dependency's exported signature therefore rechecks its callers in the same graph. Unreplaced files still load from disk; this does not provide an atomic snapshot of a changing filesystem.

Paths are made absolute and weakly canonicalized so nonexistent files can participate while existing symlinks share normal module identity. Relative imports resolve beside that canonical source path. Duplicate overlay identities, including symlink aliases, are rejected rather than choosing an arbitrary buffer. The loader reports `M006` for invalid/duplicate overlay paths, `M004` for configured limits and the usual parser/type/module codes for source errors.

Both the supplied overlay set and the reachable source graph are bounded independently by the existing file/byte budgets. Each source contributes its text length plus one separator byte. Unreachable overlays cannot bypass the snapshot budget. Existing graph-depth, lexer, parser and diagnostic budgets also apply. Overlay paths require `.k`, without NUL or line breaks. Overlays provide no package discovery or network access.

Checks never write buffer contents. Each call has its own replacements and module graph; a failed or successful check does not change the next check. Diagnostics retain original filenames, UTF-8 byte spans and UTF-16 editor ranges for unsaved text, including imported buffers.

## Qualification and remaining work

Normal and ASan/UBSan tests cover multi-buffer signature changes, owned source lifetimes, Unicode locations, new roots/dependencies/package entries, symlink aliases, cycles, source budgets and unchanged disk checks. CLI tests cover stdin, manifests/packages, JSON/terminal output and bounded reads. See the [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/source-overlays-2026-10-20.json).

The CLI accepts one root buffer; multiple buffers are currently a native API facility. The [native language server](native-language-server.md) now uses this API for versioned full-document/incremental updates and diagnostic publication. Asynchronous stale-result suppression and editor navigation/completion remain. Unsaved project manifests and package installation are not part of source overlays.
