"""Closed synchronous entry versus frozen cancellable entry and policy controls.

The production standalone entry now owns an opaque scheduler. Only a fully
proved pure region with no independent progress source becomes indivisible.
Ordinary TaskCore execution retains cancellation checkpoints. All comparisons
use the same strict shaders and verify every output cell independently.
"""
import argparse
import json
import os
import shutil
import sys
from pathlib import Path

os.environ["OPENBLAS_NUM_THREADS"] = "1"
sys.path.insert(0, str(Path(__file__).resolve().parent / "fair"))
import protocol as p
import transformer_reference as ref

parser = argparse.ArgumentParser(description=__doc__)
for name in ("baseline", "prepared", "build", "qualification", "output"):
    parser.add_argument("--" + name, type=Path, required=True)
parser.add_argument("--study", choices=("prepare", "paired", "control", "ordinary", "matched", "repeat", "streaming"), required=True)
parser.add_argument("--blocks", type=int, default=7)
parser.add_argument("--target-ms", type=float, default=100)
a = parser.parse_args()
assert not a.output.exists()
for flag in ("MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION", "DYLD_INSERT_LIBRARIES", "KALANDRA_GPU_PROFILE"):
    assert os.getenv(flag) in (None, "", "0"), flag

baseline = a.baseline.resolve()
prepared = a.prepared.resolve()
build = a.build.resolve()
build.mkdir(parents=True, exist_ok=True)
fair = Path(__file__).resolve().parent / "fair"
program = p.ROOT / "benchmarks/.build/gpu-attention-2026-10-29/acceptance"
bundle = program / "bundle"
mps = p.ROOT / "benchmarks/.build/gpu-upload-2026-10-29/acceptance/mps"
compiler = p.ROOT / "build/native/native/kalandra"
metal = compiler.parent / "metal/libkalandra-metal-optimized.a"
task = compiler.parent / "libkalandra-task-core-optimized.a"
old = json.loads((baseline / "manifest.json").read_text())
for file, digest in old["artifacts"].items():
    assert p.sha(p.ROOT / file) == digest, file
qualified = json.loads(a.qualification.read_text())
assert qualified["failed"] == 0 and qualified["tests"] >= 1407
prepared_checks = json.loads((prepared / "qualification.json").read_text())
assert {row["mode"] for row in prepared_checks["records"]} == {"debug", "optimized", "sanitized", "thread-sanitized"}
assert all("independent progress passed" in row["closed_result"] for row in prepared_checks["records"])
shaders = [file for file in bundle.iterdir() if file.suffix in (".metal", ".mlir")]
assert shaders and all(p.sha(file) == p.sha(prepared / "bundle" / file.name) for file in shaders)
sources = [Path(__file__), fair / "TransformerClosedMetal.cpp", fair / "TransformerBlock.h",
           fair / "prepared_program.py", fair / "protocol.py", fair / "transformer_reference.py",
           fair / "ffn_reference.py", a.qualification.resolve(), prepared / "qualification.json",
           prepared / "TransformerProgram.h", prepared / "bundle/host.cpp"]
sources += [p.ROOT / file for file in (
    "native/runtime/Tasks.h", "native/runtime/Tasks.cpp", "native/metal/Metal.h", "native/metal/Metal.mm",
    "native/metal-source/Compiler.cpp", "native/metal-source/NumericProgram.h", "native/metal-source/NumericProgram.cpp",
    "native/tests/ClosedTaskTests.cpp", "native/tests/ClosedNumericProgramTests.cpp", "native/tests/PreparedNumericProgramTests.py")]
signature = {p.key(file): p.sha(file) for file in sources + [compiler, metal, task]}
manifest = build / "manifest.json"
if not manifest.exists():
    assert a.study == "prepare", "Prepare and verify all artifacts before acceptance timing"
    for source, name in ((compiler, "kalandra"), (metal, "metal.a"), (task, "task.a"),
                         (prepared / "TransformerProgram.h", "TransformerProgram.h"),
                         (fair / "TransformerClosedMetal.cpp", "Driver.cpp")):
        shutil.copy2(source, build / name)
    p.run([p.CLANG, "-std=c++23", "-O2", "-pthread", "-ffp-contract=off", "-mmacosx-version-min=26.0",
           "-I" + str(build), "-I" + str(fair), "-I" + str(p.ROOT / "native/metal"), build / "Driver.cpp",
           build / "metal.a", build / "task.a", "-framework", "Foundation", "-framework", "Metal", "-o", build / "gpu"])
    p.run([compiler, p.ROOT / "native/benchmarks/TransformerBlock.k", "--test-entry", "block",
           "--arithmetic", "hardware", "-O2", "--emit-llvm", build / "cpu.ll", "-o", build / "cpu-standalone"])
    assert p.sha(build / "cpu.ll") == p.sha(program / "cpu.ll")
    artifacts = [build / name for name in ("gpu", "kalandra", "metal.a", "task.a", "TransformerProgram.h", "Driver.cpp", "cpu.ll")]
    artifacts += [baseline / "gpu", mps, program / "cpu", *bundle.glob("*.metallib"), *shaders]
    manifest.write_text(json.dumps(dict(signature=signature,
        artifacts={p.key(file): p.sha(file) for file in artifacts}, commands=p.COMMANDS), indent=2) + "\n")
