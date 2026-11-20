"""Compile the public stream example and compare both destinations byte for byte."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", required=True, type=Path)
parser.add_argument("--variant", required=True, choices=["debug", "optimized", "sanitized"])
args = parser.parse_args()
flag = {"debug": "-O0", "optimized": "-O2", "sanitized": "--sanitize"}[args.variant]
source = Path(__file__).with_name("streams") / "copy.k"
with tempfile.TemporaryDirectory() as name:
    root = Path(name)
    app = root / ("copy.exe" if os.name == "nt" else "copy")
    subprocess.run([args.compiler.resolve(), source, flag, "-o", app], check=True, timeout=180)
    for data in [bytes(range(256)) * 520 + b"\x00\xff", b"", b"x"]:
        (root / "source.bin").write_bytes(data)
        subprocess.run([app], cwd=root, check=True, timeout=60)
        for destination in ("target.bin", "buffered.bin"):
            assert (root / destination).read_bytes() == data, destination
