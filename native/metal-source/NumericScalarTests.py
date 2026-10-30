"""Scalar-only GPU dispatch and rank-zero typed tensor maps and sums."""
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

TYPES = (("int8",8),("uint8",8),("int16",16),("uint16",16),("int32",32),
         ("uint32",32),("int64",64),("uint64",64),("float32",32),("float64",64))

def encoded(name, width, values):
    def bits(value):
        if name.startswith("float"):
            return int.from_bytes(struct.pack("<f" if width == 32 else "<d", value), "little")
        return int(value) & ((1 << width) - 1)
    return "[" + ",".join(f"{bits(v):0{width//4}x}" for v in values) + "]\n"

programs = 0

def check(label, name, width, body, values, expected, rank=1, scalar="0", fault=False):
    global programs
    if rank==0:
        argument=f"values:Tensor[{name}]"
        argument_value="resident,"
        construction=(f"    values:Tensor[{name}]=tensor(7)\n"
                      "    resident=try await transfer(values,gpu)\n")
    else:
        argument=""
        argument_value=""
        construction=""
    source = (f"fn operation({argument}{',' if argument else ''}scalar:{name})->{name}:\n" +
              "".join("  " + line + "\n" for line in body.splitlines()) +
              f"fn main()->{name} ! TaskError:\n  try parallel:\n"
              "    gpu=devices.gpu()\n" + construction +
              f"    try await spawn operation({argument_value}{scalar}) on gpu\n")
    path, destination = args.root / f"{name}-{label}.k", args.root / f"{name}-{label}"
    path.write_text(source)
    built = subprocess.run(list(map(str,[args.compiler,"--arithmetic","software",path,*flags,"-o",destination])),
                           capture_output=True,text=True,timeout=120)
    assert built.returncode == 0, (path,built.stdout,built.stderr)
    if args.variant:
        for workers in (1,2,4):
            ran = subprocess.run([destination/"program"],capture_output=True,text=True,timeout=60,
                                 env=dict(os.environ,KALANDRA_WORKERS=str(workers)))
            assert not any(t in ran.stderr for t in ("AddressSanitizer","ThreadSanitizer","runtime error:")), ran.stderr
            if fault:
                assert ran.returncode != 0 and ran.stdout == "" and "Metal task failed: 1/4" in ran.stderr, (path,ran)
            else:
                assert ran.returncode == 0 and all(line.endswith("Metal API Validation Enabled") for line in ran.stderr.splitlines()) and ran.stdout == encoded(name,width,expected)[1:-2]+"\n", (path,ran,expected)
    programs += 1

for name,width in TYPES:
    check("arithmetic",name,width,"(scalar*3+1)/2",[],[5],scalar="3")
    check("lazy",name,width,"if scalar==0:\n  1\nelse:\n  7/scalar",[],[1])
    check("rank-zero-sum",name,width,"reduceSum(values+scalar)",[],[9],rank=0,scalar="2")
    if not name.startswith("float"):
        maximum=(1 << (width if name.startswith("uint") else width-1))-1
        check("overflow",name,width,"scalar+1",[],[],scalar=str(maximum),fault=True)
        check("division",name,width,"7/scalar",[],[],fault=True)
    else:
        boundary=1 << (24 if width==32 else 53)
        check("rounding",name,width,"scalar+1",[],[boundary],scalar=str(boundary))
print(f"Typed GPU scalar tasks: {programs} programs, ten types, scalar-only dispatch, rank-zero sums, lazy arithmetic, exact rounding and checked faults passed")
