"""Framed stdio client shared by native LSP qualification fixtures."""
import json
import os
from pathlib import Path
import queue
import threading
import subprocess
import time

def frame(value):
    data = json.dumps(value, ensure_ascii=False).encode()
    return f"Content-Length: {len(data)}\r\n\r\n".encode() + data


class Client:
    def __init__(self, server, *args):
        self.process = subprocess.Popen([server, *args], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.output = queue.Queue()
        def receive():
            try:
                while True:
                    data = os.read(self.process.stdout.fileno(), 65536)
                    self.output.put(data)
                    if not data:
                        return
            except OSError as error:
                self.output.put(error)
        self.reader = threading.Thread(target=receive, daemon=True)
        self.reader.start()
        self.buffer = b""
        self.serial = 0
        self.events = []

    def send(self, method, params=None, ident=None):
        value = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            value["params"] = params
        if ident is not None:
            value["id"] = ident
        self.process.stdin.write(frame(value))
        self.process.stdin.flush()

    def read(self):
        deadline = time.monotonic() + 20
        while True:
            if b"\r\n\r\n" in self.buffer:
                header, body = self.buffer.split(b"\r\n\r\n", 1)
                length = int(header.decode().split(":", 1)[1].strip())
                if len(body) >= length:
                    self.buffer = body[length:]
                    value = json.loads(body[:length])
                    assert value["jsonrpc"] == "2.0"
                    return value
            remaining = deadline - time.monotonic()
            assert remaining > 0, "server response timed out"
            try:
                data = self.output.get(timeout=remaining)
            except queue.Empty:
                raise AssertionError("server stalled") from None
            if isinstance(data, OSError):
                raise data
            assert data, "server exited before response"
            self.buffer += data

    def rpc(self, method, params=None):
        self.serial += 1
        self.send(method, params, self.serial)
        while True:
            value = self.read()
            if value.get("id") == self.serial:
                return value
            self.events.append(value)

    def barrier(self):
        assert self.rpc("test/barrier")["error"]["code"] == -32601
        events, self.events = self.events, []
        return events

    def init(self, formats=None, rename=False, signature_offsets=False, **workspace):
        capabilities = {} if formats is None else {"textDocument": {"hover": {"contentFormat": formats}}}
        if signature_offsets:
            capabilities.setdefault("textDocument", {})["signatureHelp"] = {
                "signatureInformation": {"parameterInformation": {"labelOffsetSupport": True}}}
        if rename:
            capabilities["workspace"] = {"workspaceEdit": {"documentChanges": True}}
            capabilities.setdefault("textDocument", {})["rename"] = {"prepareSupport": True}
        result = self.rpc("initialize", {"capabilities": capabilities, **workspace})["result"]
        assert result["serverInfo"] == {"name": "kalandra-native", "version": (Path(__file__).resolve().parents[2] / "VERSION").read_text().strip()}
        caps = result["capabilities"]
        assert caps["positionEncoding"] == "utf-16"
        assert caps["textDocumentSync"]["change"] == 2  # Incremental UTF-16 changes.
        assert caps["hoverProvider"] is True
        assert caps["signatureHelpProvider"] == {"triggerCharacters": ["(", ","]}
        assert caps["definitionProvider"] is True
        assert caps["referencesProvider"] is True
        assert caps["renameProvider"] == ({"prepareProvider": True} if rename else False)
        assert caps["workspace"]["workspaceFolders"] == {"supported": True, "changeNotifications": False}
        assert caps["completionProvider"] == {"resolveProvider": False, "triggerCharacters": [".", "{", ",", "/"]}
        assert caps["documentFormattingProvider"] is True
        self.send("initialized", {})

    def open(self, path, text, version=1):
        self.send("textDocument/didOpen", {"textDocument": {
            "uri": path.as_uri(), "languageId": "kalandra", "version": version, "text": text}})

    def change(self, path, text, version):
        self.send("textDocument/didChange", {"textDocument": {
            "uri": path.as_uri(), "version": version}, "contentChanges": [{"text": text}]})

    def edits(self, path, changes, version):
        self.send("textDocument/didChange", {"textDocument": {
            "uri": path.as_uri(), "version": version}, "contentChanges": changes})

    def hover(self, path, line, character):
        return self.rpc("textDocument/hover", {"textDocument": {"uri": path.as_uri()},
                                              "position": {"line": line, "character": character}})

    def signature(self, path, line, character):
        return self.rpc("textDocument/signatureHelp", {"textDocument": {"uri": path.as_uri()},
            "position": {"line": line, "character": character}})

    def formatting(self, path, options=None):
        return self.rpc("textDocument/formatting", {"textDocument": {"uri": path.as_uri()},
            "options": options if options is not None else {"tabSize": 4, "insertSpaces": True}})

    def completion(self, path, line, character):
        return self.rpc("textDocument/completion", {"textDocument": {"uri": path.as_uri()},
            "position": {"line": line, "character": character}})

    def definition(self, path, line, character):
        return self.rpc("textDocument/definition", {"textDocument": {"uri": path.as_uri()},
                                                   "position": {"line": line, "character": character}})

    def references(self, path, line, character, include=False):
        return self.rpc("textDocument/references", {"textDocument": {"uri": path.as_uri()},
            "position": {"line": line, "character": character},
            "context": {"includeDeclaration": include}})

    def rename(self, path, line, character, name):
        uri = path.as_uri() if isinstance(path, Path) else path
        return self.rpc("textDocument/rename", {"textDocument": {"uri": uri},
            "position": {"line": line, "character": character}, "newName": name})

    def prepare_rename(self, path, line, character):
        uri = path.as_uri() if isinstance(path, Path) else path
        return self.rpc("textDocument/prepareRename", {"textDocument": {"uri": uri},
            "position": {"line": line, "character": character}})

    def close(self, path):
        self.send("textDocument/didClose", {"textDocument": {"uri": path.as_uri()}})

    def finish(self):
        assert self.rpc("shutdown")["result"] is None
        self.send("exit")
        self.process.stdin.close()
        assert self.process.wait(timeout=10) == 0
        assert self.process.stdout.read() == b"", "unexpected trailing protocol data"
        assert self.process.stderr.read() == b"", "unexpected server diagnostics/sanitizer output"


def edit(line, start, end, text, **extra):
    return {"range": {"start": {"line": line, "character": start},
                      "end": {"line": line, "character": end}}, "text": text, **extra}


def apply_workspace_edit(result, sources):
    # Independently interpret UTF-16 protocol edits against the original texts.
    output = dict(sources)
    def offset(text, position):
        lines = text.splitlines(keepends=True)
        prefix = lines[position["line"]].encode("utf-16-le")[:2*position["character"]].decode("utf-16-le")
        return sum(len(line) for line in lines[:position["line"]]) + len(prefix)
    for change in result["documentChanges"]:
        uri = change["textDocument"]["uri"]
        text = output[uri]
        replacements = [(offset(text, edit["range"]["start"]), offset(text, edit["range"]["end"]), edit["newText"])
                        for edit in change["edits"]]
        previous = len(text)+1
        for start, end, replacement in sorted(replacements, reverse=True):
            assert start < end <= previous, "overlapping or empty rename edit"
            text = text[:start] + replacement + text[end:]
            previous = start
        output[uri] = text
    return output


def publications(events):
    result = {}
    for value in events:
        if value.get("method") == "textDocument/publishDiagnostics":
            params = value["params"]
            result[params["uri"]] = params
    return result


