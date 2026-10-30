"""Balanced typed GPU reductions, exact scalar task results and checked trees."""
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
    shape = ",".join(["?"] * rank)
    result_type = name
    concrete = ",".join(["1"] * (rank-1) + [str(len(values))])
    literal = "[" + ",".join(map(str, values)) + "]"
    for _ in range(rank-1):
        literal = "[" + literal + "]"
    source = (f"fn helper(value:{name})->{name}:\n  value*2\n"
              f"fn operation(values:Tensor[{name},{shape}],scalar:{name})->{result_type}:\n" +
              "".join("  " + line + "\n" for line in body.splitlines()) +
              f"fn main()->{result_type} ! TaskError:\n  try parallel:\n"
              "    gpu=devices.gpu()\n" +
              f"    values:Tensor[{name},{concrete}]=tensor({literal})\n"
              "    resident=try await transfer(values,gpu)\n" +
              f"    work=spawn operation(resident,{scalar}) on gpu\n"
              "    try await work\n")
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
    check("odd",name,width,"reduceSum(values*scalar)",[1,2,3,4,5],[30],rank=3,scalar="2")
    check("empty",name,width,"reduceSum(values+scalar)",[],[0],rank=8,scalar="1")
    boundary=(1 << (24 if width==32 else 53)) if name.startswith("float") else (1 << (width if name.startswith("uint") else width-1))-1
    check("identity",name,width,"reduceSum(values)",[boundary],[boundary])
    if not name.startswith("float"):
        check("overflow",name,width,"reduceSum(values)",[boundary,1],[],fault=True)
        check("uniform-empty-fault",name,width,"amount=7/scalar\nreduceSum(values+amount)",[],[],fault=True)
    else:
        # Balanced adjacent addition: (large + 1) + (-large + 1) rounds each pair separately.
        check("balanced",name,width,"reduceSum(values)",[boundary,1,-boundary,1],[1])
print(f"Typed GPU reductions: {programs} programs, ten exact scalar identities, balanced odd trees, ranks three/eight, empty sums and checked faults passed")
