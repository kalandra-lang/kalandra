"""Compile once and exercise the real application with and without a GPU request."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--physical", metavar="INTEL_ICD", type=Path,
                        help="also test automatic, CUDA, and Vulkan selection on the NVIDIA/Intel host")
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
    if args.physical:
        driver = {"VK_DRIVER_FILES": str(args.physical.resolve())}
        for override, hidden, backend, adapter in (
            ("", False, "cuda", "NVIDIA"), ("cuda", False, "cuda", "NVIDIA"),
            ("vulkan", False, "vulkan", "Intel"), ("", True, "vulkan", "Intel"),
        ):
            selected = dict(env, **driver, KALANDRA_GPU_BACKEND=override)
            if hidden:
                selected["CUDA_VISIBLE_DEVICES"] = "-1"
            for mode in ("compute", "fault"):
                result = subprocess.run([program, mode], env=selected, capture_output=True, timeout=45)
                trace = result.stderr.decode()
                assert trace.count("selected ") == 1 and f"selected {backend} ({adapter}" in trace, result
                lines = result.stdout.splitlines()
                if mode == "compute":
                    assert result.returncode == 0 and lines == [b"dispatch", b"42"], result
                else:
                    assert result.returncode != 0 and lines == [b"dispatch"] and "1/4" in trace, result
            print(f"{override or 'auto'} (CUDA hidden={hidden}): {backend} on {adapter}, output and fault passed")
        unavailable = dict(env, KALANDRA_GPU_BACKEND="", CUDA_VISIBLE_DEVICES="-1",
                           VK_DRIVER_FILES=str(args.output / "missing-icd.json"))
        failed = subprocess.run([program, "compute"],env=unavailable,capture_output=True,timeout=30)
        assert failed.returncode != 0 and failed.stdout.splitlines() == [b"dispatch"], failed
        assert b"selected " not in failed.stderr, failed

        # A malformed artifact on an otherwise usable device is an application
        # error. Replaying on the next backend would hide the damaged package.
        with tempfile.TemporaryDirectory() as root:
            damaged = Path(root) / "app"
            shutil.copytree(args.output,damaged)
            next(damaged.glob("*.ptx")).write_bytes(b"invalid CUDA artifact")
            bad = subprocess.run([damaged / program.name,"compute"],
                                 env=dict(env, **driver, KALANDRA_GPU_BACKEND=""),
                                 capture_output=True,timeout=30)
            assert bad.returncode != 0 and bad.stdout.splitlines() == [b"dispatch"], bad
            assert b"selected " not in bad.stderr and b"cuda" in bad.stderr, bad

if __name__ == "__main__":
    main()
