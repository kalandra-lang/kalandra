"""Paired -O2 file-copy timings with one worker and verified binary output."""
import argparse
import hashlib
import json
import os
from pathlib import Path
from statistics import median
import subprocess
import tempfile
from time import perf_counter_ns

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", type=Path, required=True)
args = parser.parse_args()
pump = """
from "std/buffer" import ByteBuffer, byteBuffer, BufferError

fn pump{generic}(source: {reader}, target: {writer}, mut chunk: ByteBuffer) -> unit ! {error}:
  while true:
    count = try readInto(source, chunk, 65536)
    if count == 0:
      break
    try writeAll(target, chunk)
  return ()

fn main() -> unit ! ({errors}):
  source = try open("source.bin", OpenMode.Read)
  target = try open("target.bin", OpenMode.Create)
  chunk = try byteBuffer(65536)
  try pump(source, target, chunk)
  try close(target)
  try close(source)
  return ()
"""
variants = {
    "direct": ('from "std/fs" import File, open, OpenMode, close, readInto, writeAll, FsError',
               "", "File", "File", "FsError", "FsError | BufferError"),
    "generic": ('from "std/fs" import open, OpenMode, close, FsError\n'
                'from "std/stream" import ByteReader, ByteWriter, readInto, writeAll, StreamError',
                "[R: ByteReader, W: ByteWriter]", "R", "W", "StreamError", "StreamError | FsError | BufferError"),
}
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    payload = bytes(range(256)) * 16384 + b"x"
    digest = hashlib.sha256(payload).digest()
    (root / "source.bin").write_bytes(payload)
    times = {key: [] for key in variants}
    apps = {}
    for name, (imports, generic, reader, writer, error, errors) in variants.items():
        src = root / (name + ".k")
        src.write_text(imports + pump.format(generic=generic, reader=reader, writer=writer, error=error, errors=errors))
        apps[name] = root / (name + (".exe" if os.name == "nt" else ""))
        subprocess.run([args.compiler.resolve(), src, "-O2", "-o", apps[name]], check=True, timeout=180)
    # First pair warms both paths. Reverse the order on alternate measured pairs.
    for i, order in enumerate([("direct", "generic")] + [("direct", "generic"), ("generic", "direct")] * 4):
        for name in order:
            begin = perf_counter_ns()
            subprocess.run([apps[name]], cwd=root, env=dict(os.environ, KALANDRA_WORKERS="1"), check=True, timeout=60)
            elapsed = (perf_counter_ns() - begin) / 1e6
            assert hashlib.sha256((root / "target.bin").read_bytes()).digest() == digest
            if i:
                times[name].append(round(elapsed, 3))
    print(json.dumps({"bytes": len(payload), "sha256": digest.hex(), "ms": times,
                      "median_ms": {key: median(values) for key, values in times.items()}}, indent=2))