saved = json.loads(manifest.read_text())
assert saved["signature"] == signature
for file, digest in saved["artifacts"].items():
    assert p.sha(p.ROOT / file) == digest, file

shapes = [(1, 64), (16, 64), (64, 64), (1, 256), (16, 256), (64, 256), (1, 512), (16, 512), (64, 512)]
policies = {"ordinary": 0, "bounded": 1, "closed": 2}


def execute(backend, m, d, mode, count=1, rounds=1, dump=0, pattern=0, streaming=False):
    if backend == "old":
        command = [baseline / "gpu", bundle, m, d, count, rounds, mode, dump, pattern, 1, 1, 1, 1, 1, 1]
    elif backend == "cpu":
        command = [program / "cpu", m, d, count, rounds, 0, dump, pattern]
    elif backend == "mps":
        command = [mps, m, d, count, rounds, mode, dump, pattern, 1]
    else:
        command = [build / "gpu", bundle, m, d, count, rounds, mode, dump, pattern, policies[backend], int(not streaming)]
    output = p.run(command)
    if dump:
        return json.loads(output)
    rows = p.observations(output, m * d * 4, count, rounds)
    if backend in policies or backend == "old":
        assert all(row["gpu_ns"] == 0 and row["encoding_ns"] == 0 and row["dispatches"] == 0
                   for row in rows), "Acceptance must be unprofiled"
        if not streaming and backend == "closed":
            assert all(row["submissions"] == count for row in rows), (backend, m, d, mode, rows)
    return rows


verification_file = build / "verification.json"
if a.study == "prepare":
    assert not verification_file.exists()
    verification = []
    for m, d, pattern in [(2, 3, 1), (3, 7, 1), (5, 17, 1), (17, 33, 1)] + [(m, d, 0) for m, d in shapes]:
        reference = execute("old", m, d, 0, dump=1, pattern=pattern)
        for backend in ("old", "ordinary", "bounded", "closed", "cpu", "mps"):
            for mode in ((0,) if backend == "cpu" else range(3)):
                result = execute(backend, m, d, mode, dump=1, pattern=pattern)
                if backend not in ("cpu", "mps"):
                    assert result == reference
                metrics, _ = ref.assess(m, d, pattern, result)
                metrics.update(backend=backend, m=m, d=d, mode=mode, pattern=pattern,
                               bit_identical_to_baseline=result == reference)
                verification.append(metrics)
        for backend in ("bounded", "closed"):
            for mode in range(3):
                result = execute(backend, m, d, mode, dump=1, pattern=pattern, streaming=True)
                metrics, _ = ref.assess(m, d, pattern, result)
                metrics.update(backend=backend, m=m, d=d, mode=mode, pattern=pattern, streaming=True,
                               bit_identical_to_baseline=result == reference)
                assert result == reference
                verification.append(metrics)
        print(f"{m}x{d}: frozen/ordinary/bounded/closed/streaming bits match; independent CPU/MPS oracle checks pass", flush=True)
    verification_file.write_text(json.dumps(verification, indent=2) + "\n")
    result = dict(prepared=True)
else:
    assert verification_file.exists()
    if a.study == "matched":
        cells = [(backend, m, d, mode) for m, d in shapes for backend in ("cpu", "closed", "mps")
                 for mode in ((0,) if backend == "cpu" else range(3))]
        result = p.measure(cells, lambda c, n, r: execute(*c, n, r), lambda c: c[1] * c[2] * 4,
                           processes=a.blocks, rounds=3, target_ms=a.target_ms, adaptive=True, max_iterations=5000)
    else:
        cases = [(1, 512), (64, 256), (16, 512), (64, 512)] if a.study == "repeat" else [(1, 64), (1, 512)] if a.study == "streaming" else shapes
        cells = [(version, m, d, mode) for m, d in cases for mode in range(3) for version in ("baseline", "current")]
        def invoke(cell, count, rounds):
            version, m, d, mode = cell
            if version == "baseline":
                backend = "bounded" if a.study in ("control", "streaming") else "old"
            else:
                backend = "ordinary" if a.study == "ordinary" else "closed"
            return execute(backend, m, d, mode, count, rounds, streaming=a.study == "streaming")
        result = p.measure_paired(cells, invoke, lambda c: c[1] * c[2] * 4, blocks=a.blocks,
                                 target_ms=a.target_ms, adaptive=True, max_iterations=5000)
verification = json.loads(verification_file.read_text())
assert signature == {p.key(file): p.sha(file) for file in sources + [compiler, metal, task]}
qualification_summary = dict(tests=qualified["tests"], failed=qualified["failed"],
                             skipped=qualified.get("skipped", []),
                             path=p.key(a.qualification.resolve()), sha256=p.sha(a.qualification))
result.update(contract=__doc__, study=a.study, verification=verification, qualification=qualification_summary,
              prepared_qualification=prepared_checks, unchanged_shader_texts={p.key(file): p.sha(file) for file in shaders},
              unchanged_cpu_llvm_sha256=p.sha(build / "cpu.ll"), target_ms=a.target_ms,
              timing="Warm strict whole-block wall time including selected fresh owned uploads and cleanup. Setup is outside timing. Closed entry has no external cancellation capability; ordinary TaskCore remains cancellable. MPSGraph is a reference only.")
p.evidence(a.output, result, sources + [manifest, verification_file],
           [p.ROOT / file for file in saved["artifacts"]], compiler)
