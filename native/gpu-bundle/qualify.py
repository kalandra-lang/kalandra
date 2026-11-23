"""Compile once and exercise the real application with and without a GPU request."""
import argparse
import os
from pathlib import Path
import subprocess

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vulkan", action="store_true")
    args = parser.parse_args()
    source = Path(__file__).with_name("example.k")
    subprocess.run([args.compiler, source, "--target", "cuda,vulkan", "-O2", "-o", args.output],
                   check=True, timeout=300)
    assert list(args.output.glob("*.ptx")) and list(args.output.glob("*.spv"))
    program = args.output / ("program.exe" if os.name == "nt" else "program")
    env = dict(os.environ, KALANDRA_GPU_BACKEND="invalid", KALANDRA_GPU_TRACE="1")
    cpu = subprocess.run([program], env=env, capture_output=True, timeout=30)
    assert cpu.returncode == 0 and cpu.stdout.strip() == b"cpu" and not cpu.stderr, cpu
    missing = subprocess.run([program, "gpu"], env=env, capture_output=True, timeout=30)
    assert missing.returncode != 0 and missing.stdout.splitlines() == [b"dispatch"], missing
    if args.vulkan:
        env["KALANDRA_GPU_BACKEND"] = "vulkan"
        gpu = subprocess.run([program, "gpu"], env=env, capture_output=True, timeout=45)
        assert gpu.returncode == 0 and gpu.stdout.splitlines() == [b"dispatch", b"42"], gpu
        assert b"selected vulkan" in gpu.stderr, gpu
if __name__ == "__main__":
    main()
