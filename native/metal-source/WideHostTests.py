"""Exact numeric host/worker/coordinator boundaries in mixed GPU source programs."""
import argparse
import os
from pathlib import Path
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--variant", choices=("debug", "optimized", "sanitized", "thread-sanitized"))
args = parser.parse_args()
args.root.mkdir(parents=True, exist_ok=True)
flags = [] if not args.variant else ["--target", "metal", {
    "debug": "-O0", "optimized": "-O2", "sanitized": "--sanitize",
    "thread-sanitized": "--thread-sanitize"}[args.variant]]

def run(command, expected=None, env=None):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=90, env=env)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    assert not any(t in result.stderr for t in ("AddressSanitizer", "ThreadSanitizer", "runtime error:")), result.stderr
    if expected is not None:
        assert result.stdout == expected, (command, result.stdout, expected)

def bits(name, width, value):
    if name.startswith("float"):
        return int.from_bytes(struct.pack("<f" if width == 32 else "<d", value), "little")
    return value & ((1 << width) - 1)

programs = 0
for name, width in (("int8", 8), ("uint8", 8), ("int16", 16), ("uint16", 16),
                    ("int32", 32), ("uint32", 32), ("int64", 64), ("uint64", 64),
                    ("float32", 32), ("float64", 64)):
    gpu = "fn total(values:Tensor[float32,?])->float32:\n  reduceSum(values)\n"
    scalar = (f"fn increment(value:{name})->{name}:\n  value+1\n" + gpu +
              f"fn coordinate(value:{name})->{name} ! TaskError:\n  try parallel:\n"
              "    work=spawn increment(value)\n    gpu=devices.gpu()\n"
              "    values:Tensor[float32,1]=tensor([1])\n"
              "    resident=try await transfer(values,gpu)\n"
              "    extra=try await spawn total(resident) on gpu\n"
              f"    result=try await work\n    result+{name}(extra)\n")
    spawned = scalar + (f"fn main()->{name} ! TaskError:\n  try parallel:\n"
                        "    work=spawn coordinate(6)\n    try await work\n")
    if name.startswith("float"):
        boundary = 1 << (24 if width == 32 else 53)
        expected_boundary = boundary
    else:
        expected_boundary = (1 << (width if name.startswith("uint") else width - 1)) - 1
        boundary = expected_boundary - 1
    exact = scalar.replace("tensor([1])", "tensor([0])") + (
        f"fn main()->{name} ! TaskError:\n  try coordinate({boundary})\n")
    tensor = (f"fn increment(values:Tensor[{name},?])->Tensor[{name},?]:\n  values+1\n" + gpu +
              f"fn main()->Tensor[{name},?] ! TaskError:\n  try parallel:\n"
              f"    values:Tensor[{name},3]=tensor([1,2,3])\n"
              "    work=spawn increment(values)\n    result=try await work\n    gpu=devices.gpu()\n"
              "    resident=try await transfer(result,gpu)\n"
              "    returned=try await transfer(resident,devices.cpu())\n"
              "    zero:Tensor[float32,1]=tensor([0])\n"
              "    z=try await transfer(zero,gpu)\n    amount=try await spawn total(z) on gpu\n"
              f"    returned+{name}(amount)\n")
    expected = {
        "spawned": f"{bits(name,width,8):0{width//4}x}\n",
        "boundary": f"{bits(name,width,expected_boundary):0{width//4}x}\n",
        "tensor": "[" + ",".join(f"{bits(name,width,v):0{width//4}x}" for v in (2,3,4)) + "]\n",
    }
    for label, source in (("spawned", spawned), ("boundary", exact), ("tensor", tensor)):
        path, destination = args.root / f"{name}-{label}.k", args.root / f"{name}-{label}"
        path.write_text(source)
        run([args.compiler,"--arithmetic","software", path, *flags, "-o", destination])
        if args.variant:
            for workers in (1, 2, 4):
                run([destination / "program"], expected[label], dict(os.environ, KALANDRA_WORKERS=str(workers)))
        programs += 1
print(f"Wide mixed host bridge: {programs} programs, ten numeric identities, independent coordinators, boundary literals, workers, conversions and typed GPU round trips passed")
