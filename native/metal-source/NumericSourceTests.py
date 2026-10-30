"""Source-to-device typed maps, checked faults and selected-path arithmetic."""
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
    concrete = ",".join(["1"] * (rank-1) + [str(len(values))])
    literal = "[" + ",".join(map(str, values)) + "]"
    for _ in range(rank-1):
        literal = "[" + literal + "]"
    source = (f"fn helper(value:{name})->{name}:\n  value*2\n"
              f"fn operation(values:Tensor[{name},{shape}],scalar:{name})->Tensor[{name},{shape}]:\n" +
              "".join("  " + line + "\n" for line in body.splitlines()) +
              f"fn main()->Tensor[{name},{shape}] ! TaskError:\n  try parallel:\n"
              "    gpu=devices.gpu()\n" +
              f"    values:Tensor[{name},{concrete}]=tensor({literal})\n"
              "    resident=try await transfer(values,gpu)\n" +
              f"    work=spawn operation(resident,{scalar}) on gpu\n"
              "    result=try await work\n"
              "    try await transfer(result,devices.cpu())\n")
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
                assert ran.returncode == 0 and all(line.endswith("Metal API Validation Enabled") for line in ran.stderr.splitlines()) and ran.stdout == encoded(name,width,expected), (path,ran,expected)
    programs += 1

for name,width in TYPES:
    check("arithmetic",name,width,"values*helper(scalar)+3",[1,3,5],[7,15,23],scalar="2")
    # Division by zero in an unselected arm must never fail, including empty tensors.
    body="safe=scalar==0 or 7/scalar>0\nif safe:\n  values+1\nelse:\n  values/scalar"
    check("lazy",name,width,body,[1,3,5],[2,4,6])
    check("empty",name,width,body,[],[])
    check("rank-eight",name,width,"values+scalar",[1,3,5],[3,5,7],rank=8,scalar="2")
    if not name.startswith("float"):
        maximum=(1 << (width if name.startswith("uint") else width-1))-1
        check("overflow",name,width,"values+scalar",[maximum],[],scalar="1",fault=True)
        check("uniform-empty-fault",name,width,"amount=7/scalar\nvalues+amount",[],[],fault=True)
    else:
        boundary=1 << (24 if width==32 else 53)
        check("rounding",name,width,"values+scalar",[boundary],[boundary],scalar="1")
print(f"Typed GPU source: {programs} programs, ten numeric types, rank eight, helpers, exact rounding, lazy choices and checked empty/overflow faults passed")
