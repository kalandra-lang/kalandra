"""Independent stdlib-only, framed stdio tests for the native language server."""
import json
import os
from pathlib import Path
from ProtocolClient import Client as BaseClient, frame, edit, apply_workspace_edit, publications
import subprocess
import sys
import tempfile

SERVER = sys.argv[1]
ROOT = Path(sys.argv[2])
ROOT.mkdir(parents=True, exist_ok=True)


class Client(BaseClient):
    def __init__(self, *args):
        super().__init__(SERVER, *args)


def invalid_stream(data, expected=2):
    result = subprocess.run([SERVER], input=data, capture_output=True, timeout=20)
    assert result.returncode == expected, (result.returncode, result.stderr)
    assert result.stdout == b""
    assert b"Sanitizer" not in result.stderr and b"runtime error:" not in result.stderr


with tempfile.TemporaryDirectory(dir=ROOT) as temp:
    root = Path(temp).resolve()
    main = root / 'new space#é.k'
    good = "fn main()->int:\n  42\n"
    client = Client()
    assert client.rpc("textDocument/hover", {})["error"]["code"] == -32002
    client.init()
    assert client.rpc("initialize", {})["error"]["code"] == -32600
    client.open(main, 'fn main()->String:\n  "😀é" + missing\n')
    pub = publications(client.barrier())[main.as_uri()]
    assert pub["version"] == 1
    diagnostic = pub["diagnostics"][0]
    assert diagnostic["code"] == "S016" and diagnostic["severity"] == 1
    assert diagnostic["range"] == {"start": {"line": 1, "character": 10},
                                   "end": {"line": 1, "character": 17}}
    client.change(main, good.replace("\n", "\r\n"), 2)
    pub = publications(client.barrier())[main.as_uri()]
    assert pub["version"] == 2 and not pub["diagnostics"]
    client.change(main, "bad", 1)  # Old snapshots cannot replace new text.
    assert not publications(client.barrier())
    client.send("textDocument/didChange", {"textDocument": {"uri": main.as_uri(), "version": 3},
        "contentChanges": [{"range": {"start": {"line": 0, "character": 0},
                                      "end": {"line": 9, "character": 1}}, "text": "bad"}]})
    assert any(e.get("method") == "window/logMessage" for e in client.barrier())
    client.change(main, "bad", 9.223372036854776e18)
    assert any(e.get("method") == "window/logMessage" for e in client.barrier())
    client.change(main, good, 3)  # Invalid change did not consume the version.
    assert not publications(client.barrier())[main.as_uri()]["diagnostics"]
    # New-file normalized URI identity also rejects duplicate opens.
    alias_uri = main.as_uri().replace("/new", "/./new")
    client.send("textDocument/didOpen", {"textDocument": {"uri": alias_uri,
        "version": 1, "languageId": "kalandra", "text": "bad"}})
    assert any(e.get("method") == "window/logMessage" for e in client.barrier())
    client.change(main, " " * (16 * 1024 * 1024), 4)
    assert any(e.get("method") == "window/logMessage" for e in client.barrier())
    client.change(main, good, 4)
    assert not publications(client.barrier())[main.as_uri()]["diagnostics"]
    client.close(main)
    assert not publications(client.barrier())[main.as_uri()]["diagnostics"]
    assert not main.exists(), "server wrote an unsaved file"
    client.finish()

    # Incremental UTF-16 edits over CRLF, sequential ranges and transactional rollback.
    client = Client()
    client.init()
    client.open(main, 'fn main()->String:\r\n  "😀é" + missing\r\n')
    client.barrier()
    client.edits(main, [edit(1, 3, 5, "猫", rangeLength=2), edit(1, 9, 16, '"ok"')], 2)
    assert not publications(client.barrier())[main.as_uri()]["diagnostics"]
    client.edits(main, [{"text": "bad"}, edit(999, 0, 0, "x")], 3)
    assert any(e.get("method") == "window/logMessage" for e in client.barrier())
    client.edits(main, [edit(1, 9, 13, "missing")], 3)
    pub = publications(client.barrier())[main.as_uri()]
    assert pub["version"] == 3 and pub["diagnostics"][0]["code"] == "S016"
    assert pub["diagnostics"][0]["range"]["start"]["character"] == 9
    client.edits(main, [edit(1, 3, 4, "😀"), edit(1, 10, 17, '"ok"')], 4)
    assert not publications(client.barrier())[main.as_uri()]["diagnostics"]
    invalid_changes = [
        [edit(1, 3, 4, "x")],  # Half of a surrogate pair.
        [edit(1, 5, 3, "x")],
        [edit(-1, 0, 0, "x")],
        [edit(1, 0.5, 0.5, "x")],
        [edit(9.223372036854776e18, 0, 0, "x")],
        [edit(1, 3, 5, "x", rangeLength=1)],
        [{"text": "bad", "range": None}],
        [{"text": "bad", "rangeLength": 0}],
        [edit(1, 0, 0, "")] * 1025,
    ]
    for changes in invalid_changes:
        client.edits(main, changes, 5)
        events = client.barrier()
        assert not publications(events) and any(e.get("method") == "window/logMessage" for e in events)
    client.edits(main, [], 5)  # Valid no-op still advances the document version.
    assert publications(client.barrier())[main.as_uri()]["version"] == 5
    client.edits(main, [edit(999, 0, 0, "bad")], 4)  # Stale even when malformed.
    assert not client.barrier()
    client.edits(main, [{"text": good}, edit(1, 200, 200, " ")], 6)
    assert not publications(client.barrier())[main.as_uri()]["diagnostics"]
    client.finish()

    # Open dependencies, changed export signatures, deduplication and close-to-disk.
    lib = root / "lib.k"
    lib.write_text("export fn value()->int:\n  42\n")
    main = root / "main.k"
    caller = 'from "./lib.k" import value\nfn main()->int:\n  value()\n'
    client = Client()
    client.init()
    client.open(lib, lib.read_text())
    client.open(main, caller)
    assert not publications(client.barrier())[main.as_uri()]["diagnostics"]
    client.edits(lib, [edit(0, 19, 22, "String"), edit(1, 2, 4, '"text"')], 2)
    assert publications(client.barrier())[main.as_uri()]["diagnostics"]
    client.change(main, caller.replace("->int", "->String"), 2)
    assert not publications(client.barrier())[main.as_uri()]["diagnostics"]
    client.change(lib, 'export fn value()->String:\n  missing\n', 3)
    pub = publications(client.barrier())[lib.as_uri()]
    assert len(pub["diagnostics"]) == 1 and pub["version"] == 3
    client.close(lib)
    pubs = publications(client.barrier())
    assert pubs[main.as_uri()]["diagnostics"]  # Reverts to int from disk.
    assert not pubs[lib.as_uri()]["diagnostics"] and "version" not in pubs[lib.as_uri()]
    assert lib.read_text() == "export fn value()->int:\n  42\n"
    client.close(main)
    assert not publications(client.barrier())[main.as_uri()]["diagnostics"]
    client.finish()

    # Diagnostics in unopened imports, then clearing when an unsaved import fixes it.
    lib.write_text("export fn value()->int:\n  missing\n")
    client = Client()
    client.init()
    client.open(main, caller)
    pubs = publications(client.barrier())
    assert pubs[lib.as_uri()]["diagnostics"] and "version" not in pubs[lib.as_uri()]
    client.open(lib, "export fn value()->int:\n  42\n")
    assert not publications(client.barrier())[lib.as_uri()]["diagnostics"]
    client.close(main)
    client.close(lib)
    client.barrier()
    client.finish()

    # Placement mode uses the shared checker without accessing GPU hardware.
    placement = ("fn double(x:float32)->float32:\n  x*2\n"
                 "fn main()->float32 ! TaskError:\n  try parallel:\n"
                 "    try await spawn double(2) on devices.gpu()\n")
    for args, expected_error in (((), True), (("--placement",), False)):
        client = Client(*args)
        client.init()
        client.open(main, placement)
        assert bool(publications(client.barrier())[main.as_uri()]["diagnostics"]) == expected_error
        client.finish()

    # Explicit project package mappings and no startup source execution.
    lib.write_text("export fn value()->int:\n  42\n")
    manifest = root / "kalandra.toml"
    manifest.write_text('format=1\n[project]\nentry="main.k"\n[packages]\nmath="lib.k"\n')
    main.write_text(caller.replace('"./lib.k"', '"math"'))
    client = Client("--project", str(manifest))
    client.init()
    client.open(main, main.read_text())
    assert not publications(client.barrier())[main.as_uri()]["diagnostics"]
    client.change(main, "fn main()->int:\n  missing\n", 2)
    assert publications(client.barrier())[main.as_uri()]["diagnostics"]
    client.finish()

    # URI spelling changes must clear the exact previously published URI.
    client = Client()
    client.init()
    local_uri = main.as_uri().replace("file:///", "file://localhost/")
    client.send("textDocument/didOpen", {"textDocument": {"uri": local_uri,
        "languageId": "kalandra", "version": 1, "text": "fn main()->int:\n  missing\n"}})
    assert publications(client.barrier())[local_uri]["diagnostics"]
    client.send("textDocument/didClose", {"textDocument": {"uri": local_uri}})
    assert not publications(client.barrier())[local_uri]["diagnostics"]
    for uri in ("https://example.com/test.k", "file://remote/test.k", "file:///bad%00.k",
                "file:///bad%FF.k", "file:///bad%Z0.k", "file:///bad.k?query"):
        client.send("textDocument/didOpen", {"textDocument": {"uri": uri,
            "languageId": "kalandra", "version": 1, "text": good}})
        assert any(e.get("method") == "window/logMessage" for e in client.barrier())
    client.open(main, good)
    client.barrier()
    client.send("textDocument/didChange", {"textDocument": {"uri": main.as_uri(), "version": 2},
        "contentChanges": [{"text": "bad"}, {"text": good}]})
    assert not publications(client.barrier())[main.as_uri()]["diagnostics"]
    client.close(main)
    client.barrier()
    client.process.stdin.write(frame({"jsonrpc": "2.0", "id": 9.223372036854776e18,
                                     "method": "test/huge"}))
    client.process.stdin.flush()
    assert client.read()["error"]["code"] == -32600
    client.finish()

    # Parse-error recovery and bounded JSON nesting on a still-usable session.
    client = Client()
    for body in (b"{", b"[" * 129 + b"]" * 129):
        client.process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        client.process.stdin.flush()
        assert client.read()["error"]["code"] == -32700
    client.init()
    client.process.stdin.write(frame([]))
    client.process.stdin.flush()
    assert client.read()["error"]["code"] == -32600
    assert client.rpc("shutdown")["result"] is None
    assert client.rpc("unknown")["error"]["code"] == -32600
    client.send("exit")
    client.process.stdin.close()
    assert client.process.wait(timeout=10) == 0
    assert not client.process.stderr.read()

    # Hover reflects checked types, open-buffer changes and exact UTF-16 token ranges.
    source = "fn double(value:int):\n  value*2\nfn main():\n  number=21\n  double(number)\n"
    client = Client()
    client.init()
    client.open(main, source)
    result = client.hover(main, 0, 4)["result"]
    assert result["contents"] == {"kind": "plaintext", "value": "fn double(value: int64) -> int64"}
    assert result["range"] == {"start": {"line": 0, "character": 3}, "end": {"line": 0, "character": 9}}
    assert client.hover(main, 3, 3)["result"]["contents"]["value"] == "number: int64"
    assert client.hover(main, 4, 4)["result"]["contents"]["value"] == "fn double(value: int64) -> int64"
    assert client.hover(main, 1, 0)["result"] is None  # Whitespace.
    assert client.hover(main, 0, 0)["result"] is None  # Keyword.
    assert client.hover(main, 999, 0)["result"] is None
    assert client.hover(main, 0, -1)["error"]["code"] == -32602
    assert client.rpc("textDocument/hover", {})["error"]["code"] == -32602
    client.edits(main, [edit(3, 11, 11, ".5"), edit(0, 16, 19, "float")], 2)
    assert client.hover(main, 3, 3)["result"]["contents"]["value"] == "number: float64"
    assert client.hover(main, 4, 4)["result"]["contents"]["value"] == "fn double(value: float64) -> float64"
    client.change(main, "fn main():\n  unknown\n", 3)
    assert client.hover(main, 0, 4)["result"] is None  # No stale/partial typed IR.
    client.change(main, 'fn main()->String:\r\n  text="😀é"\r\n  text\r\n', 4)
    result = client.hover(main, 1, 10)["result"]  # Inside the string after the emoji.
    assert result["contents"]["value"] == "String"
    assert result["range"] == {"start": {"line": 1, "character": 7}, "end": {"line": 1, "character": 12}}
    assert client.hover(main, 1, 9)["result"] is None  # Inside surrogate pair.
    assert client.hover(main, 2, 3)["result"]["contents"]["value"] == "text: String"
    client.close(main)
    assert client.hover(main, 0, 3)["result"] is None
    client.finish()

    # Imported alias calls resolve to the compiler target, with current unsaved exports.
    lib.write_text("export fn scale(value:int):\n  value*2\n")
    caller = 'from "./lib.k" import scale as twice\nfn main():\n  twice(21)\n'
    client = Client()
    client.init(["markdown", "plaintext"])
    client.open(main, caller)
    hover = client.hover(main, 2, 3)["result"]
    assert hover["contents"] == {"kind": "markdown", "value": "```kalandra\nfn twice(value: int64) -> int64\n```"}
    client.open(lib, "export fn scale(value:float):\n  value*2\n")
    assert "float64" in client.hover(main, 2, 3)["result"]["contents"]["value"]
    client.edits(main, [edit(2, 10, 10, ".5")], 2)
    assert "float64" in client.hover(main, 2, 3)["result"]["contents"]["value"]
    client.close(lib)
    assert client.hover(main, 2, 3)["result"] is None
    client.finish()

    # Definition locations follow checked symbol identity and current source offsets.
    client = Client()
    client.init()
    source = "fn twice(value:int):\n  value*2\nfn main():\n  transform=twice\n  transform(21)\n"
    client.open(main, source)
    target = client.definition(main, 1, 3)["result"]
    assert target == {"uri": main.as_uri(), "range": {"start": {"line": 0, "character": 9},
                                                        "end": {"line": 0, "character": 14}}}
    assert client.definition(main, 3, 14)["result"]["range"]["start"] == {"line": 0, "character": 3}
    assert client.definition(main, 4, 4)["result"]["range"]["start"] == {"line": 3, "character": 2}
    client.change(main, 'fn main()->String:\r\n  text="é"\r\n  "😀" + text\r\n', 2)
    assert client.definition(main, 2, 10)["result"]["range"]["start"] == {"line": 1, "character": 2}
    assert client.definition(main, 2, 4)["result"] is None  # Inside the emoji's surrogate pair.
    assert client.definition(main, 1, 8)["result"] is None  # String literal.
    assert client.definition(main, 999, 0)["result"] is None
    assert client.definition(main, 0, -1)["error"]["code"] == -32602
    assert client.rpc("textDocument/definition", {})["error"]["code"] == -32602
    client.change(main, "fn main():\n  value=1\n  callback=fn(x:int):x+value\n  callback(2)\n", 3)
    assert client.definition(main, 2, 25)["result"]["range"]["start"] == {"line": 1, "character": 2}
    client.change(main, "fn main():\n  missing\n", 4)
    assert client.definition(main, 0, 4)["result"] is None
    client.close(main)
    assert client.definition(main, 0, 4)["result"] is None
    client.finish()

    # References distinguish lexical identities and honor declaration filtering.
    client = Client()
    client.init()
    source = "fn main()->int:\n  value=1\n  if true:\n    value=2\n    value\n  else:\n    value\n"
    client.open(main, source)
    refs = client.references(main, 1, 3)["result"]
    assert len(refs) == 1 and refs[0]["range"]["start"] == {"line": 6, "character": 4}
    refs = client.references(main, 1, 3, True)["result"]
    assert [r["range"]["start"]["line"] for r in refs] == [1, 6]
    assert client.references(main, 3, 5)["result"][0]["range"]["start"]["line"] == 4
    assert client.references(main, 0, 0)["result"] is None
    assert client.references(main, 999, 0)["result"] is None
    assert client.references(main, 0, -1)["error"]["code"] == -32602
    for context in ({}, {"includeDeclaration": 1}, {"includeDeclaration": "true"}):
        assert client.rpc("textDocument/references", {"textDocument": {"uri": main.as_uri()},
            "position": {"line": 1, "character": 3}, "context": context})["error"]["code"] == -32602
    client.change(main, 'fn main()->String:\r\n  text="é"\r\n  "😀" + text\r\n', 2)
    refs = client.references(main, 1, 3)["result"]
    assert refs == [{"uri": main.as_uri(), "range": {"start": {"line": 2, "character": 9},
                                                      "end": {"line": 2, "character": 13}}}]
    assert client.references(main, 2, 4)["result"] is None
    client.close(main)
    assert client.references(main, 0, 3)["result"] is None
    client.finish()

    # Nominal annotations, constructors and update labels use checked source identities.
    client = Client()
    client.init()
    types = ("type Point:\n  x:int\ntype Other:\n  x:int\n"
             "fn copy(point:Point)->Point:\n  point with {x:point.x+1}\n"
             "fn main()->Point:\n  value:Point=Point {x:2}\n  copy(value)\n")
    client.open(main, types)
    line = types.splitlines()[7]
    target = {"uri": main.as_uri(), "range": {"start": {"line": 0, "character": 5},
                                               "end": {"line": 0, "character": 10}}}
    for column in (line.index("Point"), line.rindex("Point")):
        assert client.definition(main, 7, column)["result"] == target
        assert client.hover(main, 7, column)["result"]["contents"]["value"] == "type Point"
    assert len(client.references(main, 7, line.index("Point"))["result"]) == 5
    assert len(client.references(main, 7, line.index("Point"), True)["result"]) == 6
    label = line.index("x:")
    field = client.definition(main, 7, label)["result"]
    assert field["range"]["start"] == {"line": 1, "character": 2}
    refs = client.references(main, 7, label)["result"]
    assert len(refs) == 3 and {r["range"]["start"]["line"] for r in refs} == {5, 7}
    assert client.hover(main, 7, label)["result"]["contents"]["value"] == "x: int64"
    assert client.definition(main, 1, 4)["result"] is None  # Builtin type has no source declaration.
    client.edits(main, [edit(0, 0, 0, "# 😀\r\n")], 2)
    assert client.definition(main, 8, label)["result"]["range"]["start"]["line"] == 2
    client.change(main, "type Point:\n  x:int\nfn main():\n  Point {bad:1}\n", 3)
    assert client.definition(main, 3, 3)["result"] is None
    client.finish()

    # Enum cases and shorthand pattern tokens retain both field and local roles.
    client = Client()
    client.init()
    enum = ("enum Signal:\n  Off\n  Sample:\n    value:int\n"
            "fn read(signal:Signal)->int:\n  match signal:\n"
            "    Signal.Sample {value}:value\n    Signal.Off:0\n"
            "fn main():\n  read(Signal.Sample {value:2})\n")
    client.open(main, enum)
    pattern = enum.splitlines()[6]
    case = pattern.index("Sample")
    label, use = pattern.index("value"), pattern.rindex("value")
    variant = client.definition(main, 6, case)["result"]
    assert variant["range"]["start"] == {"line": 2, "character": 2}
    assert client.hover(main, 6, case)["result"]["contents"]["value"] == "Sample: Signal"
    assert len(client.references(main, 6, case)["result"]) == 2
    assert len(client.references(main, 6, case, True)["result"]) == 3
    binding = client.definition(main, 6, use)["result"]
    assert binding["range"]["start"] == {"line": 6, "character": label}
    assert client.definition(main, 6, label)["result"] == binding
    assert len(client.references(main, 6, label)["result"]) == 1
    assert len(client.references(main, 3, 5)["result"]) == 2  # Field label includes shorthand.
    assert client.hover(main, 6, label)["result"]["contents"]["value"] == "value: int64"
    explicit = enum.replace("{value}:value", "{value:item}:item")
    client.change(main, explicit, 2)
    field = client.definition(main, 6, label)["result"]
    assert field["range"]["start"] == {"line": 3, "character": 4}
    assert client.definition(main, 6, label+6)["result"]["range"]["start"] == {"line": 6, "character": label+6}
    client.edits(main, [edit(0, 0, 0, "# 😀\r\n")], 3)
    assert client.definition(main, 7, case)["result"]["range"]["start"]["line"] == 3
    client.change(main, enum.replace("Signal.Off:0", "Signal.Unknown:0"), 4)
    assert client.definition(main, 6, case)["result"] is None
    client.finish()

    # Imported enum case and payload origins follow an unsaved library's source URI.
    enum_lib = root / "enum space#é.k"
    enum_text = "export enum Signal:\n  Off\n  Sample:\n    value:int\n"
    enum_lib.write_text(enum_text)
    enum_facade = root / "enum-public.k"
    enum_facade.write_text('export from "./enum space#é.k" import Signal as PublicSignal\n')
    enum_caller = ('from "./enum-public.k" import PublicSignal as S\nfn main()->int:\n'
                   '  match S.Sample {value:2}:\n    S.Sample {value:item}:item\n    _:0\n')
    client = Client()
    client.init()
    client.open(main, enum_caller)
    target = client.definition(main, 3, 6)["result"]
    assert target["uri"] == enum_lib.as_uri() and target["range"]["start"] == {"line": 2, "character": 2}
    assert len(client.references(main, 3, 6, True)["result"]) == 3
    assert client.definition(main, 4, 4)["result"] is None  # Wildcard.
    enum_uri = enum_lib.as_uri().replace("file:///", "file://localhost/")
    client.send("textDocument/didOpen", {"textDocument": {"uri": enum_uri,
        "languageId": "kalandra", "version": 1, "text": "# 😀\r\n"+enum_text}})
    target = client.definition(main, 3, 14)["result"]  # Explicit payload label.
    assert target["uri"] == enum_uri and target["range"]["start"] == {"line": 4, "character": 4}
    refs = client.references(main, 3, 6, True)["result"]
    assert any(r["uri"] == enum_uri and r["range"]["start"]["line"] == 3 for r in refs)
    client.send("textDocument/didClose", {"textDocument": {"uri": enum_uri}})
    assert client.definition(main, 3, 6)["result"]["range"]["start"]["line"] == 2
    assert enum_lib.read_text() == enum_text
    client.finish()

    # A same-spelled local function cannot steal a re-exported alias's destination.
    leaf = root / "deep space#é.k"
    original = "export fn scale(value:int):\n  value*2\n"
    leaf.write_text(original)
    facade = root / "public.k"
    facade.write_text('export from "./deep space#é.k" import scale as advertised\n')
    caller = ('from "./public.k" import advertised as twice\n'
              'fn scale(value:String):\n  value\nfn main():\n  twice(21)\n')
    client = Client()
    client.init()
    client.open(main, caller)
    target = client.definition(main, 4, 4)["result"]
    assert target == {"uri": leaf.as_uri(), "range": {"start": {"line": 0, "character": 10},
                                                        "end": {"line": 0, "character": 15}}}
    refs = client.references(main, 4, 4, True)["result"]
    assert {(r["uri"], r["range"]["start"]["line"]) for r in refs} == {(leaf.as_uri(), 0), (main.as_uri(), 0), (main.as_uri(), 4), (facade.as_uri(), 0)}, refs
    assert len(refs) == 6  # Original declaration, two import spellings per alias, and call.
    assert client.definition(main, 0, caller.index("advertised"))["result"] == target
    assert client.definition(main, 0, caller.index("twice"))["result"] == target
    assert "fn twice(" in client.hover(main, 0, caller.index("twice"))["result"]["contents"]["value"]
    other = root / "other.k"
    client.open(other, 'from "./public.k" import advertised\nfn main():\n  advertised(3)\n')
    refs = client.references(main, 4, 4)["result"]
    assert {r["uri"] for r in refs} == {main.as_uri(), other.as_uri(), facade.as_uri()}
    assert len(refs) == 7  # Shared dependency occurrences are deduplicated.
    client.change(other, "fn main():\n  missing\n", 2)
    assert client.references(main, 4, 4)["error"]["code"] == -32803
    client.close(other)
    local_uri = leaf.as_uri().replace("file:///", "file://localhost/")
    client.send("textDocument/didOpen", {"textDocument": {"uri": local_uri,
        "languageId": "kalandra", "version": 1, "text": "\n\n" + original}})
    target = client.definition(main, 4, 4)["result"]
    assert target["uri"] == local_uri and target["range"]["start"]["line"] == 2
    client.send("textDocument/didChange", {"textDocument": {"uri": local_uri, "version": 2},
        "contentChanges": [edit(0, 0, 0, "# 😀\n")]})
    assert client.definition(main, 4, 4)["result"]["range"]["start"]["line"] == 3
    refs = client.references(main, 4, 4, True)["result"]
    assert len(refs) == 6
    assert any(r["uri"] == local_uri and r["range"]["start"]["line"] == 3 for r in refs)
    client.send("textDocument/didClose", {"textDocument": {"uri": local_uri}})
    target = client.definition(main, 4, 4)["result"]
    assert target["uri"] == leaf.as_uri() and target["range"]["start"]["line"] == 0
    refs = client.references(main, 4, 4, True)["result"]
    assert any(r["uri"] == leaf.as_uri() and r["range"]["start"]["line"] == 0 for r in refs)
    assert leaf.read_text() == original
    client.finish()

    # Explicit package mappings also retain the ultimate source declaration.
    manifest.write_text('format=1\n[project]\nentry="main.k"\n[packages]\nmath="public.k"\n')
    client = Client("--project", str(manifest))
    client.init(workspaceFolders=[])
    client.open(main, caller.replace('"./public.k"', '"math"'))
    assert client.definition(main, 4, 4)["result"]["uri"] == leaf.as_uri()
    assert len(client.references(main, 4, 4, True)["result"]) == 6
    client.finish()

    # Workspace discovery includes unopened importers, with fresh reads and buffer precedence.
    workspace = root / "workspace space#é"
    workspace.mkdir()
    (workspace / "nested").mkdir()
    ws_lib = workspace / "lib.k"
    ws_source = "export fn twice(value:int):\n  value*2\n"
    ws_lib.write_text(ws_source)
    first = workspace / "first.k"
    first_source = 'from "./lib.k" import twice\nfn main():\n  twice(1)\n'
    first.write_text(first_source)
    second = workspace / "nested" / "second.k"
    second.write_text(first_source.replace('"./lib.k"', '"../lib.k"'))
    sibling = root / "workspace-other"
    sibling.mkdir()
    broken = sibling / "bad.k"
    broken.write_text("fn main():\n  unknown\n")
    folders = [{"uri": workspace.as_uri(), "name": "project"},
               {"uri": sibling.as_uri(), "name": "other"}]
    client = Client()
    client.init(workspaceFolders=folders)
    client.open(ws_lib, ws_source)
    client.open(broken, broken.read_text())
    refs = client.references(ws_lib, 0, 11)["result"]
    assert len(refs) == 4 and {r["uri"] for r in refs} == {first.as_uri(), second.as_uri()}
    assert len(client.references(ws_lib, 0, 11, True)["result"]) == 5
    first.write_text("# disk edit\n"+first_source)
    refs = client.references(ws_lib, 0, 11)["result"]
    assert any(r["uri"] == first.as_uri() and r["range"]["start"]["line"] == 3 for r in refs)
    added = workspace / "added.k"
    added.write_text(first_source)
    assert len(client.references(ws_lib, 0, 11)["result"]) == 6
    added.unlink()
    assert len(client.references(ws_lib, 0, 11)["result"]) == 4
    first.write_text("fn main():\n  missing\n")
    first_uri = first.as_uri().replace("file:///", "file://localhost/")
    client.send("textDocument/didOpen", {"textDocument": {"uri": first_uri,
        "languageId": "kalandra", "version": 1, "text": first_source}})
    refs = client.references(ws_lib, 0, 11)["result"]
    assert len(refs) == 4 and any(r["uri"] == first_uri for r in refs)
    client.send("textDocument/didClose", {"textDocument": {"uri": first_uri}})
    assert client.references(ws_lib, 0, 11)["error"]["code"] == -32803
    first.write_text(first_source)
    client.change(ws_lib, "export fn twice(value:String):\n  value\n", 2)
    assert client.references(ws_lib, 0, 11)["error"]["code"] == -32803
    client.change(ws_lib, ws_source, 3)
    for excluded in (".git", ".hg", ".svn", "node_modules", "build", "dist", "target",
                     ".cache", ".svelte-kit", ".vercel"):
        directory = workspace / excluded
        directory.mkdir()
        (directory / "bad.k").write_text("fn main():\n  unknown\n")
    (workspace / "external-link").symlink_to(sibling, target_is_directory=True)
    (workspace / "file-link.k").symlink_to(broken)
    assert len(client.references(ws_lib, 0, 11)["result"]) == 4
    client.finish()

    # rootUri fallback and workspaceFolders precedence; no dynamic folder changes advertised.
    client = Client()
    client.init(rootUri=workspace.as_uri())
    client.open(ws_lib, ws_source)
    assert len(client.references(ws_lib, 0, 11)["result"]) == 4
    client.finish()
    client = Client()
    client.init(workspaceFolders=folders, rootUri="invalid ignored root")
    client.open(ws_lib, ws_source)
    assert len(client.references(ws_lib, 0, 11)["result"]) == 4
    outside = root / "standalone.k"
    standalone = "fn only()->int:\n  1\nfn main():\n  only()\n"
    client.open(outside, standalone)
    assert len(client.references(outside, 0, 4)["result"]) == 1  # Outside configured roots: own graph only.
    client.finish()

    # An explicitly nested workspace isolates its query from the containing project's errors.
    nested_workspace = workspace / "island"
    nested_workspace.mkdir()
    island_lib = nested_workspace / "lib.k"
    island_lib.write_text(ws_source)
    (nested_workspace / "caller.k").write_text(first_source)
    outer_bad = workspace / "bad.k"
    outer_bad.write_text("fn main():\n  missing\n")
    client = Client()
    client.init(workspaceFolders=[folders[0], {"uri": nested_workspace.as_uri(), "name": "nested"}])
    client.open(island_lib, ws_source)
    assert len(client.references(island_lib, 0, 11)["result"]) == 2
    client.finish()
    outer_bad.unlink()

    # Explicit manifest-directory discovery supplies package mappings to unopened roots.
    project_dir = root / "discovered-project"
    project_dir.mkdir()
    project_lib = project_dir / "lib.k"
    project_lib.write_text(ws_source)
    package_caller = first_source.replace('"./lib.k"', '"math"')
    (project_dir / "main.k").write_text(package_caller)
    (project_dir / "unopened.k").write_text(package_caller)
    project_manifest = project_dir / "kalandra.toml"
    project_manifest.write_text('format=1\n[project]\nentry="main.k"\n[packages]\nmath="lib.k"\n')
    client = Client("--project", str(project_manifest))
    client.init()
    client.open(project_lib, ws_source)
    assert len(client.references(project_lib, 0, 11)["result"]) == 4
    client.finish()
    client = Client("--project", str(project_manifest))
    client.init(workspaceFolders=[])
    client.open(project_lib, ws_source)
    assert len(client.references(project_lib, 0, 11)["result"]) == 2  # Explicit opt-out, project entry retained.
    client.finish()

    # Explicit imports may reach external packages; discovery does not crawl their directories.
    external_package = root / "external-package.k"
    external_package.write_text(ws_source)
    project_manifest.write_text('format=1\n[project]\nentry="main.k"\n[packages]\nmath="../external-package.k"\n')
    client = Client("--project", str(project_manifest))
    client.init()
    client.open(project_dir / "main.k", package_caller)
    refs = client.references(project_dir / "main.k", 2, 3, True)["result"]
    assert len(refs) == 5 and any(r["uri"] == external_package.as_uri() for r in refs)
    assert all(r["uri"] != project_lib.as_uri() for r in refs)  # Same spelling, distinct declaration.
    client.finish()

    # Initialization validates roots transactionally and permits a corrected initialize request.
    for params in ({"workspaceFolders": {}}, {"workspaceFolders": [{"uri": broken.as_uri()}]},
                   {"rootUri": "https://example.com/"}, {"rootUri": (root/"missing").as_uri()},
                   {"workspaceFolders": [folders[0]]*17}):
        client = Client()
        assert client.rpc("initialize", {"capabilities": {}, **params})["error"]["code"] == -32602
        client.init(workspaceFolders=[])
        client.finish()

    # Discovery budgets fail explicitly before yielding a partial array, and recover next request.
    limited = root / "limited-workspace"
    limited.mkdir()
    limited_lib = limited / "lib.k"
    limited_lib.write_text(ws_source)
    for number in range(512):
        (limited / f"{number}.k").write_text("")
    client = Client()
    client.init(rootUri=limited.as_uri())
    client.open(limited_lib, ws_source)
    response = client.references(limited_lib, 0, 11)
    assert response["error"]["code"] == -32803 and "source-file budget" in response["error"]["message"]
    for number in range(512):
        (limited / f"{number}.k").unlink()
    assert client.references(limited_lib, 0, 11)["result"] == []
    client.finish()

    # Versioned rename proposals preserve Unicode/CRLF and never write buffers or files.
    rename_root = root / "rename-unicode"
    rename_root.mkdir()
    rename_file = rename_root / "main.k"
    disk = "fn main():\n  0\n"
    rename_file.write_text(disk)
    rename_text = '# 😀\r\nfn main()->String:\r\n  value="é"\r\n  "😀" + value\r\n'
    rename_uri = rename_file.as_uri().replace("file:///", "file://localhost/")
    client = Client()
    client.init(rename=True, rootUri=rename_root.as_uri())
    client.send("textDocument/didOpen", {"textDocument": {"uri": rename_uri,
        "languageId": "kalandra", "version": 7, "text": rename_text}})
    prepared = client.prepare_rename(rename_uri, 3, 10)["result"]
    assert prepared == {"placeholder": "value", "range": {"start": {"line": 3, "character": 9},
                                                           "end": {"line": 3, "character": 14}}}
    change = client.rename(rename_uri, 3, 10, "textValue")["result"]
    assert len(change["documentChanges"]) == 1
    assert change["documentChanges"][0]["textDocument"] == {"uri": rename_uri, "version": 7}
    assert len(change["documentChanges"][0]["edits"]) == 2
    applied = apply_workspace_edit(change, {rename_uri: rename_text})[rename_uri]
    assert applied == rename_text.replace("value", "textValue")
    assert rename_file.read_text() == disk
    assert client.prepare_rename(rename_uri, 3, 10)["result"] == prepared  # Proposal did not mutate state.
    assert client.rename(rename_uri, 3, 10, "value")["result"] == {"documentChanges": []}
    for invalid in ("", "fn", "String", "float32", "_", "main", "é", "a b", "9name"):
        assert client.rename(rename_uri, 3, 10, invalid)["error"]["code"] == -32602
    assert client.prepare_rename(rename_uri, 1, 4)["result"] is None  # main entry.
    assert client.prepare_rename(rename_uri, 1, 0)["result"] is None  # keyword.
    assert client.prepare_rename(rename_uri, 3, 4)["result"] is None  # split surrogate.
    assert client.rename(rename_uri, 999, 0, "other")["result"] is None
    assert client.rename(rename_uri, 0, -1, "other")["error"]["code"] == -32602
    client.send("textDocument/didChange", {"textDocument": {"uri": rename_uri, "version": 8},
        "contentChanges": [{"text": applied}]})
    assert client.prepare_rename(rename_uri, 3, 10)["result"]["placeholder"] == "textValue"
    client.finish()

    # No unversioned edit fallback and no implicit workspace for rename.
    client = Client()
    client.init(rootUri=rename_root.as_uri())
    client.open(rename_file, rename_text)
    assert client.rename(rename_file, 2, 3, "other")["error"]["code"] == -32803
    client.finish()
    client = Client()
    client.init(rename=True)
    client.open(rename_file, rename_text)
    assert client.rename(rename_file, 2, 3, "other")["error"]["code"] == -32803
    client.finish()
    client = Client()
    caps = client.rpc("initialize", {"rootUri": rename_root.as_uri(), "capabilities": {
        "workspace": {"workspaceEdit": {"documentChanges": True}}}})["result"]["capabilities"]
    assert caps["renameProvider"] is True  # No options object without prepareSupport.
    client.send("initialized", {})
    client.open(rename_file, rename_text)
    assert client.rename(rename_file, 2, 3, "other")["result"]["documentChanges"]
    client.finish()

    # Original export rename stops at explicit aliases, including identical spelling aliases.
    alias_root = root / "rename-aliases"
    alias_root.mkdir()
    alias_sources = {
        "lib.k": "export fn scale(value:int):\n  value*2\n",
        "facade.k": 'export from "./lib.k" import scale as publicScale\n',
        "relay.k": 'export from "./facade.k" import publicScale\n',
        "main.k": 'from "./relay.k" import publicScale as localScale\nfn main():\n  localScale(2)\n',
        "unopened.k": 'from "./lib.k" import scale\nfn main():\n  scale(2)\n',
        "same.k": 'from "./lib.k" import scale as scale\nfn main():\n  scale(2)\n'}
    for file, text in alias_sources.items():
        (alias_root/file).write_text(text)
    alias_uris = {(alias_root/file).as_uri(): text for file, text in alias_sources.items()}
    client = Client()
    client.init(rename=True, rootUri=alias_root.as_uri())
    client.open(alias_root/"lib.k", alias_sources["lib.k"], 4)
    change = client.rename(alias_root/"lib.k", 0, 11, "amplify")["result"]
    assert {entry["textDocument"]["uri"] for entry in change["documentChanges"]} == {
        (alias_root/file).as_uri() for file in ("lib.k", "facade.k", "unopened.k", "same.k")}
    for entry in change["documentChanges"]:
        assert entry["textDocument"]["version"] == (4 if entry["textDocument"]["uri"] == (alias_root/"lib.k").as_uri() else None)
    applied = apply_workspace_edit(change, alias_uris)
    assert 'import amplify as scale' in applied[(alias_root/"same.k").as_uri()]
    assert '  scale(2)' in applied[(alias_root/"same.k").as_uri()]
    assert applied[(alias_root/"main.k").as_uri()] == alias_sources["main.k"]
    assert '  amplify(2)' in applied[(alias_root/"unopened.k").as_uri()]
    client.open(alias_root/"facade.k", alias_sources["facade.k"])
    at = alias_sources["facade.k"].index("publicScale")
    change = client.rename(alias_root/"facade.k", 0, at, "published")["result"]
    applied = apply_workspace_edit(change, alias_uris)
    assert 'import published' in applied[(alias_root/"relay.k").as_uri()]
    assert 'published as localScale' in applied[(alias_root/"main.k").as_uri()]
    assert applied[(alias_root/"lib.k").as_uri()] == alias_sources["lib.k"]
    client.open(alias_root/"main.k", alias_sources["main.k"], 2)
    change = client.rename(alias_root/"main.k", 2, 3, "transform")["result"]
    assert len(change["documentChanges"]) == 1 and change["documentChanges"][0]["textDocument"]["version"] == 2
    applied = apply_workspace_edit(change, alias_uris)
    assert 'publicScale as transform' in applied[(alias_root/"main.k").as_uri()]
    assert '  transform(2)' in applied[(alias_root/"main.k").as_uri()]
    for file, text in alias_sources.items():
        assert (alias_root/file).read_text() == text
    client.finish()

    # Shorthand field rename and local rename expand opposite roles.
    pattern_root = root / "rename-patterns"
    pattern_root.mkdir()
    pattern_file = pattern_root / "main.k"
    pattern_source = ('enum Box:\n  Some:\n    value:int\nfn read(box:Box)->int:\n'
                      '  match box:\n    Box.Some {value}:value\nfn main():\n  read(Box.Some {value:2})\n')
    pattern_file.write_text(pattern_source)
    client = Client()
    client.init(rename=True, rootUri=pattern_root.as_uri())
    client.open(pattern_file, pattern_source)
    change = client.rename(pattern_file, 2, 5, "amount")["result"]
    applied = apply_workspace_edit(change, {pattern_file.as_uri(): pattern_source})[pattern_file.as_uri()]
    assert '{amount:value}:value' in applied and '{amount:2}' in applied
    at = pattern_source.splitlines()[5].index("value")
    change = client.rename(pattern_file, 5, at, "amount")["result"]
    applied = apply_workspace_edit(change, {pattern_file.as_uri(): pattern_source})[pattern_file.as_uri()]
    assert '{value:amount}:amount' in applied and '{value:2}' in applied
    client.finish()

    # A compiling rename that captures another binding is rejected without changing the buffer.
    capture_root = root / "rename-capture"
    capture_root.mkdir()
    capture_file = capture_root/"main.k"
    capture_source = "fn main():\n  value=1\n  read=fn(other:int):value+other\n  read(2)\n"
    capture_file.write_text(capture_source)
    client = Client()
    client.init(rename=True, rootUri=capture_root.as_uri())
    client.open(capture_file, capture_source)
    response = client.rename(capture_file, 1, 3, "other")
    assert response["error"]["code"] == -32803 and "resolution" in response["error"]["message"]
    assert client.prepare_rename(capture_file, 1, 3)["result"]["placeholder"] == "value"
    assert capture_file.read_text() == capture_source
    client.finish()

    # Completion probes unsaved buffers, returns plain UTF-16 edits, and leaves
    # diagnostics and source buffers alone. Dependencies use the same overlays.
    completion_root = Path(temp) / "completion"
    completion_root.mkdir()
    library = completion_root / "library.k"
    library.write_text("export fn hidden(value:int):\n  value\n")
    document = completion_root / "main.k"
    client = Client()
    client.init()
    text = ('from "./library.k" import hidden as publicName\n'
            'fn main():\n  earlier = 1\n  pubSuffix\n')
    client.open(document, text)
    client.barrier()
    result = client.completion(document, 3, 5)["result"]
    assert not result["isIncomplete"]
    assert [item["label"] for item in result["items"]] == ["publicName"], result
    item = result["items"][0]
    assert item["kind"] == 3 and item["insertTextFormat"] == 1
    assert item["textEdit"] == {"newText": "publicName", "range": {
        "start": {"line": 3, "character": 2}, "end": {"line": 3, "character": 11}}}, item
    assert not document.exists(), "completion wrote the unsaved document"
    assert not client.barrier(), "completion published probe diagnostics"
    client.open(library, "export fn changed(value:int):\n  value\n")
    client.barrier()
    assert client.completion(document, 3, 5)["result"]["items"] == [], "stale import suggestions"
    client.close(library)
    client.barrier()
    assert client.completion(document, 3, 5)["result"]["items"][0]["label"] == "publicName"
    # Astral Unicode before a cursor must not shift the UTF-16 replacement edit.
    # Use a preceding String argument instead of an unsupported statement separator.
    text = 'fn join(text:String, value:int):\n  value\nfn main():\n  earlier = 2\n  join("😀", earSuffix)\n'
    client.change(document, text, 2)
    client.barrier()
    result = client.completion(document, 4, 16)["result"]
    assert [item["label"] for item in result["items"]] == ["earlier"], result
    assert result["items"][0]["textEdit"]["range"] == {
        "start": {"line": 4, "character": 13}, "end": {"line": 4, "character": 22}}
    assert client.completion(document, 4, 9)["result"] is None, "surrogate split"
    assert client.completion(document, 300, 0)["result"] is None
    assert client.rpc("textDocument/completion", {"textDocument": {"uri": document.as_uri()},
        "position": {"line": -1, "character": 0}})["error"]["code"] == -32602
    # Record fields use current imported declarations and UTF-16 member edits.
    library.write_text("export type Box:\n  value:int\n")
    text = ('from "./library.k" import Box\n'
            'fn join(text:String, value:int):\n  value\n'
            'fn main():\n  box=Box {value:1}\n  join("😀", box.vaSuffix)\n')
    client.change(document, text, 3)
    client.barrier()
    member_line = text.splitlines()[5]
    start = len(member_line[:member_line.index("vaSuffix")].encode("utf-16-le")) // 2
    result = client.completion(document, 5, start + 2)["result"]
    assert [item["label"] for item in result["items"]] == ["value"], result
    field = result["items"][0]
    assert field["kind"] == 5 and field["detail"] == "record field", field
    assert field["textEdit"] == {"newText": "value", "range": {
        "start": {"line": 5, "character": start},
        "end": {"line": 5, "character": start + len("vaSuffix")}}}, field
    assert not client.barrier(), "member probe leaked diagnostics"
    client.open(library, "export type Box:\n  changed:int\n")
    client.barrier()
    assert client.completion(document, 5, start + 2)["result"]["items"] == [], "stale member declaration"
    client.change(document, text.replace("{value:1}", "{changed:1}").replace("vaSuffix", "chSuffix"), 4)
    client.barrier()
    assert [item["label"] for item in client.completion(document, 5, start + 2)["result"]["items"]] == ["changed"]
    client.close(library)
    client.barrier()
    # Enum constructors and match patterns use the same current nominal declaration.
    library.write_text("export enum Choice:\n  Some:\n    value:int\n  Other\n")
    text = ('from "./library.k" import Choice as Pick\n'
            'fn main():\n  Pick.SoSuffix {value:1}\n')
    client.change(document, text, 5)
    client.barrier()
    result = client.completion(document, 2, 9)["result"]
    assert [item["label"] for item in result["items"]] == ["Some"], result
    case = result["items"][0]
    assert case["kind"] == 20 and case["detail"] == "enum case", case
    assert case["textEdit"] == {"newText": "Some", "range": {
        "start": {"line": 2, "character": 7}, "end": {"line": 2, "character": 15}}}, case
    assert not client.barrier(), "enum probe leaked diagnostics"
    text = ('from "./library.k" import Choice as Pick\n'
            'fn inspect(c:Pick):\n  match c:\n    Pick.Other: 0\n    Pick.: 1\n'
            'fn main():\n  0\n')
    client.change(document, text, 6)
    client.barrier()
    assert [item["label"] for item in client.completion(document, 4, 9)["result"]["items"]] == ["Some"]
    client.open(library, "export enum Choice:\n  Changed\n  Other\n")
    client.barrier()
    assert [item["label"] for item in client.completion(document, 4, 9)["result"]["items"]] == ["Changed"]
    client.close(library)
    client.barrier()
    # Named construction labels add a colon only when absent, without dummy values.
    library.write_text("export type Box:\n  value:int\n  other:int\n")
    text = ('from "./library.k" import Box\n'
            'fn join(text:String,value:Box):\n  value\n'
            'fn main():\n  join("😀", Box { vaSuffix, other:2 })\n')
    client.change(document, text, 7)
    client.barrier()
    line = text.splitlines()[4]
    start = len(line[:line.index("vaSuffix")].encode("utf-16-le")) // 2
    result = client.completion(document, 4, start + 2)["result"]
    assert [item["label"] for item in result["items"]] == ["value"], result
    field = result["items"][0]
    assert field["kind"] == 5 and field["insertTextFormat"] == 1
    assert field["textEdit"] == {"newText": "value: ", "range": {
        "start": {"line": 4, "character": start},
        "end": {"line": 4, "character": start + 8}}}, field
    assert not document.exists(), "label completion wrote source"
    assert not client.barrier(), "label probe published diagnostics"
    client.change(document, text.replace("vaSuffix", "vaSuffix:1"), 8)
    client.barrier()
    assert client.completion(document, 4, start + 2)["result"]["items"][0]["textEdit"]["newText"] == "value"
    client.open(library, "export type Box:\n  variable:int\n  other:int\n")
    client.barrier()
    assert [item["label"] for item in client.completion(document, 4, start + 2)["result"]["items"]] == ["variable"]
    client.close(library)
    client.barrier()
    # Pattern labels preserve shorthand and explicit local binder names.
    library.write_text("export enum Choice:\n  Some:\n    value:int\n    count:int\n  Other\n")
    text = ('from "./library.k" import Choice as Pick\n'
            'fn inspect(p:Pick):\n  match p:\n    Pick.Some {vaSuffix, count:total}: 0\n'
            '    Pick.Other: 0\nfn main():\n  0\n')
    client.change(document, text, 9)
    client.barrier()
    start = text.splitlines()[3].index("vaSuffix")
    result = client.completion(document, 3, start + 2)["result"]
    assert [item["label"] for item in result["items"]] == ["value"], result
    assert result["items"][0]["textEdit"] == {"newText": "value", "range": {
        "start": {"line": 3, "character": start},
        "end": {"line": 3, "character": start + 8}}}, result
    client.change(document, text.replace("vaSuffix", "vaSuffix:item"), 10)
    client.barrier()
    assert client.completion(document, 3, start + 2)["result"]["items"][0]["textEdit"]["newText"] == "value"
    assert client.completion(document, 3, start + 11)["result"]["items"] == []
    assert not client.barrier(), "pattern probe leaked diagnostics"
    client.open(library, "export enum Choice:\n  Some:\n    variable:int\n    count:int\n  Other\n")
    client.barrier()
    assert [item["label"] for item in client.completion(document, 3, start + 2)["result"]["items"]] == ["variable"]
    client.close(library)
    client.barrier()
    assert not document.exists(), "pattern completion wrote source"
    client.close(document)
    client.barrier()
    assert client.completion(document, 4, 16)["result"] is None
    client.finish()

    # Signature help uses checked imported signatures and current unsaved graphs.
    signature_document = Path(temp) / "signature.k"
    signature_library = Path(temp) / "signatures.k"
    library_text = "export fn combine(text:String,left:int,right:int)->int:\n  left+right\n"
    signature_library.write_text(library_text)
    client = Client()
    client.init()
    text = ('from "./signatures.k" import combine as join\n'
            'fn main():\n  join("😀",1,2)\n')
    client.open(signature_document, text)
    client.barrier()
    line = text.splitlines()[2]
    cursor = len(line[:line.index(",2") + 1].encode("utf-16-le")) // 2
    result = client.signature(signature_document, 2, cursor)["result"]
    assert result == {"signatures": [{"label": "fn join(text: String, left: int64, right: int64) -> int64",
        "parameters": [{"label": "text: String"}, {"label": "left: int64"}, {"label": "right: int64"}]}],
        "activeSignature": 0, "activeParameter": 2}, result
    assert not client.barrier(), "signature query leaked diagnostics"
    client.open(signature_library, library_text.replace("right", "amount"))
    client.barrier()
    assert "amount: int64" in client.signature(signature_document, 2, cursor)["result"]["signatures"][0]["label"]
    client.change(signature_library, library_text.replace("left+right", "missing"), 2)
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"] is None
    client.close(signature_library)
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"] is not None
    client.change(signature_document, text.replace(',2)', ',2'), 2)
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"] is None
    # Recovery applies only at the trailing cursor, never by hiding later source.
    recovered = client.signature(signature_document, 2, cursor + 1)["result"]
    assert recovered["activeParameter"] == 2 and "right: int64" in recovered["signatures"][0]["label"], recovered
    assert not client.barrier(), "recovery query published synthetic diagnostics"
    client.change(signature_document, text.replace(',2)', ','), 3)
    client.barrier()
    missing_argument = client.signature(signature_document, 2, cursor)["result"]
    assert missing_argument["activeParameter"] == 2, missing_argument
    assert client.signature(signature_document, 200, 0)["result"] is None
    assert client.rpc("textDocument/signatureHelp", {"textDocument": {"uri": signature_document.as_uri()},
        "position": {"line": -1, "character": 0}})["error"]["code"] == -32602
    client.close(signature_document)
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"] is None
    assert not signature_document.exists(), "signature query wrote source"
    assert signature_library.read_text() == library_text
    client.finish()

    client = Client()
    client.init(signature_offsets=True)
    text = 'fn add(a:int,b:int)->int:\n  a+b\nfn main():\n  f=add\n  f(1,2)\n'
    client.open(signature_document, text)
    client.barrier()
    result = client.signature(signature_document, 4, 6)["result"]
    assert result["activeParameter"] == 1
    assert result["signatures"][0] == {"label": "fn f(int64, int64) -> int64",
        "parameters": [{"label": [5, 10]}, {"label": [12, 17]}]}, result
    client.finish()

    # Higher-order calls use the checked invocation's callee type, not the factory's signature.
    higher_library = Path(temp) / "higher-order.k"
    higher_text = "export fn make()->fn(int,int)->int:\n  fn(a:int,b:int):a+b\n"
    higher_library.write_text(higher_text)
    client = Client()
    client.init(signature_offsets=True)
    text = ('from "./higher-order.k" import make as factory\n'
            'fn main():\n  factory()(1,2)\n')
    client.open(signature_document, text)
    client.barrier()
    cursor = text.splitlines()[2].index(",2") + 1
    result = client.signature(signature_document, 2, cursor)["result"]
    assert result == {"signatures": [{"label": "fn(int64, int64) -> int64",
        "parameters": [{"label": [3, 8]}, {"label": [10, 15]}]}],
        "activeSignature": 0, "activeParameter": 1}, result
    assert not client.barrier(), "higher-order query published diagnostics"
    client.open(higher_library, higher_text.replace("int", "uint"))
    client.barrier()
    result = client.signature(signature_document, 2, cursor)["result"]
    assert result["signatures"][0]["label"] == "fn(uint64, uint64) -> uint64", result
    client.change(higher_library, higher_text.replace("a+b", "missing"), 2)
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"] is None
    client.close(higher_library)
    client.barrier()
    text = ('from "./higher-order.k" import make as factory\n'
            'fn main():\n  callbacks=[factory()]\n  callbacks[0](1,2)\n')
    client.change(signature_document, text, 2)
    client.barrier()
    cursor = text.splitlines()[3].index(",2") + 1
    result = client.signature(signature_document, 3, cursor)["result"]
    assert result["activeParameter"] == 1 and result["signatures"][0]["label"] == "fn(int64, int64) -> int64", result
    client.change(signature_document, text.replace("callbacks[0]", "callbacks[2]"), 3)
    client.barrier()
    assert client.signature(signature_document, 3, cursor)["result"] is None
    assert not signature_document.exists() and higher_library.read_text() == higher_text
    client.finish()

    client = Client()
    client.init(signature_offsets=True)
    recovered_text = ('from "./higher-order.k" import make as factory\n'
                      'fn main():\n  factory()(1,2\n')
    client.open(signature_document, recovered_text)
    client.barrier()
    cursor = len(recovered_text.splitlines()[2])
    result = client.signature(signature_document, 2, cursor)["result"]
    assert result["signatures"][0]["label"] == "fn(int64, int64) -> int64", result
    client.open(higher_library, higher_text.replace("int", "uint"))
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"]["signatures"][0]["label"] == "fn(uint64, uint64) -> uint64"
    client.change(higher_library, higher_text.replace("a+b", "missing"), 2)
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"] is None
    client.close(higher_library)
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"] is not None
    assert not client.barrier(), "temporary closing delimiters changed diagnostics"
    assert not signature_document.exists() and higher_library.read_text() == higher_text
    client.finish()

    client = Client()
    client.init(signature_offsets=True)
    partial_text = ('from "./higher-order.k" import make as factory\n'
                    'fn main():\n  factory()(1,)\n')
    client.open(signature_document, partial_text)
    client.barrier()
    cursor = partial_text.splitlines()[2].index(",)") + 1
    result = client.signature(signature_document, 2, cursor)["result"]
    assert result["activeParameter"] == 1 and result["signatures"][0]["label"] == "fn(int64, int64) -> int64", result
    assert not client.barrier(), "missing-argument snapshot changed published diagnostics"
    client.open(higher_library, higher_text.replace("int", "uint"))
    client.barrier()
    assert "uint64" in client.signature(signature_document, 2, cursor)["result"]["signatures"][0]["label"]
    client.change(signature_document, partial_text.replace("(1,)", '("bad",)'), 2)
    client.barrier()
    bad_cursor = partial_text.splitlines()[2].index(",)") + 5
    assert client.signature(signature_document, 2, bad_cursor)["result"] is None
    assert not signature_document.exists() and higher_library.read_text() == higher_text
    client.finish()

    client = Client()
    client.init(signature_offsets=True)
    editing_text = ('from "./higher-order.k" import make as factory\n'
                    'fn main():\n  factory()(1, vaSuffix)\n')
    client.open(signature_document, editing_text)
    client.barrier()
    cursor = editing_text.splitlines()[2].index("vaSuffix") + 2
    result = client.signature(signature_document, 2, cursor)["result"]
    assert result["activeParameter"] == 1 and result["signatures"][0]["label"] == "fn(int64, int64) -> int64", result
    assert client.hover(signature_document, 2, cursor)["result"] is None, "signature projection escaped into ordinary analysis"
    assert not client.barrier(), "editing-name projection published diagnostics"
    client.open(higher_library, higher_text.replace("int", "uint"))
    client.barrier()
    assert "uint64" in client.signature(signature_document, 2, cursor)["result"]["signatures"][0]["label"]
    broken_prefix = editing_text.replace("(1,", "(missing,")
    client.change(signature_document, broken_prefix, 2)
    client.barrier()
    bad_cursor = broken_prefix.splitlines()[2].index("vaSuffix") + 2
    assert client.signature(signature_document, 2, bad_cursor)["result"] is None
    client.change(signature_document, editing_text.replace(", vaSuffix)", ", vaSuffix"), 3)
    client.barrier()
    end_cursor = len(editing_text.splitlines()[2]) - 1
    assert "uint64" in client.signature(signature_document, 2, end_cursor)["result"]["signatures"][0]["label"]
    assert not signature_document.exists() and higher_library.read_text() == higher_text
    client.finish()

    # Name completion shares bounded trailing-call recovery with signature help.
    call_library = Path(temp) / "completion-call.k"
    call_library_text = "export fn combine(left:int,right:int)->int:\n  left+right\n"
    call_library.write_text(call_library_text)
    client = Client()
    client.init()
    call_text = ('from "./completion-call.k" import combine\n'
                 'fn main():\n  earlier=2\n  combine(ear\n')
    client.open(signature_document, call_text)
    assert publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    result = client.completion(signature_document, 3, len(call_text.splitlines()[3]))["result"]
    assert [item["label"] for item in result["items"]] == ["earlier"], result
    item = result["items"][0]
    assert item["textEdit"] == {"newText":"earlier", "range":{
        "start":{"line":3,"character":10}, "end":{"line":3,"character":13}}}, item
    assert not client.barrier(), "completion recovery published probe diagnostics"
    assert client.hover(signature_document, 3, 11)["result"] is None
    client.open(call_library, call_library_text.replace("left+right", "unknown"))
    client.barrier()
    assert client.completion(signature_document, 3, 13)["result"]["items"] == []
    client.change(call_library, call_library_text, 2)
    client.barrier()
    assert client.completion(signature_document, 3, 13)["result"]["items"][0]["label"] == "earlier"
    unicode_call = ('fn join(text:String,left:int,right:int)->int:\n  left+right\n'
                    'fn main():\n  earlier=2\n  join("😀", ear\n')
    client.change(signature_document, unicode_call, 2)
    client.barrier()
    line = unicode_call.splitlines()[4]
    end = len(line.encode("utf-16-le")) // 2
    result = client.completion(signature_document, 4, end)["result"]
    assert [item["label"] for item in result["items"]] == ["earlier"], result
    assert result["items"][0]["textEdit"]["range"] == {
        "start":{"line":4,"character":end-3},"end":{"line":4,"character":end}}
    broken = unicode_call.replace('join("😀", ear', 'join(1, ear')
    client.change(signature_document, broken, 3)
    client.barrier()
    assert client.completion(signature_document, 4, len(broken.splitlines()[4]))["result"]["items"] == []
    assert not signature_document.exists() and call_library.read_text() == call_library_text
    client.finish()

    # A final member name may be incomplete, but its receiver must still check.
    receiver_library = Path(temp) / "signature-record.k"
    receiver_text = "export type Reading:\n  value:int\n"
    receiver_library.write_text(receiver_text)
    client = Client()
    client.init(signature_offsets=True)
    member_text = ('from "./higher-order.k" import make as factory\n'
                   'from "./signature-record.k" import Reading\n'
                   'fn main():\n  reading=Reading {value:2}\n'
                   '  unused="😀"\n  factory()(1, reading.vaSuffix)\n')
    client.open(signature_document, member_text)
    client.barrier()
    line = member_text.splitlines()[5]
    cursor = len(line[:line.index("vaSuffix") + 2].encode("utf-16-le")) // 2
    result = client.signature(signature_document, 5, cursor)["result"]
    assert result["activeParameter"] == 1 and result["signatures"][0]["label"] == "fn(int64, int64) -> int64", result
    assert client.hover(signature_document, 5, cursor)["result"] is None
    assert not client.barrier(), "member signature query published diagnostic replacements"
    client.open(receiver_library, receiver_text.replace("value:int", "other:int"))
    client.barrier()
    assert client.signature(signature_document, 5, cursor)["result"] is None, "invalid receiver constructor was hidden"
    client.change(receiver_library, receiver_text, 2)
    client.barrier()
    assert client.signature(signature_document, 5, cursor)["result"] is not None
    broken = member_text.replace("reading.vaSuffix", "unknown.vaSuffix")
    client.change(signature_document, broken, 2)
    client.barrier()
    bad_cursor = broken.splitlines()[5].index("vaSuffix") + 2
    assert client.signature(signature_document, 5, bad_cursor)["result"] is None
    opened = member_text.replace("reading.vaSuffix)", "reading.vaSuffix")
    client.change(signature_document, opened, 3)
    client.barrier()
    assert client.signature(signature_document, 5, cursor)["result"]["signatures"][0]["label"] == "fn(int64, int64) -> int64", "EOF repair preserves the field suffix and validated factory callee"
    end_cursor = len(opened.splitlines()[5])
    assert client.signature(signature_document, 5, end_cursor)["result"] is not None
    empty = member_text.replace("reading.vaSuffix", "reading.")
    client.change(signature_document, empty, 4)
    assert publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    empty_cursor = empty.splitlines()[5].index("reading.") + len("reading.")
    result = client.signature(signature_document, 5, empty_cursor)["result"]
    assert result["activeParameter"] == 1 and result["signatures"][0]["label"] == "fn(int64, int64) -> int64", result
    assert client.hover(signature_document, 5, empty_cursor)["result"] is None
    assert not client.barrier(), "empty-field recovery published replacement diagnostics"
    client.open(higher_library, higher_text.replace("int", "uint"))
    client.barrier()
    assert "uint64" in client.signature(signature_document, 5, empty_cursor)["result"]["signatures"][0]["label"]
    client.change(receiver_library, receiver_text.replace("value:int", "other:int"), 3)
    client.barrier()
    assert client.signature(signature_document, 5, empty_cursor)["result"] is None
    client.change(receiver_library, receiver_text, 4)
    client.barrier()
    assert client.signature(signature_document, 5, empty_cursor)["result"] is not None
    client.change(signature_document, empty.replace("reading.)", "reading."), 5)
    client.barrier()
    assert client.signature(signature_document, 5, empty_cursor)["result"] is not None
    unknown = empty.replace("reading.)", "unknown.)")
    client.change(signature_document, unknown, 6)
    client.barrier()
    assert client.signature(signature_document, 5, unknown.splitlines()[5].index("unknown.") + 8)["result"] is None
    assert not signature_document.exists() and receiver_library.read_text() == receiver_text
    assert higher_library.read_text() == higher_text
    client.finish()

    # Annotation completion follows only the current module's type namespace.
    type_library = Path(temp) / "type-completion.k"
    type_library_text = "export type Reading:\n  value:int\ntype Hidden:\n  value:int\n"
    type_library.write_text(type_library_text)
    client = Client()
    client.init()
    type_text = ('from "./type-completion.k" import Reading as Sample\n'
                 'fn inspect(value:SaSuffix):\n  0\nfn main():\n  0\n')
    client.open(signature_document, type_text)
    client.barrier()
    start = type_text.splitlines()[1].index("SaSuffix")
    result = client.completion(signature_document, 1, start + 2)["result"]
    assert [item["label"] for item in result["items"]] == ["Sample"], result
    item = result["items"][0]
    assert item["kind"] == 22 and item["textEdit"] == {"newText":"Sample", "range":{
        "start":{"line":1,"character":start},"end":{"line":1,"character":start+8}}}, item
    assert client.hover(signature_document, 1, start+2)["result"] is None
    assert not client.barrier(), "type completion published probe diagnostics"
    client.open(type_library, type_library_text.replace("type Reading", "type Changed"))
    client.barrier()
    assert client.completion(signature_document, 1, start+2)["result"]["items"] == []
    client.change(type_library, type_library_text, 2)
    client.barrier()
    assert client.completion(signature_document, 1, start+2)["result"]["items"][0]["label"] == "Sample"
    client.change(signature_document, type_text.replace("SaSuffix", "Sample"), 2)
    assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    assert client.hover(signature_document, 1, start+2)["result"] is not None
    tensor_type = 'fn inspect(values:Tensor[floSuffix,?]):\n  0\nfn main():\n  0\n'
    client.change(signature_document, tensor_type, 3)
    client.barrier()
    result = client.completion(signature_document, 0, tensor_type.splitlines()[0].index("floSuffix")+3)["result"]
    assert [item["label"] for item in result["items"]] == ["float","float32","float64"], result
    array_type = 'fn inspect(values:[]):\n  0\nfn main():\n  0\n'
    client.change(signature_document, array_type, 4)
    client.barrier()
    result = client.completion(signature_document, 0, array_type.splitlines()[0].index("]"))["result"]
    labels = [item["label"] for item in result["items"]]
    assert "Bool" in labels and "String" in labels and "int8" in labels
    assert not any(name in labels for name in ["TaskError","Tensor","Hidden","Reading","Sample"]), labels
    assert not signature_document.exists() and type_library.read_text() == type_library_text
    client.finish()

    # Expected-error slots expose only scoped error families/aliases.
    channel_library = Path(temp) / "channel-completion.k"
    channel_library_text = ('export error ParseError:\n  Invalid\nerror Hidden:\n  Bad\n'
                            'export type Problem = ParseError | TaskError\n'
                            'export type Plain:\n  value:int\n')
    channel_library.write_text(channel_library_text)
    client = Client()
    client.init()
    channel_imports = 'from "./channel-completion.k" import ParseError as Parse, Problem as Failure, Plain\n'
    channel_text = channel_imports + 'fn inspect()->int ! FaSuffix:\n  0\nfn main():\n  0\n'
    client.open(signature_document, channel_text)
    client.barrier()
    start = channel_text.splitlines()[1].index("FaSuffix")
    result = client.completion(signature_document, 1, start+2)["result"]
    assert [item["label"] for item in result["items"]] == ["Failure"], result
    assert result["items"][0]["kind"] == 22
    assert result["items"][0]["textEdit"] == {"newText":"Failure","range":{
        "start":{"line":1,"character":start},"end":{"line":1,"character":start+8}}}
    assert not client.barrier(), "error-channel query published probe diagnostics"
    assert client.hover(signature_document, 1, start+2)["result"] is None
    client.open(channel_library, channel_library_text.replace("type Problem", "type Changed"))
    client.barrier()
    assert client.completion(signature_document, 1, start+2)["result"]["items"] == []
    client.change(channel_library, channel_library_text, 2)
    client.barrier()
    assert client.completion(signature_document, 1, start+2)["result"]["items"][0]["label"] == "Failure"
    client.change(signature_document, channel_text.replace("FaSuffix","Failure"), 2)
    assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    union_text = channel_imports + 'fn inspect()->int ! (Parse | ):\n  0\nfn main():\n  0\n'
    client.change(signature_document, union_text, 3)
    client.barrier()
    cursor = union_text.splitlines()[1].index("):")
    result = client.completion(signature_document, 1, cursor)["result"]
    assert [item["label"] for item in result["items"]] == ["Failure","Parse","TaskError"], result
    alias_text = channel_imports + 'type Composite = \nfn main():\n  0\n'
    client.change(signature_document, alias_text, 4)
    client.barrier()
    result = client.completion(signature_document, 1, len(alias_text.splitlines()[1]))["result"]
    assert [item["label"] for item in result["items"]] == ["Failure","Parse","TaskError"], result
    assert not signature_document.exists() and channel_library.read_text() == channel_library_text
    client.finish()

    # Import-name completion uses public declarations and resolved re-export names.
    exports_library = Path(temp) / "exports-completion.k"
    exports_text = ('export fn double(v:int):\n  v*2\nfn hidden():\n  0\n'
                    'export type Box:\n  value:int\nexport error Problem:\n  Bad\n'
                    'export type Failure = Problem | TaskError\n')
    exports_library.write_text(exports_text)
    facade = Path(temp) / "facade-completion.k"
    facade_text = ('export from "./exports-completion.k" import double as twice, Box as Parcel, Failure\n'
                   'from "./exports-completion.k" import Problem\n')
    facade.write_text(facade_text)
    client = Client()
    client.init()
    import_text = 'from "./facade-completion.k" import twSuffix as run\nfn main():\n  0\n'
    client.open(signature_document, import_text)
    client.barrier()
    start = import_text.splitlines()[0].index("twSuffix")
    result = client.completion(signature_document, 0, start+2)["result"]
    assert [item["label"] for item in result["items"]] == ["twice"], result
    item = result["items"][0]
    assert item["kind"] == 3 and item["textEdit"] == {"newText":"twice", "range":{
        "start":{"line":0,"character":start},"end":{"line":0,"character":start+8}}}, item
    assert not client.barrier(), "import completion published probe diagnostics"
    empty_import = 'from "./facade-completion.k" import \nfn main():\n  0\n'
    client.change(signature_document, empty_import, 2)
    client.barrier()
    result = client.completion(signature_document, 0, len(empty_import.splitlines()[0]))["result"]
    assert [(item["label"],item["kind"]) for item in result["items"]] == [
        ("Failure",22),("Parcel",22),("twice",3)], result
    client.open(exports_library, exports_text.replace("export fn double", "fn double"))
    client.barrier()
    result = client.completion(signature_document, 0, len(empty_import.splitlines()[0]))["result"]
    assert [item["label"] for item in result["items"]] == ["Failure","Parcel"], result
    client.change(exports_library, exports_text, 2)
    client.barrier()
    accepted = 'from "./facade-completion.k" import twice as run\nfn main():\n  run(21)\n'
    client.change(signature_document, accepted, 3)
    assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    assert client.hover(signature_document, 2, 3)["result"] is not None
    # Alias declarations and empty import prefixes do not supply public-name suggestions.
    alias_cursor = accepted.splitlines()[0].index("run") + 1
    assert client.completion(signature_document, 0, alias_cursor)["result"]["items"] == []
    assert client.completion(signature_document, 0, 6)["result"]["items"] == []
    missing = 'from "./missing-completion.k" import \nfn main():\n  0\n'
    client.change(signature_document, missing, 4)
    client.barrier()
    assert client.completion(signature_document, 0, len(missing.splitlines()[0]))["result"]["items"] == []
    assert not signature_document.exists() and exports_library.read_text() == exports_text
    assert facade.read_text() == facade_text
    client.finish()

    # Relative import paths enumerate one directory without reading candidate source contents.
    path_root = root / "path-candidates"
    path_root.mkdir()
    path_document = path_root / "main.k"
    path_disk = 'fn main():\n  0\n'
    path_document.write_text(path_disk)
    math_text = 'export fn double(v:int):\n  v*2\n'
    (path_root / "math.k").write_text(math_text)
    (path_root / "math😀.k").write_text(math_text)
    (path_root / "notes.txt").write_text("not Kalandra")
    (path_root / ".hidden.k").write_text(math_text)
    if os.name != "nt":
        # Windows forbids quotes and treats backslashes as path separators.
        (path_root / 'bad"name.k').write_text(math_text)
        (path_root / ("bad" + chr(92) + "name.k")).write_text(math_text)
    (path_root / "nested").mkdir()
    (path_root / "nested" / "helper.k").write_text(math_text)
    (path_root / "linked.k").symlink_to(path_root / "math.k")
    (path_root / "linked-folder").symlink_to(path_root / "nested", target_is_directory=True)
    client = Client()
    client.init()
    path_text = 'from "./ma😀Suffix.k" import double as twice\nfn main():\n  twice(21)\n'
    client.open(path_document, path_text)
    client.barrier()
    result = client.completion(path_document, 0, len('from "./ma'))["result"]
    assert [item["label"] for item in result["items"]] == ["./math.k", "./math😀.k"], result
    item = result["items"][0]
    assert item["kind"] == 17 and item["textEdit"] == {"newText":"./math.k", "range":{
        "start":{"line":0,"character":6},
        "end":{"line":0,"character":6+len('./ma😀Suffix.k'.encode('utf-16-le'))//2}}}, item
    assert not client.barrier(), "path completion published diagnostics"
    # Applying just the path edit preserves the imported alias and checks the call.
    client.change(path_document, path_text.replace('./ma😀Suffix.k','./math.k'), 2)
    assert not publications(client.barrier())[path_document.as_uri()]["diagnostics"]
    assert client.hover(path_document, 2, 4)["result"] is not None
    empty_path = 'export from "./" import double\nfn main():\n  0\n'
    client.change(path_document, empty_path, 3)
    client.barrier()
    result = client.completion(path_document, 0, len('export from "./'))["result"]
    assert [(item["label"],item["kind"]) for item in result["items"]] == [
        ("./math.k",17),("./math😀.k",17),("./nested/",19)], result
    nested_path = 'from "./nested/heSuffix.k" import double\nfn main():\n  0\n'
    client.change(path_document, nested_path, 4)
    client.barrier()
    result = client.completion(path_document, 0, len('from "./nested/he'))["result"]
    assert [item["label"] for item in result["items"]] == ["./nested/helper.k"], result
    parent_path = 'from "../maSuffix.k" import double\nfn main():\n  0\n'
    nested_document = path_root / "nested" / "main.k"
    client.open(nested_document, parent_path)
    client.barrier()
    result = client.completion(nested_document, 0, len('from "../ma'))["result"]
    assert [item["label"] for item in result["items"]] == ["../main.k", "../math.k", "../math😀.k"], result
    virtual = path_root / "virtual.k"
    client.open(virtual, math_text)
    client.barrier()
    client.change(path_document, 'from "./vi" import double\nfn main():\n  0\n', 5)
    client.barrier()
    result = client.completion(path_document, 0, len('from "./vi'))["result"]
    assert [item["label"] for item in result["items"]] == ["./virtual.k"], result
    assert not virtual.exists() and not nested_document.exists()
    assert path_document.read_text() == path_disk and (path_root / "math.k").read_text() == math_text
    # The sorted result limit is independent of filesystem iteration order.
    for i in range(300):
        (path_root / f"candidate{i:03}.k").write_text("invalid source is not read")
    bounded_path = 'from "./candidate" import double\nfn main():\n  0\n'
    client.change(path_document, bounded_path, 6)
    client.barrier()
    result = client.completion(path_document, 0, len('from "./candidate'))["result"]
    assert result["isIncomplete"] and len(result["items"]) == 256, result
    assert result["items"][0]["label"] == "./candidate000.k"
    assert result["items"][-1]["label"] == "./candidate255.k"
    narrowed = bounded_path.replace("candidate", "candidate29")
    client.change(path_document, narrowed, 7)
    client.barrier()
    result = client.completion(path_document, 0, len('from "./candidate29'))["result"]
    assert not result["isIncomplete"] and len(result["items"]) == 10, result
    client.finish()



    # Collection builtin signatures are specializations of checked IR operations.
    client = Client()
    client.init(signature_offsets=True)
    builtin_cases = [
        ('fn main():\n  length(|[1,2])\n', 'fn length(input: [int64]) -> int64', 0),
        ('fn main():\n  byteLength(|"😀")\n', 'fn byteLength(input: String) -> int64', 0),
        ('fn main():\n  shape(|tensor([1,2]))\n', 'fn shape(input: Tensor[int64, 2]) -> [int64]', 0),
        ('fn main():\n  slice([1,2,3],0,|2)\n',
         'fn slice(input: [int64], start: int64, end: int64) -> [int64]', 2),
        ('fn main():\n  "😀abc" -> slice(0,|2)\n',
         'fn slice(input: String, start: int64, end: int64) -> String', 2),
        ('fn main():\n  concat([true],|[false])\n',
         'fn concat(left: [Bool], right: [Bool]) -> [Bool]', 1),
    ]
    client.open(signature_document, 'fn main():\n  0\n')
    client.barrier()
    version = 1
    for marked, expected, active in builtin_cases:
        version += 1
        cursor = marked.index('|')
        text = marked.replace('|','')
        client.change(signature_document, text, version)
        assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
        line = marked[:cursor].count('\n')
        character = len(marked[:cursor].rsplit('\n',1)[-1].encode('utf-16-le'))//2
        result = client.signature(signature_document, line, character)["result"]
        assert result["signatures"][0]["label"] == expected and result["activeParameter"] == active, result
        for parameter in result["signatures"][0]["parameters"]:
            start, end = parameter["label"]
            assert 0 <= start < end <= len(expected) and ':' in expected[start:end], result
        assert not client.barrier(), "builtin signature published diagnostics"
    version += 1
    client.change(signature_document, 'fn main():\n  length(42)\n', version)
    before = publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    assert before and client.signature(signature_document, 1, 9)["result"] is None
    assert not client.barrier(), "invalid builtin signature changed diagnostics"
    signature_values = Path(temp) / "signature-values.k"
    signature_values_text = 'export fn read()->String:\n  "abc"\n'
    signature_values.write_text(signature_values_text)
    version += 1
    imported_builtin = 'from "./signature-values.k" import read\nfn main():\n  length(read())\n'
    client.change(signature_document, imported_builtin, version)
    assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    assert client.signature(signature_document, 2, 9)["result"]["signatures"][0]["label"] == 'fn length(input: String) -> int64'
    client.open(signature_values, 'export fn read()->[int32]:\n  [1,2]\n')
    client.barrier()
    assert client.signature(signature_document, 2, 9)["result"]["signatures"][0]["label"] == 'fn length(input: [int32]) -> int64'
    client.change(signature_values, 'export fn read()->String:\n  missing\n', 2)
    client.barrier()
    assert client.signature(signature_document, 2, 9)["result"] is None
    client.change(signature_values, signature_values_text, 3)
    client.barrier()
    assert client.signature(signature_document, 2, 9)["result"]["signatures"][0]["label"] == 'fn length(input: String) -> int64'
    assert not signature_document.exists() and signature_values.read_text() == signature_values_text
    client.finish()

    # A missing concat operand derives its contract from the checked first array.
    client = Client()
    client.init(signature_offsets=True)
    incomplete_concat = 'fn main():\n  values:[int32]=[1,2]\n  concat(values,\n'
    client.open(signature_document, incomplete_concat)
    before = publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    assert before and any(d["code"] == "L005" for d in before), before
    result = client.signature(signature_document, 2, len(incomplete_concat.splitlines()[2]))["result"]
    expected = 'fn concat(left: [int32], right: [int32]) -> [int32]'
    assert result["activeParameter"] == 1 and result["signatures"][0]["label"] == expected, result
    labels = result["signatures"][0]["parameters"]
    assert [expected[p["label"][0]:p["label"][1]] for p in labels] == ["left: [int32]", "right: [int32]"]
    assert not client.barrier(), "missing concat guidance replaced the actual syntax diagnostic"
    partial_concat = 'fn main():\n  concat([true], riSuffix)\n'
    client.change(signature_document, partial_concat, 2)
    before = publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    assert before and any(d["code"] == "S016" for d in before), before
    cursor = partial_concat.splitlines()[1].index('riSuffix') + 2
    result = client.signature(signature_document, 1, cursor)["result"]
    assert result["signatures"][0]["label"] == 'fn concat(left: [Bool], right: [Bool]) -> [Bool]', result
    assert client.hover(signature_document, 1, cursor)["result"] is None
    assert not client.barrier(), "editing-hole concat guidance published diagnostics"
    client.change(signature_document, 'fn main():\n  [true] -> concat()\n', 3)
    client.barrier()
    result = client.signature(signature_document, 1, len('  [true] -> concat('))["result"]
    assert result["activeParameter"] == 1 and result["signatures"][0]["label"] == 'fn concat(left: [Bool], right: [Bool]) -> [Bool]', result
    hint_library = Path(temp) / 'concat-hints.k'
    hint_library_text = 'export type Box:\n  value:int\n'
    hint_library.write_text(hint_library_text)
    imported_concat = ('from "./concat-hints.k" import Box as Parcel\n'
                       'fn main():\n  concat([Parcel {value:1}],)\n')
    client.change(signature_document, imported_concat, 4)
    client.barrier()
    cursor = imported_concat.splitlines()[2].index(',') + 1
    result = client.signature(signature_document, 2, cursor)["result"]
    assert result["signatures"][0]["label"] == 'fn concat(left: [Box], right: [Box]) -> [Box]', result
    client.open(hint_library, hint_library_text.replace('value:int','value:String'))
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"] is None
    client.change(hint_library, hint_library_text, 2)
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"] is not None
    assert not signature_document.exists() and hint_library.read_text() == hint_library_text
    client.finish()

    # Compiler builtin names appear at call targets, not first-class value positions.
    client = Client()
    client.init()
    builtin_completion = 'fn main():\n  coSuffix([1],[2])\n'
    client.open(signature_document, builtin_completion)
    client.barrier()
    start = builtin_completion.splitlines()[1].index('coSuffix')
    result = client.completion(signature_document, 1, start+2)["result"]
    assert [item["label"] for item in result["items"]] == ["concat"], result
    item = result["items"][0]
    assert item["kind"] == 3 and item["textEdit"] == {"newText":"concat", "range":{
        "start":{"line":1,"character":start},"end":{"line":1,"character":start+8}}}, item
    assert not client.barrier(), "builtin completion published probe diagnostics"
    accepted = builtin_completion.replace('coSuffix','concat')
    client.change(signature_document, accepted, 2)
    assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    conversion = 'fn main():\n  floSuffix(1)\n'
    client.change(signature_document, conversion, 3)
    client.barrier()
    start = conversion.splitlines()[1].index('floSuffix')
    result = client.completion(signature_document, 1, start+3)["result"]
    assert [item["label"] for item in result["items"]] == ["float","float32","float64"], result
    assert all(item["kind"] == 3 for item in result["items"]), result
    client.change(signature_document, conversion.replace('floSuffix','float32'), 4)
    assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    piped = 'fn main():\n  [1,2] -> parSuffix(fn(v:int):v*2)\n'
    client.change(signature_document, piped, 5)
    client.barrier()
    result = client.completion(signature_document, 1, piped.splitlines()[1].index('parSuffix')+3)["result"]
    assert [item["label"] for item in result["items"]] == ["parallelMap"], result
    ordinary = 'fn main():\n  coSuffix\n'
    client.change(signature_document, ordinary, 6)
    client.barrier()
    assert client.completion(signature_document, 1, 4)["result"]["items"] == []
    callback = 'fn main():\n  map([1,2],coSuffix)\n'
    client.change(signature_document, callback, 7)
    client.barrier()
    assert client.completion(signature_document, 1, callback.splitlines()[1].index('coSuffix')+2)["result"]["items"] == []
    shadowed = 'fn main():\n  concat=fn(v:int):v*2\n  coSuffix(1)\n'
    client.change(signature_document, shadowed, 8)
    client.barrier()
    result = client.completion(signature_document, 2, 4)["result"]
    assert [(item["label"],item["kind"],item["detail"]) for item in result["items"]] == [
        ("concat",6,"binding")], result
    assert not signature_document.exists()
    client.finish()

    # Numeric conversion hints use checked source/destination types and tensor shapes.
    client = Client()
    client.init(signature_offsets=True)
    numeric_cases = [
        ('fn main():\n  float32(|1)\n', 'fn float32(input: int64) -> float32'),
        ('fn main():\n  uint(|int16(1))\n', 'fn uint(input: int16) -> uint64'),
        ('fn main():\n  float(|int8(1))\n', 'fn float(input: int8) -> float64'),
        ('fn main():\n  int16(|tensor([1,2],[1,2]))\n',
         'fn int16(input: Tensor[int64, 1, 2]) -> Tensor[int16, 1, 2]'),
        ('fn convert(v:Tensor[float32,?,2]):\n  float64(v|)\nfn main():\n  0\n',
         'fn float64(input: Tensor[float32, ?, 2]) -> Tensor[float64, ?, 2]'),
        ('fn main():\n  1 -> float32(|)\n', 'fn float32(input: int64) -> float32'),
        ('fn main():\n  float32=fn(v:int):v+1\n  float32(|2)\n', 'fn float32(int64) -> int64'),
    ]
    client.open(signature_document, 'fn main():\n  0\n')
    client.barrier()
    version = 1
    for marked, expected in numeric_cases:
        version += 1
        cursor = marked.index('|')
        text = marked.replace('|','')
        client.change(signature_document, text, version)
        assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
        line = marked[:cursor].count('\n')
        character = len(marked[:cursor].rsplit('\n',1)[-1].encode('utf-16-le'))//2
        result = client.signature(signature_document, line, character)["result"]
        assert result["signatures"][0]["label"] == expected and result["activeParameter"] == 0, result
        start, end = result["signatures"][0]["parameters"][0]["label"]
        assert expected[start:end] == ("int64" if 'float32(int64)' in expected else expected[expected.index('input:'):expected.rfind(') ->')]), result
        assert not client.barrier(), "numeric signature leaked diagnostics"
    for invalid in ['int8(300)', 'float32([1,2])', 'float32(true)']:
        version += 1
        text = 'fn main():\n  '+invalid+'\n'
        client.change(signature_document, text, version)
        diagnostics = publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
        assert diagnostics and any(d["code"] == 'S055' for d in diagnostics), diagnostics
        assert client.signature(signature_document, 1, text.splitlines()[1].index('(')+1)["result"] is None
        assert not client.barrier(), "invalid numeric hint changed diagnostics"
    numeric_library = Path(temp) / 'numeric-signatures.k'
    numeric_library_text = 'export fn read()->float32:\n  float32(1)\n'
    numeric_library.write_text(numeric_library_text)
    version += 1
    imported_numeric = 'from "./numeric-signatures.k" import read\nfn main():\n  float64(read())\n'
    client.change(signature_document, imported_numeric, version)
    client.barrier()
    assert client.signature(signature_document, 2, 10)["result"]["signatures"][0]["label"] == 'fn float64(input: float32) -> float64'
    client.open(numeric_library, 'export fn read()->int16:\n  int16(1)\n')
    client.barrier()
    assert client.signature(signature_document, 2, 10)["result"]["signatures"][0]["label"] == 'fn float64(input: int16) -> float64'
    client.change(numeric_library, 'export fn read()->String:\n  "bad"\n', 2)
    client.barrier()
    assert client.signature(signature_document, 2, 10)["result"] is None
    client.change(numeric_library, numeric_library_text, 3)
    client.barrier()
    assert client.signature(signature_document, 2, 10)["result"] is not None
    assert not signature_document.exists() and numeric_library.read_text() == numeric_library_text
    client.finish()

    # Tensor builtin hints use checked operation operands and derived result shapes.
    client = Client()
    client.init(signature_offsets=True)
    tensor_cases = [
        ('fn main():\n  reduceSum(|tensor([1,2]))\n',
         'fn reduceSum(input: Tensor[int64, 2]) -> int64', 0),
        ('fn main():\n  array(|tensor([7],[]))\n',
         'fn array(input: Tensor[int64]) -> [int64]', 0),
        ('fn main():\n  matmul(tensor([1,2],[1,2]),|tensor([3,4],[2,1]))\n',
         'fn matmul(left: Tensor[int64, 1, 2], right: Tensor[int64, 2, 1]) -> Tensor[int64, 1, 1]', 1),
        ('fn main():\n  tensor([1,2],[1,2]) -> matmul(|tensor([3,4],[2,1]))\n',
         'fn matmul(left: Tensor[int64, 1, 2], right: Tensor[int64, 2, 1]) -> Tensor[int64, 1, 1]', 1),
        ('fn multiply(a:Tensor[float32,?,3],b:Tensor[float32,3,?]):\n  matmul(a,|b)\nfn main():\n  0\n',
         'fn matmul(left: Tensor[float32, ?, 3], right: Tensor[float32, 3, ?]) -> Tensor[float32, ?, ?]', 1),
    ]
    client.open(signature_document, 'fn main():\n  0\n')
    client.barrier()
    version = 1
    for marked, expected, active in tensor_cases:
        version += 1
        cursor = marked.index('|')
        text = marked.replace('|','')
        client.change(signature_document, text, version)
        assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
        line = marked[:cursor].count('\n')
        character = len(marked[:cursor].rsplit('\n',1)[-1].encode('utf-16-le'))//2
        result = client.signature(signature_document, line, character)["result"]
        assert result["signatures"][0]["label"] == expected and result["activeParameter"] == active, result
        parameters = result["signatures"][0]["parameters"]
        assert len(parameters) == (2 if expected.startswith('fn matmul') else 1), result
        for parameter in parameters:
            start, end = parameter["label"]
            assert 0 <= start < end <= len(expected) and ':' in expected[start:end], result
        assert not client.barrier(), "tensor signature published probe diagnostics"
    matrix_library = Path(temp) / 'matrix-signatures.k'
    matrix_library_text = 'export fn read()->Tensor[int64,1,2]:\n  tensor([1,2],[1,2])\n'
    matrix_library.write_text(matrix_library_text)
    version += 1
    imported_matrix = ('from "./matrix-signatures.k" import read\nfn main():\n'
                       '  matmul(read(),tensor([3,4],[2,1]))\n')
    client.change(signature_document, imported_matrix, version)
    client.barrier()
    cursor = imported_matrix.splitlines()[2].index(',')+1
    result = client.signature(signature_document, 2, cursor)["result"]
    assert result["signatures"][0]["label"].endswith('-> Tensor[int64, 1, 1]'), result
    client.open(matrix_library, 'export fn read()->Tensor[int64,2,2]:\n  tensor([1,2,3,4],[2,2])\n')
    client.barrier()
    result = client.signature(signature_document, 2, cursor)["result"]
    assert result["signatures"][0]["label"] == 'fn matmul(left: Tensor[int64, 2, 2], right: Tensor[int64, 2, 1]) -> Tensor[int64, 2, 1]', result
    client.change(matrix_library, 'export fn read()->Tensor[int64,1,3]:\n  tensor([1,2,3],[1,3])\n', 2)
    diagnostics = publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    assert any(d["code"] == 'S029' for d in diagnostics), diagnostics
    assert client.signature(signature_document, 2, cursor)["result"] is None
    assert not client.barrier(), "invalid matrix signature changed diagnostics"
    client.change(matrix_library, matrix_library_text, 3)
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"] is not None
    assert not signature_document.exists() and matrix_library.read_text() == matrix_library_text
    client.finish()

    # Map callback hints expose the element contract, not closure/wrapper captures.
    client = Client()
    client.init(signature_offsets=True)
    map_cases = [
        ('fn main():\n  map([1,2],|fn(v):v>0)\n',
         'fn map(input: [int64], callback: fn(int64) -> Bool) -> [Bool]'),
        ('fn main():\n  offset=3\n  callback=fn(v:int):v+offset\n  map([1,2],|callback)\n',
         'fn map(input: [int64], callback: fn(int64) -> int64) -> [int64]'),
        ('fn main():\n  [1,2] -> map(|fn(v):v*2)\n',
         'fn map(input: [int64], callback: fn(int64) -> int64) -> [int64]'),
        ('fn main()->[int] ! TaskError:\n  try parallelMap([1,2],|fn(v):v*2)\n',
         'fn parallelMap(input: [int64], callback: fn(int64) -> int64) -> [int64] ! TaskError'),
        ('error Bad:\n  Failed\nfn checked(v:int)->int ! Bad:\n  v\n'
         'fn main()->[int] ! (Bad | TaskError):\n  try parallelMap([1,2],|checked)\n',
         'fn parallelMap(input: [int64], callback: fn(int64) -> int64 ! Bad) -> [int64] ! Bad | TaskError'),
    ]
    client.open(signature_document, 'fn main():\n  0\n')
    client.barrier()
    version = 1
    for marked, expected in map_cases:
        version += 1
        # Error unions use a pipe too; the editing marker is the final one.
        cursor = marked.rfind('|')
        text = marked[:cursor]+marked[cursor+1:]
        client.change(signature_document, text, version)
        assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
        line = marked[:cursor].count('\n')
        character = len(marked[:cursor].rsplit('\n',1)[-1].encode('utf-16-le'))//2
        result = client.signature(signature_document, line, character)["result"]
        assert result["signatures"][0]["label"] == expected and result["activeParameter"] == 1, result
        parameters = result["signatures"][0]["parameters"]
        assert len(parameters) == 2, result
        start,end = parameters[1]["label"]
        assert expected[start:end].startswith('callback: fn('), result
        assert not client.barrier(), "map hint published diagnostics"
    callback_library = Path(temp) / 'map-signatures.k'
    callback_library_text = 'export fn convert(v:int)->float32:\n  float32(v)\n'
    callback_library.write_text(callback_library_text)
    version += 1
    imported_map = 'from "./map-signatures.k" import convert\nfn main():\n  map([1,2],convert)\n'
    client.change(signature_document, imported_map, version)
    client.barrier()
    cursor = imported_map.splitlines()[2].index(',')+1
    result = client.signature(signature_document, 2, cursor)["result"]
    assert result["signatures"][0]["label"] == 'fn map(input: [int64], callback: fn(int64) -> float32) -> [float32]', result
    client.open(callback_library, 'export fn convert(v:int)->Bool:\n  v>0\n')
    client.barrier()
    result = client.signature(signature_document, 2, cursor)["result"]
    assert result["signatures"][0]["label"] == 'fn map(input: [int64], callback: fn(int64) -> Bool) -> [Bool]', result
    client.change(callback_library, 'export fn convert(v:int)->Bool:\n  missing\n', 2)
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"] is None
    client.change(callback_library, callback_library_text, 3)
    client.barrier()
    assert client.signature(signature_document, 2, cursor)["result"] is not None
    assert not signature_document.exists() and callback_library.read_text() == callback_library_text
    client.finish()

    # Builtin hover shares checked call signatures and original source-token ranges.
    client = Client()
    client.init(formats=["plaintext"])
    builtin_hover_cases = [
        ('fn main():\n  length([1,2])\n', 'length', 'fn length(input: [int64]) -> int64'),
        ('fn main():\n  [1,2] -> length\n', 'length', 'fn length(input: [int64]) -> int64'),
        ('fn main():\n  "😀abc" -> slice(0,1)\n', 'slice',
         'fn slice(input: String, start: int64, end: int64) -> String'),
        ('fn main():\n  float32(1)\n', 'float32', 'fn float32(input: int64) -> float32'),
        ('fn main()->[int] ! TaskError:\n  offset=3\n  try parallelMap([1,2],fn(v):v+offset)\n', 'parallelMap',
         'fn parallelMap(input: [int64], callback: fn(int64) -> int64) -> [int64] ! TaskError'),
    ]
    client.open(signature_document, 'fn main():\n  0\n')
    client.barrier()
    version = 1
    for text, name, expected in builtin_hover_cases:
        version += 1
        client.change(signature_document, text, version)
        assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
        at = text.index(name)
        line = text[:at].count('\n')
        start = len(text[:at].rsplit('\n',1)[-1].encode('utf-16-le'))//2
        result = client.hover(signature_document, line, start+1)["result"]
        assert result == {"contents":{"kind":"plaintext","value":expected}, "range":{
            "start":{"line":line,"character":start},"end":{"line":line,"character":start+len(name)}}}, result
        assert not client.barrier(), "builtin hover published probe diagnostics"
    hover_library = Path(temp) / 'builtin-hover.k'
    hover_library_text = 'export fn read()->String:\n  "abc"\n'
    hover_library.write_text(hover_library_text)
    version += 1
    imported_hover = 'from "./builtin-hover.k" import read\nfn main():\n  length(read())\n'
    client.change(signature_document, imported_hover, version)
    client.barrier()
    assert client.hover(signature_document, 2, 3)["result"]["contents"]["value"] == 'fn length(input: String) -> int64'
    client.open(hover_library, 'export fn read()->[int32]:\n  [1,2]\n')
    client.barrier()
    assert client.hover(signature_document, 2, 3)["result"]["contents"]["value"] == 'fn length(input: [int32]) -> int64'
    client.change(hover_library, 'export fn read()->String:\n  missing\n', 2)
    client.barrier()
    assert client.hover(signature_document, 2, 3)["result"] is None
    client.change(hover_library, hover_library_text, 3)
    client.barrier()
    assert client.hover(signature_document, 2, 3)["result"] is not None
    version += 1
    client.change(signature_document, 'fn main():\n  concat([1],)\n', version)
    diagnostics = publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    assert diagnostics and any(d["code"] == 'S027' for d in diagnostics), diagnostics
    assert client.hover(signature_document, 1, 3)["result"] is None
    assert not client.barrier(), "failed builtin hover changed diagnostics"
    assert not signature_document.exists() and hover_library.read_text() == hover_library_text
    client.finish()
    client = Client()
    client.init(formats=["markdown"])
    client.open(signature_document, 'fn main():\n  float32(1)\n')
    client.barrier()
    result = client.hover(signature_document, 1, 3)["result"]
    assert result["contents"] == {"kind":"markdown","value":'```kalandra\nfn float32(input: int64) -> float32\n```'}, result
    client.finish()

    # Device-selector completion is placement-gated and performs no hardware query.
    selector_text = 'fn main():\n  target=devices.gSuffix(1)\n  0\n'
    client = Client('--placement')
    client.init()
    client.open(signature_document, selector_text)
    client.barrier()
    start = selector_text.splitlines()[1].index('gSuffix')
    result = client.completion(signature_document, 1, start+1)["result"]
    assert [item["label"] for item in result["items"]] == ["gpu"], result
    item = result["items"][0]
    assert item["kind"] == 3 and item["textEdit"] == {"newText":"gpu", "range":{
        "start":{"line":1,"character":start},"end":{"line":1,"character":start+7}}}, item
    assert not client.barrier(), "selector completion published diagnostics"
    client.change(signature_document, selector_text.replace('gSuffix','gpu'), 2)
    assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    # Even an index which is not present on the machine is only source-checked.
    client.change(signature_document, selector_text.replace('gSuffix','gpu').replace('(1)','(4294967295)'), 3)
    assert not publications(client.barrier())[signature_document.as_uri()]["diagnostics"]
    empty_selector = 'fn main():\n  target=devices.()\n  0\n'
    client.change(signature_document, empty_selector, 4)
    client.barrier()
    cursor = empty_selector.splitlines()[1].index('devices.')+len('devices.')
    result = client.completion(signature_document, 1, cursor)["result"]
    assert [(item["label"],item["kind"]) for item in result["items"]] == [("cpu",3),("gpu",3)], result
    bare_selector = 'fn main():\n  devices.\n'
    client.change(signature_document, bare_selector, 5)
    client.barrier()
    result = client.completion(signature_document, 1, len(bare_selector.splitlines()[1]))["result"]
    assert [item["label"] for item in result["items"]] == ["cpu","gpu"], result
    shadow_selector = 'fn main():\n  devices=1\n  target=devices.gSuffix()\n  0\n'
    client.change(signature_document, shadow_selector, 6)
    client.barrier()
    assert client.completion(signature_document, 2, shadow_selector.splitlines()[2].index('gSuffix')+1)["result"]["items"] == []
    selector_library = Path(temp) / 'selector-completion.k'
    selector_library_text = 'export fn choose():\n  target=devices.gpu()\n  0\n'
    selector_library.write_text(selector_library_text)
    imported_selector = ('from "./selector-completion.k" import choose\nfn main():\n'
                         '  target=devices.gSuffix()\n  choose()\n')
    client.change(signature_document, imported_selector, 7)
    client.barrier()
    assert client.completion(signature_document, 2, imported_selector.splitlines()[2].index('gSuffix')+1)["result"]["items"][0]["label"] == 'gpu'
    assert not signature_document.exists() and selector_library.read_text() == selector_library_text
    client.finish()
    client = Client()
    client.init()
    client.open(signature_document, selector_text)
    client.barrier()
    assert client.completion(signature_document, 1, start+1)["result"]["items"] == []
    client.finish()

    # Transfer guidance uses checked placement IR, without enumerating hardware.
    client = Client('--placement')
    client.init(formats=['markdown'], signature_offsets=True)
    transfer_text = ('# 👩\nfn main()->int ! TaskError:\n  try parallel:\n'
                     '    values=tensor([1,2])\n'
                     '    resident=try await transfer(values,devices.gpu(1))\n'
                     '    host=try await transfer(resident,devices.cpu())\n    reduceSum(host)\n')
    client.open(signature_document, transfer_text)
    assert not publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    transfer_labels = [
        'fn transfer(input: Tensor[int64, 2], target: Device[gpu:1]) -> task<Tensor[int64, 2] on gpu:1>',
        'fn transfer(input: Tensor[int64, 2] on gpu:1, target: Device[cpu]) -> task<Tensor[int64, 2]>',
    ]
    for line, expected in zip([4,5], transfer_labels):
        text_line = transfer_text.splitlines()[line]
        start = text_line.index('transfer')
        hover = client.hover(signature_document, line, start+1)['result']
        assert hover['contents'] == {'kind':'markdown', 'value':'```kalandra\n'+expected+'\n```'}, hover
        assert hover['range'] == {'start':{'line':line,'character':start},
                                  'end':{'line':line,'character':start+8}}, hover
        result = client.signature(signature_document, line, text_line.index(',')+1)['result']
        assert result['signatures'][0]['label'] == expected and result['activeParameter'] == 1, result
        params = result['signatures'][0]['parameters']
        assert len(params) == 2
        for param in params:
            begin,end = param['label']
            assert expected[begin:end].startswith(('input:', 'target:')), result
    assert not client.barrier(), 'transfer queries published diagnostics'
    changed = transfer_text.replace('gpu(1)', 'gpu(4294967295)')
    client.change(signature_document, changed, 2)
    assert not publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    result = client.signature(signature_document, 4, changed.splitlines()[4].index(',')+1)['result']
    assert 'Device[gpu:4294967295]' in result['signatures'][0]['label'], result
    invalid = transfer_text.replace('devices.gpu(1)', '1')
    client.change(signature_document, invalid, 3)
    assert publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert client.hover(signature_document, 4, invalid.splitlines()[4].index('transfer')+1)['result'] is None
    assert client.signature(signature_document, 4, invalid.splitlines()[4].index(',')+1)['result'] is None
    assert not signature_document.exists()
    client.finish()
    client = Client()
    client.init()
    client.open(signature_document, transfer_text)
    assert publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert client.hover(signature_document, 4, transfer_text.splitlines()[4].index('transfer')+1)['result'] is None
    assert client.signature(signature_document, 4, transfer_text.splitlines()[4].index(',')+1)['result'] is None
    client.finish()

    # Complete a transfer call name only in an owning placement group.
    transfer_library = Path(temp) / 'transfer-completion.k'
    transfer_library_text = 'export fn read():\n  tensor([1,2])\n'
    transfer_library.write_text(transfer_library_text)
    transfer_completion_text = ('# 👩\nfrom "./transfer-completion.k" import read\n'
                                'fn main()->int ! TaskError:\n  try parallel:\n'
                                '    values=read()\n'
                                '    resident=try await traSuffix(values,devices.gpu(1))\n'
                                '    host=try await transfer(resident,devices.cpu())\n    reduceSum(host)\n')
    client = Client('--placement')
    client.init()
    client.open(signature_document, transfer_completion_text)
    original_diagnostics = publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert original_diagnostics
    start = transfer_completion_text.splitlines()[5].index('traSuffix')
    result = client.completion(signature_document, 5, start+3)['result']
    assert [(item['label'],item['kind']) for item in result['items']] == [('transfer',3),('transpose',3)], result
    item = next(item for item in result['items'] if item['label']=='transfer')
    assert item['textEdit'] == {'newText':'transfer','range':{
        'start':{'line':5,'character':start}, 'end':{'line':5,'character':start+9}}}, item
    assert not client.barrier(), 'transfer completion published probe diagnostics'
    applied = apply_workspace_edit({'documentChanges':[{'textDocument':{'uri':signature_document.as_uri()},
        'edits':[item['textEdit']]}]}, {signature_document.as_uri():transfer_completion_text})[signature_document.as_uri()]
    assert applied == transfer_completion_text.replace('traSuffix','transfer')
    client.change(signature_document, applied, 2)
    assert not publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert 'Device[gpu:1]' in client.hover(signature_document, 5, start+1)['result']['contents']['value']
    client.open(transfer_library, 'export fn read():\n  tensor([3,4,5])\n')
    assert not publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert 'Tensor[int64, 3]' in client.hover(signature_document, 5, start+1)['result']['contents']['value']
    client.change(signature_document, transfer_completion_text, 3)
    client.barrier()
    assert client.completion(signature_document, 5, start+3)['result']['items'][0]['label'] == 'transfer'
    for version, text in enumerate([
            'fn main():\n  traSuffix(tensor([1]),devices.gpu())\n',
            'fn main()->int ! TaskError:\n  try parallel:\n    spawn traSuffix(tensor([1]),devices.gpu())\n',
            'fn main()->int ! TaskError:\n  try parallel:\n    traSuffix\n',
        ], 4):
        client.change(signature_document, text, version)
        client.barrier()
        offset = text.index('traSuffix')+3
        line = text[:offset].count('\n')
        character = len(text[:offset].rsplit('\n',1)[-1])
        assert not any(item['label']=='transfer' for item in
                       client.completion(signature_document,line,character)['result']['items'])
    assert not signature_document.exists() and transfer_library.read_text() == transfer_library_text
    client.finish()
    client = Client()
    client.init()
    client.open(signature_document, transfer_completion_text)
    client.barrier()
    assert not any(item['label']=='transfer' for item in
                   client.completion(signature_document,5,start+3)['result']['items'])
    client.finish()

    # Device-selector guidance follows checked call arity and exact selection.
    selector_hint_library = Path(temp) / 'selector-guidance.k'
    selector_hint_library_text = 'export fn choose():\n  other=devices.gpu(4)\n  0\n'
    selector_hint_library.write_text(selector_hint_library_text)
    client = Client('--placement')
    client.init(formats=['plaintext'], signature_offsets=True)
    client.open(signature_document, 'fn main():\n  0\n')
    client.barrier()
    for version, (form, expected, parameters) in enumerate([
            ('devices.cpu()', 'fn devices.cpu() -> Device[cpu]', []),
            ('devices.gpu()', 'fn devices.gpu() -> Device[gpu:0]', []),
            ('devices.gpu(1)', 'fn devices.gpu(index: uint32) -> Device[gpu:1]', ['index: uint32']),
            ('devices.gpu(4294967295)', 'fn devices.gpu(index: uint32) -> Device[gpu:4294967295]', ['index: uint32']),
        ], 2):
        text = ('# 👩\nfrom "./selector-guidance.k" import choose\nfn main():\n  target='+form+'\n  choose()\n')
        client.change(signature_document, text, version)
        assert not publications(client.barrier())[signature_document.as_uri()]['diagnostics']
        start = text.splitlines()[3].index('devices.')
        hover = client.hover(signature_document, 3, start+9)['result']
        assert hover['contents'] == {'kind':'plaintext','value':expected}, hover
        assert hover['range'] == {'start':{'line':3,'character':start},
                                  'end':{'line':3,'character':start+len('devices.cpu')}}, hover
        result = client.signature(signature_document,3,text.splitlines()[3].index('(')+1)['result']
        assert result['signatures'][0]['label'] == expected, result
        labels = result['signatures'][0]['parameters']
        assert [expected[p['label'][0]:p['label'][1]] for p in labels] == parameters, result
        assert result.get('activeParameter') == (0 if parameters else None), result
        assert not client.barrier(), 'selector guidance published diagnostics'
    invalid_selector = 'fn main():\n  index=1\n  target=devices.gpu(index)\n  0\n'
    client.change(signature_document, invalid_selector, 6)
    assert publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert client.hover(signature_document,2,18)['result'] is None
    assert client.signature(signature_document,2,21)['result'] is None
    client.finish()
    client = Client('--placement')
    client.init(formats=['markdown'])
    text = 'fn main():\n  target=devices.gpu(1)\n  0\n'
    client.open(signature_document,text)
    assert not publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert client.hover(signature_document,1,18)['result']['contents'] == {
        'kind':'markdown','value':'```kalandra\nfn devices.gpu(index: uint32) -> Device[gpu:1]\n```'}
    client.finish()
    client = Client()
    client.init()
    client.open(signature_document,text)
    assert publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert client.hover(signature_document,1,18)['result'] is None
    assert client.signature(signature_document,1,21)['result'] is None
    assert not signature_document.exists() and selector_hint_library.read_text() == selector_hint_library_text
    client.finish()

    # Callee-name completion can append bounded missing parentheses at EOF.
    callee_library = Path(temp) / 'open-callee.k'
    callee_library_text = 'export fn combine(left:int,right:int):\n  left+right\n'
    callee_library.write_text(callee_library_text)
    callee_text = ('# 👩\nfrom "./open-callee.k" import combine as calculate\n'
                   'fn main():\n  calSuffix(1,2\n')
    client = Client()
    client.init()
    client.open(signature_document,callee_text)
    before = publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert before and any(d['code']=='L005' for d in before), before
    result = client.completion(signature_document,3,5)['result']
    assert [(item['label'],item['kind']) for item in result['items']] == [('calculate',3)], result
    item = result['items'][0]
    assert item['textEdit'] == {'newText':'calculate','range':{
        'start':{'line':3,'character':2},'end':{'line':3,'character':11}}}, item
    assert not client.barrier(), 'callee recovery published probe diagnostics'
    applied = apply_workspace_edit({'documentChanges':[{'textDocument':{'uri':signature_document.as_uri()},
        'edits':[item['textEdit']]}]}, {signature_document.as_uri():callee_text})[signature_document.as_uri()]
    assert applied == callee_text.replace('calSuffix','calculate')
    client.change(signature_document,applied,2)
    after = publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert after and any(d['code']=='L005' for d in after), after
    assert client.hover(signature_document,3,4)['result'] is None
    completed = applied.rstrip()+')\n'
    client.change(signature_document,completed,3)
    assert not publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert client.hover(signature_document,3,4)['result'] is not None
    client.open(callee_library,'export fn combine(left:String,right:String):\n  left+right\n')
    client.barrier()
    client.change(signature_document,callee_text,4)
    client.barrier()
    assert client.completion(signature_document,3,5)['result']['items'][0]['label'] == 'calculate'
    client.change(signature_document,completed,5)
    assert publications(client.barrier())[signature_document.as_uri()]['diagnostics'], 'arguments must still check after real closure'
    rejected = ('from "./open-callee.k" import combine as calculate\nfn main():\n'
                '  value=1\n  calculate(vaSuffix\n')
    client.change(signature_document,rejected,6)
    client.barrier()
    assert client.completion(signature_document,3,14)['result']['items'][0]['label'] == 'value', 'final argument suffix uses checked callee context'
    assert not signature_document.exists() and callee_library.read_text()==callee_library_text
    client.finish()

    # Final argument suffixes use EOF-only repair and checked prefix/receiver context.
    argument_library = Path(temp) / 'argument-suffix.k'
    argument_library_text = ('export type Reading:\n  value:int\n'
                             'export fn read():\n  Reading {value:2}\n'
                             'export fn combine(left:int,right:int):\n  left+right\n')
    argument_library.write_text(argument_library_text)
    argument_text = ('from "./argument-suffix.k" import read, combine\nfn main():\n'
                     '  reading=read()\n  combine(1,reading.vaSuffix\n')
    client = Client()
    client.init()
    client.open(signature_document,argument_text)
    before = publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert before and any(d['code']=='L005' for d in before), before
    start = argument_text.splitlines()[3].index('vaSuffix')
    result = client.completion(signature_document,3,start+2)['result']
    assert [(item['label'],item['kind']) for item in result['items']] == [('value',5)], result
    item=result['items'][0]
    assert item['textEdit']=={'newText':'value','range':{
        'start':{'line':3,'character':start},'end':{'line':3,'character':start+8}}}, item
    assert client.signature(signature_document,3,start+2)['result']['signatures'][0]['label'] == 'fn combine(left: int64, right: int64) -> int64'
    assert client.hover(signature_document,3,start+1)['result'] is None
    assert not client.barrier(), 'argument suffix recovery published probe diagnostics'
    applied = apply_workspace_edit({'documentChanges':[{'textDocument':{'uri':signature_document.as_uri()},
        'edits':[item['textEdit']]}]}, {signature_document.as_uri():argument_text})[signature_document.as_uri()]
    assert applied==argument_text.replace('vaSuffix','value')
    client.change(signature_document,applied,2)
    after=publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert after and any(d['code']=='L005' for d in after), after
    closed=applied.rstrip()+')\n'
    client.change(signature_document,closed,3)
    assert not publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    client.open(argument_library,argument_library_text.replace('value','variance'))
    assert publications(client.barrier())[signature_document.as_uri()]['diagnostics'], 'closed old field follows live imports'
    client.change(signature_document,argument_text,4)
    client.barrier()
    assert client.completion(signature_document,3,start+2)['result']['items'][0]['label']=='variance'
    bad_prefix=argument_text.replace('combine(1,','combine("bad",')
    client.change(signature_document,bad_prefix,5)
    client.barrier()
    assert client.completion(signature_document,3,bad_prefix.splitlines()[3].index('vaSuffix')+2)['result']['items']==[]
    compound=argument_text.replace('reading.vaSuffix','2+reading.vaSuffix')
    client.change(signature_document,compound,6)
    client.barrier()
    assert client.completion(signature_document,3,compound.splitlines()[3].index('vaSuffix')+2)['result']['items'][0]['label']=='variance'
    assert not signature_document.exists() and argument_library.read_text()==argument_library_text
    client.finish()

    # Signature help preserves final name/member suffixes with EOF-only repair.
    signature_suffix_library = Path(temp) / 'signature-suffix.k'
    signature_suffix_library_text = ('export type Reading:\n  value:int\n'
                                     'export fn read():\n  Reading {value:2}\n'
                                     'export fn combine(left:int,right:int):\n  right\n')
    signature_suffix_library.write_text(signature_suffix_library_text)
    suffix_text = ('# 👩\nfrom "./signature-suffix.k" import read, combine as calculate\n'
                   'fn main():\n  reading=read()\n  calculate(1,reading.vaSuffix\n')
    client = Client()
    client.init(signature_offsets=True)
    client.open(signature_document,suffix_text)
    diagnostics=publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert diagnostics and any(d['code']=='L005' for d in diagnostics), diagnostics
    character=suffix_text.splitlines()[4].index('vaSuffix')+2
    result=client.signature(signature_document,4,character)['result']
    expected='fn calculate(left: int64, right: int64) -> int64'
    assert result['signatures'][0]['label']==expected and result['activeParameter']==1, result
    begin,end=result['signatures'][0]['parameters'][1]['label']
    assert expected[begin:end]=='right: int64', result
    assert client.hover(signature_document,4,4)['result'] is None
    assert not client.barrier(), 'signature EOF suffix repair published diagnostics'
    client.open(signature_suffix_library,signature_suffix_library_text.replace('right:int','right:float32'))
    client.barrier()
    result=client.signature(signature_document,4,character)['result']
    assert result['signatures'][0]['label']=='fn calculate(left: int64, right: float32) -> float32', result
    for version,text in enumerate([
            suffix_text.replace('calculate(1,','calculate("bad",'),
            suffix_text.replace('reading.vaSuffix','missing.vaSuffix'),
            suffix_text.replace('reading.vaSuffix','1+reading.vaSuffix'),
        ],2):
        client.change(signature_document,text,version)
        client.barrier()
        hint = client.signature(signature_document,4,text.splitlines()[4].index('vaSuffix')+2)['result']
        assert bool(hint) == (version == 4), (text, hint)  # Compound holes now retain checked signatures.
    assert not signature_document.exists() and signature_suffix_library.read_text()==signature_suffix_library_text
    client.finish()

    # Earlier argument completion validates callee/prefix even with missing later parameters.
    earlier_library=Path(temp)/'earlier-arguments.k'
    earlier_library_text=('export type Reading:\n  value:int\nexport fn read():\n  Reading {value:1}\n'
                          'export fn triple(first:int,second:int,third:int):\n  first+second+third\n')
    earlier_library.write_text(earlier_library_text)
    earlier_text=('from "./earlier-arguments.k" import read, triple\nfn main():\n'
                  '  reading=read()\n  triple(reading.vaSuffix,2\n')
    client=Client()
    client.init()
    client.open(signature_document,earlier_text)
    before=publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert before and any(d['code']=='L005' for d in before),before
    start=earlier_text.splitlines()[3].index('vaSuffix')
    result=client.completion(signature_document,3,start+2)['result']
    assert [(item['label'],item['kind']) for item in result['items']]==[('value',5)],result
    item=result['items'][0]
    assert item['textEdit']=={'newText':'value','range':{
        'start':{'line':3,'character':start},'end':{'line':3,'character':start+8}}},item
    assert client.signature(signature_document,3,start+2)['result']['activeParameter']==0, 'earlier signature slot retains checked parameter guidance'
    assert not client.barrier(),'earlier argument recovery published diagnostics'
    applied=apply_workspace_edit({'documentChanges':[{'textDocument':{'uri':signature_document.as_uri()},
        'edits':[item['textEdit']]}]},{signature_document.as_uri():earlier_text})[signature_document.as_uri()]
    assert applied==earlier_text.replace('vaSuffix','value')
    client.change(signature_document,applied,2)
    assert any(d['code']=='L005' for d in publications(client.barrier())[signature_document.as_uri()]['diagnostics'])
    client.change(signature_document,applied.rstrip()+')\n',3)
    diagnostics=publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert any(d['code']=='S027' for d in diagnostics), 'ordinary missing-argument check was changed'
    client.change(signature_document,applied.rstrip()+',3)\n',4)
    assert not publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    client.open(earlier_library,earlier_library_text.replace('value','variance'))
    client.barrier()
    client.change(signature_document,earlier_text,5)
    client.barrier()
    assert client.completion(signature_document,3,start+2)['result']['items'][0]['label']=='variance'
    invalid_prefix=earlier_text.replace('triple(reading.','triple("bad",reading.')
    client.change(signature_document,invalid_prefix,6)
    client.barrier()
    assert client.completion(signature_document,3,invalid_prefix.splitlines()[3].index('vaSuffix')+2)['result']['items']==[]
    assert not signature_document.exists() and earlier_library.read_text()==earlier_library_text
    client.finish()

    # Earlier signature slots preserve later arguments in the syntax and snapshot only the prefix.
    earlier_signature_library=Path(temp)/'earlier-signatures.k'
    earlier_signature_library_text=('export type Reading:\n  value:int\nexport fn read():\n  Reading {value:1}\n'
                                    'export fn triple(first:int,second:int,third:int):\n  first\n')
    earlier_signature_library.write_text(earlier_signature_library_text)
    earlier_signature_text=('from "./earlier-signatures.k" import read, triple as calculate\nfn main():\n'
                           '  reading=read()\n  calculate(reading.vaSuffix,2\n')
    client=Client()
    client.init(signature_offsets=True)
    client.open(signature_document,earlier_signature_text)
    before=publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert before and any(d['code']=='L005' for d in before),before
    position=earlier_signature_text.splitlines()[3].index('vaSuffix')+2
    result=client.signature(signature_document,3,position)['result']
    label='fn calculate(first: int64, second: int64, third: int64) -> int64'
    assert result['signatures'][0]['label']==label and result['activeParameter']==0,result
    begin,end=result['signatures'][0]['parameters'][0]['label']
    assert label[begin:end]=='first: int64',result
    assert client.hover(signature_document,3,position)['result'] is None
    assert not client.barrier(),'earlier signature query published internal diagnostics'
    balanced=earlier_signature_text.rstrip()+',3)\n'
    client.change(signature_document,balanced,2)
    real=publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert real and not any('signature snapshot' in d['message'] for d in real),real
    assert client.signature(signature_document,3,position)['result']['activeParameter']==0
    assert not client.barrier(),'balanced signature query changed actual diagnostics'
    client.open(earlier_signature_library,earlier_signature_library_text.replace('first:int','first:float32'))
    client.barrier()
    result=client.signature(signature_document,3,position)['result']
    assert result['signatures'][0]['label']=='fn calculate(first: float32, second: int64, third: int64) -> float32',result
    invalid_receiver=balanced.replace('reading.vaSuffix','missing.vaSuffix')
    client.change(signature_document,invalid_receiver,3)
    client.barrier()
    assert client.signature(signature_document,3,invalid_receiver.splitlines()[3].index('vaSuffix')+2)['result'] is None
    assert not signature_document.exists() and earlier_signature_library.read_text()==earlier_signature_library_text
    client.finish()

    # Empty earlier field slots require checked receiver projection before a comma.
    empty_earlier_library=Path(temp)/'empty-earlier-fields.k'
    empty_earlier_library_text=('export type Reading:\n  value:int\nexport fn read():\n  Reading {value:2}\n'
                                'export fn combine(left:int,right:int):\n  left+right\n')
    empty_earlier_library.write_text(empty_earlier_library_text)
    empty_earlier_text=('from "./empty-earlier-fields.k" import read, combine as calculate\nfn main():\n'
                        '  reading=read()\n  calculate(reading.,2)\n')
    client=Client()
    client.init(signature_offsets=True)
    client.open(signature_document,empty_earlier_text)
    before=publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert before, 'actual empty field must retain parser diagnostic'
    position=empty_earlier_text.splitlines()[3].index('reading.')+len('reading.')
    result=client.signature(signature_document,3,position)['result']
    expected='fn calculate(left: int64, right: int64) -> int64'
    assert result['signatures'][0]['label']==expected and result['activeParameter']==0,result
    begin,end=result['signatures'][0]['parameters'][0]['label']
    assert expected[begin:end]=='left: int64',result
    assert '__kalandra' not in str(result),result
    assert client.hover(signature_document,3,position)['result'] is None
    assert not client.barrier(),'empty earlier field query published probe diagnostics'
    client.change(signature_document,empty_earlier_text.replace('reading.,','reading.value,'),2)
    assert not publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    client.open(empty_earlier_library,empty_earlier_library_text.replace('left:int,right:int','left:uint32,right:uint32'))
    client.barrier()
    opened=empty_earlier_text.replace('reading.,2)','reading.,2')
    client.change(signature_document,opened,3)
    assert publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    result=client.signature(signature_document,3,position)['result']
    assert result['signatures'][0]['label']=='fn calculate(left: uint32, right: uint32) -> uint32',result
    unknown=opened.replace('reading.,','missing.,')
    client.change(signature_document,unknown,4)
    client.barrier()
    assert client.signature(signature_document,3,unknown.splitlines()[3].index('missing.')+len('missing.'))['result'] is None
    assert not signature_document.exists() and empty_earlier_library.read_text()==empty_earlier_library_text
    client.finish()

    # Arithmetic argument completion preserves the whole expression during EOF repair.
    expression_library=Path(temp)/'expression-completion.k'
    expression_library_text=('export type Reading:\n  value:int\nexport fn read():\n  Reading {value:2}\n'
                             'export fn combine(left:int,right:int):\n  left+right\n')
    expression_library.write_text(expression_library_text)
    expression_text=('from "./expression-completion.k" import read, combine\nfn main():\n'
                     '  reading=read()\n  combine(1,2*reading.vaSuffix\n')
    client=Client()
    client.init()
    client.open(signature_document,expression_text)
    before=publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    assert before and any(d['code']=='L005' for d in before),before
    start=expression_text.splitlines()[3].index('vaSuffix')
    result=client.completion(signature_document,3,start+2)['result']
    assert [(item['label'],item['kind']) for item in result['items']]==[('value',5)],result
    item=result['items'][0]
    assert item['textEdit']=={'newText':'value','range':{
        'start':{'line':3,'character':start},'end':{'line':3,'character':start+8}}},item
    hint=client.signature(signature_document,3,start+2)['result']
    assert hint and hint['activeParameter']==1, 'compound signature checks the prefix and receiver'
    assert client.hover(signature_document,3,start+1)['result'] is None
    assert not client.barrier(),'arithmetic completion published probe diagnostics'
    applied=apply_workspace_edit({'documentChanges':[{'textDocument':{'uri':signature_document.as_uri()},
        'edits':[item['textEdit']]}]},{signature_document.as_uri():expression_text})[signature_document.as_uri()]
    assert applied==expression_text.replace('vaSuffix','value')
    client.change(signature_document,applied,2)
    assert any(d['code']=='L005' for d in publications(client.barrier())[signature_document.as_uri()]['diagnostics'])
    client.change(signature_document,applied.rstrip()+')\n',3)
    assert not publications(client.barrier())[signature_document.as_uri()]['diagnostics']
    client.open(expression_library,expression_library_text.replace('value','variance'))
    client.barrier()
    client.change(signature_document,expression_text,4)
    client.barrier()
    assert client.completion(signature_document,3,start+2)['result']['items'][0]['label']=='variance'
    bad_prefix=expression_text.replace('combine(1,','combine("bad",')
    client.change(signature_document,bad_prefix,5)
    client.barrier()
    assert client.completion(signature_document,3,bad_prefix.splitlines()[3].index('vaSuffix')+2)['result']['items']==[]
    assert not signature_document.exists() and expression_library.read_text()==expression_library_text
    client.finish()

    # Format the current buffer only; the client independently applies the edit.
    formatter = Path(temp) / "format.k"
    disk = b"fn main():\n  99\n"
    formatter.write_bytes(disk)
    client = Client()
    client.init()
    text = '# 👩\r\nfn main( ):\r\n    "😀"'
    expected = '# 👩\nfn main():\n  "😀"\n'
    client.open(formatter, text)
    client.barrier()
    edits = client.formatting(formatter)["result"]
    assert edits == [{"range": {"start": {"line": 0, "character": 0},
        "end": {"line": 2, "character": 8}}, "newText": expected}], edits
    applied = apply_workspace_edit({"documentChanges": [{"textDocument": {"uri": formatter.as_uri()},
        "edits": edits}]}, {formatter.as_uri(): text})[formatter.as_uri()]
    assert applied == expected
    assert client.formatting(formatter, {"tabSize": 8, "insertSpaces": False})["result"] == edits
    assert not client.barrier(), "formatting published diagnostics or changed buffers"
    assert formatter.read_bytes() == disk
    client.change(formatter, applied, 2)
    client.barrier()
    assert client.formatting(formatter)["result"] == [], "formatter not idempotent"
    client.change(formatter, 'fn main(\n', 3)
    client.barrier()
    assert client.formatting(formatter)["result"] == [], "invalid source offered edits"
    # Formatting is syntax-only; imports need not be available to format a file.
    client.change(formatter, 'from "./missing.k" import helper\nfn main():\n    helper( 1 )\n', 4)
    client.barrier()
    edits = client.formatting(formatter)["result"]
    assert len(edits) == 1 and edits[0]["newText"].endswith('  helper(1)\n')
    client.change(formatter, 'fn main():\n  devices . gpu(1)\n', 5)
    client.barrier()
    assert client.formatting(formatter)["error"]["code"] == -32803, "verification refusal lost"
    client.change(formatter, '', 6)
    client.barrier()
    assert client.formatting(formatter)["result"] == []
    for options in ({}, {"tabSize": 0, "insertSpaces": True},
                    {"tabSize": True, "insertSpaces": True}, {"tabSize": 2}):
        assert client.formatting(formatter, options)["error"]["code"] == -32602
    client.close(formatter)
    client.barrier()
    assert client.formatting(formatter)["result"] == []
    assert formatter.read_bytes() == disk
    client.finish()

    invalid_stream(b"Content-Length: 0\r\n\r\n")
    invalid_stream(b"Content-Length: 33554433\r\n\r\n")
    invalid_stream(b"Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}")
    invalid_stream(b"Content-Length: nope\r\n\r\n")
    invalid_stream(b"X" * 8193)
    invalid_stream(b"Content-Length: 12\r\n\r\n{}")
    invalid_stream(b"Content-Length: 2\r\nContent-Type: application/vscode-jsonrpc; charset=latin1\r\n\r\n{}")
    invalid_stream(frame({"jsonrpc": "2.0", "method": "exit"}), 1)
    invalid_stream(b"", 1)
    invalid_stream(b"Content-Length: 2\r\n")

print("Native LSP: lifecycle, full sync, versions, dependency diagnostics, Unicode URIs/ranges, "
      "project mappings, source isolation, limits and malformed framing passed")
