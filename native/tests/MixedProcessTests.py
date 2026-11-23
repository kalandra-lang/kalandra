"""Feed captured child output to a device task through a backend-enabled package."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", type=Path, required=True)
parser.add_argument("--target", choices=("metal", "vulkan", "cuda"), required=True)
parser.add_argument("--compile-only", action="store_true")
args = parser.parse_args()
with tempfile.TemporaryDirectory() as name:
    source = Path(__file__).with_name("streams") / "mixed.k"
    for flag in ("-O0", "-O1", "-O2", "-O3"):
        bundle = Path(name) / flag
        subprocess.run([args.compiler.resolve(), source, "--target", args.target, flag, "-o", bundle],
                       check=True, timeout=240)
        if not args.compile_only:
            program = bundle / ("program.exe" if os.name == "nt" else "program")
            result = subprocess.run([program, sys.executable], capture_output=True, timeout=45)
            assert result.returncode == 0 and result.stdout.strip() == b"42", result
