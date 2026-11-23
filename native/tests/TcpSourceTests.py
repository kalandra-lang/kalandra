"""Exercise native TCP against an independent peer using loopback only."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", type=Path, required=True)
parser.add_argument("--variant", choices=["debug", "optimized", "sanitized", "thread-sanitized"], required=True)
args = parser.parse_args()
flag = {"debug": "-O0", "optimized": "-O2", "sanitized": "--sanitize", "thread-sanitized": "--thread-sanitize"}[args.variant]
compiler = args.compiler.resolve()
source = Path(__file__).with_name("network") / "tcp.k"
payload = bytes(range(256)) * 8193
reply = bytes(reversed(range(256))) * 8195

@contextmanager
def application(arguments, directory, workers):
    proc = subprocess.Popen(arguments, cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=dict(os.environ, KALANDRA_WORKERS=str(workers)))
    try:
        yield proc
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=10)

def output_line(proc):
    with ThreadPoolExecutor(max_workers=1) as executor:
        line = executor.submit(proc.stdout.readline)
        try:
            return line.result(timeout=20)
        except TimeoutError:
            proc.kill()
            raise

def read_to_end(peer):
    chunks = []
    while chunk := peer.recv(65536):
        chunks.append(chunk)
    return b"".join(chunks)

def test_client(app, root, workers):
    with socket.socket() as listener:
        listener.settimeout(20)
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        started = time.monotonic()
        with application([app, "exchange", "localhost", str(listener.getsockname()[1])], root, workers) as proc:
            peer, _ = listener.accept()
            with peer, ThreadPoolExecutor(max_workers=1) as executor:
                peer.settimeout(20)
                # Both directions can fill their socket queues at the same time.
                rx = executor.submit(read_to_end, peer)
                peer.sendall(reply)
                peer.shutdown(socket.SHUT_WR)
                assert rx.result(timeout=20) == payload
            stdout, stderr = proc.communicate(timeout=20)
            assert proc.returncode == 0, (stdout, stderr)
            assert (root / "result.bin").read_bytes() == reply
            print(f"{workers} worker(s): full duplex verified {len(payload) + len(reply)} bytes in {time.monotonic()-started:.3f}s")

def test_server(app, root, host):
    with application([app, "serve", host], root, 1) as proc:
        line = output_line(proc)
        if not line.strip().isdigit():
            proc.kill()
            raise AssertionError((line, proc.communicate(timeout=10)))
        with socket.create_connection((host, int(line)), timeout=10) as peer:
            peer.sendall(payload[:99001])
            peer.shutdown(socket.SHUT_WR)
            assert read_to_end(peer) == payload[:99001]
        stdout, stderr = proc.communicate(timeout=10)
        assert proc.returncode == 0 and stdout.strip() == b"42", (stdout, stderr)

with tempfile.TemporaryDirectory(prefix="kalandra-tcp-") as directory:
    root = Path(directory)
    app = root / ("tcp.exe" if os.name == "nt" else "tcp")
    subprocess.run([compiler, source, flag, "-o", app], check=True, timeout=180)
    (root / "input.bin").write_bytes(payload)
    for workers in (1, 4):
        test_client(app, root, workers)
    test_server(app, root, "127.0.0.1")
    test_server(app, root, "::1")
    with application([app, "cancel"], root, 1) as proc:
        port = int(output_line(proc))
        with socket.create_connection(("127.0.0.1", port), timeout=10) as peer:
            peer.sendall(b"K")
            assert peer.recv(1) == b"K"
            # Leave the established connection open and make no further connections.
            # Closing the paused generator must cancel and join the outstanding accept.
            stdout, stderr = proc.communicate(timeout=10)
            assert proc.returncode == 0 and stdout.strip() == b"settled", (stdout, stderr)

    with socket.socket() as reserved:
        reserved.bind(("127.0.0.1", 0))
        with application([app, "probe", str(reserved.getsockname()[1])], root, 1) as proc:
            stdout, stderr = proc.communicate(timeout=10)
            assert proc.returncode == 0 and stdout.strip() == b"1111", (stdout, stderr)
    with application([app, "read"], root, 1) as proc:
        stdout, stderr = proc.communicate(timeout=10)
        assert proc.returncode == 0 and stdout.strip() == b"42:1:99:99:1", (stdout, stderr)

    for reset in (False, True):
        with socket.socket() as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            listener.settimeout(10)
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            # Allow the OS to deliver a reset independently of the short
            # deadline used to exercise a live peer that stops reading.
            mode = "reset" if reset else "fill"
            with application([app, mode, str(listener.getsockname()[1])], root, 1) as proc:
                peer, _ = listener.accept()
                with peer:
                    peer.settimeout(10)
                    assert peer.recv(1) == bytes([0])
                    if reset:
                        # A write to a reset peer must be an error, including on
                        # scheduler threads where SIGPIPE has its default action.
                        linger = struct.pack("HH" if os.name == "nt" else "ii", 1, 0)
                        peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
                        peer.close()
                    stdout, stderr = proc.communicate(timeout=15)
                    expected = (b"43", b"44") if reset else (b"42",)
                    assert proc.returncode == 0 and stdout.strip() in expected, (reset, stdout, stderr)

    # Owned socket capabilities must remain nominal even though they use the
    # same storage layout as pipes, buffers and other file resources.
    for name, body in {
        "moved stream": "  halves = try split(sock)\n  try close(sock)\n",
        "wrong half": "  halves = try split(sock)\n  reader = try takeReader(halves)\n  b = try byteBuffer(0)\n  try writeAll(reader, b)\n",
        "file close": "  try closeFile(sock)\n",
    }.items():
        bad = root / "invalid.k"
        bad.write_text('from "std/net" import listenTcp, localPort, connectTcp, split, takeReader, close, NetError\n'
                       'from "std/stream" import writeAll, StreamError\n'
                       'from "std/fs" import close as closeFile, FsError\n'
                       'from "std/buffer" import byteBuffer, BufferError\n'
                       'fn main() -> unit ! (NetError | FsError | StreamError | BufferError):\n'
                       '  sock = try connectTcp("localhost", 80)\n' + body + '  return ()\n')
        rejected = subprocess.run([compiler, bad, "-o", root / "invalid"], capture_output=True, timeout=60)
        assert rejected.returncode and b"S0" in rejected.stderr, (name, rejected.stderr)
