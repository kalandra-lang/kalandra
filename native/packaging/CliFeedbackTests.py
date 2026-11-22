"""Exercise the real command line with separate output streams and a terminal on stderr."""
import argparse
import errno
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("compiler", type=Path)
args = parser.parse_args()
compiler = args.compiler.resolve()
def tool(name):
    return compiler.with_name(name + compiler.suffix)

def run(*args, status=0, input_bytes=None):
    p = subprocess.run(args, input=input_bytes, capture_output=True, timeout=90, env={**os.environ, "CI": "1"})
    assert p.returncode == status, (args, p.returncode, p.stdout, p.stderr)
    # CRLF is ordinary text on Windows.
    assert b"\x1b" not in p.stderr and b"\r" not in p.stderr.replace(b"\r\n", b"\n"), p.stderr
    return p

def terminal(*args, gate=None, payload=None, status=0, environment=None):
    import pty
    import select
    master, slave = pty.openpty()
    env = dict(os.environ, TERM="xterm-256color")
    env.pop("CI", None)
    env.pop("NO_COLOR", None)
    env.update(environment or {})
    output = bytearray()
    sent = False
    try:
        started = time.monotonic()
        with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=slave, env=env) as p:
            os.close(slave)
            deadline = time.monotonic() + 60
            try:
                while True:
                    assert select.select([master], [], [], max(0, deadline-time.monotonic()))[0], output
                    try:
                        chunk = os.read(master, 8192)
                    except OSError as e:
                        if e.errno == errno.EIO:
                            break
                        raise
                    if not chunk:
                        break
                    if gate and not output:
                        assert time.monotonic() - started >= 0.19, chunk
                    output.extend(chunk)
                    if gate and not sent and b"Checking" in output:
                        # The compiler is reading this FIFO as its project manifest.
                        with gate.open("wb") as f:
                            f.write(payload)
                        sent = True
                assert p.wait(timeout=10) == status, output
                assert p.stdout.read() == b""
            finally:
                if p.poll() is None:
                    p.kill()
        assert not gate or sent
        return bytes(output)
    finally:
        os.close(master)

with tempfile.TemporaryDirectory() as tmp:
    previous = Path.cwd()
    os.chdir(tmp)
    try:
        src = Path("main.k")
        src.write_text("fn main() -> unit:\n  return ()\n")
        build = [compiler, src, "-o", "program" + compiler.suffix]
        result = run(*build)
        assert result.stdout == result.stderr == b""
        result = run(*build, "--debug")
        assert result.stdout == b""
        for phase in (b"Checking", b"Compiling", b"Linking"):
            assert re.search(phase + rb" took [0-9]+\.[0-9]+s", result.stderr), result.stderr
        assert b"Built  program" in result.stderr and b"cpu/" in result.stderr
        out = run(compiler, "--silent", "new", "app")
        assert out.stdout == out.stderr == b""
        manifest = Path("app/kalandra.toml")
        out = run(compiler, "install", "--project", manifest, "--silent", "--debug")
        assert out.stdout == b"" and b"kalandra.lock" in out.stderr
        out = run(compiler, "remove", "absent", "--silent", "--project", manifest, status=1)
        assert b"unknown direct dependency" in out.stderr
        assert b"--debug" in run(compiler, "--silent", "--help").stdout
        out = run(compiler, "--silent", "--version")
        assert out.stdout.startswith(b"kalandra ") and not out.stderr

        check = tool("kalandra-check")
        assert run(check, src, "--silent").stdout == b""
        assert run(tool("kalandra-parse"), "--silent", src).stdout == b""
        assert run(tool("kalandra-fmt"), src, "--silent").stdout == run(tool("kalandra-fmt"), src).stdout
        assert run(check, "--dump-ir", src, "--silent").stdout == run(check, "--dump-ir", src).stdout
        request = b'{"jsonrpc":"2.0","method":"unknown","id":7}'
        frame = b"Content-Length: %d\r\n\r\n" % len(request) + request
        server = tool("kalandra-lsp")
        plain = run(server, input_bytes=frame, status=1)
        traced = run(server, "--silent", "--debug", input_bytes=frame, status=1)
        header, body = traced.stdout.split(b"\r\n\r\n", 1)
        assert header == b"Content-Length: %d" % len(body)
        assert json.loads(body)["id"] == 7 and traced.stdout == plain.stdout
        assert b"[debug] Starting language server" in traced.stderr and not plain.stderr
        for valid in (True, False):
            if not valid:
                src.write_text("fn main() -> unit:\n  return missing\n")
            flags = ["--diagnostics=json", src]
            plain = run(check, *flags, status=int(not valid))
            traced = run(check, "--debug", *flags, "--silent", status=int(not valid))
            assert json.loads(traced.stdout) == json.loads(plain.stdout)
            assert b"[debug] Checking" in traced.stderr and not plain.stderr
        src.write_text("fn main() -> unit:\n  return ()\n")
        run(*build, "--emit-llvm", "--debug")
        assert Path("--debug").exists()

        if os.name != "nt":
            started = time.monotonic()
            quick = terminal(*build, environment={"NO_COLOR":"1"})
            if time.monotonic() - started < 0.2:
                assert b"Checking" not in quick and b"\r\x1b[2K" not in quick
            assert b"Built  program" in quick and b"\x1b[1;36m" not in quick
            assert terminal(*build, "--silent") == b""
            assert terminal(*build, environment={"CI":"true"}) == b""
            assert terminal(*build, environment={"TERM":"dumb"}) == b""
            gate = Path("slow.toml")
            os.mkfifo(gate)
            cmd = [compiler, "--project", gate, "-o", "program"]
            for valid, payload in [(True, b"format=1\n[project]\nentry='main.k'\n"),
                                   (False, b"format=0\n")]:
                text = terminal(*cmd, gate=gate, payload=payload, status=int(not valid))
                assert b"\r\x1b[2K" in text and b"\x1b[1;36m  Checking" in text, text
                if valid:
                    for phase in [b"Compiling", b"Linking", b"Built"]:
                        assert phase in text, text
                else:
                    assert b"Built" not in text
                    assert b"format" in text.split(b"\r\x1b[2K")[-1], text
    finally:
        os.chdir(previous)
print("CLI feedback: stream, status, package and terminal checks passed")
