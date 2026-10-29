"""Diagnostic encoder timestamps in a copied Metal runtime; never acceptance timing.

Build-only is safe during qualification. Run the probe only after all builds,
tests and acceptance measurements have finished. Counter sampling changes GPU
execution overhead; inclusive pass intervals are not an exclusive wall-time
decomposition. Arithmetic and dispatch geometry are unchanged.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "fair"))
import protocol as p

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--prepared", type=Path, required=True)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--bundle", type=Path, required=True)
parser.add_argument("--reference", type=Path)
parser.add_argument("--output", type=Path)
parser.add_argument("--build-only", action="store_true")
a = parser.parse_args()
build = a.build.resolve()
build.mkdir(parents=True, exist_ok=True)
fair = Path(__file__).resolve().parent / "fair"
source = p.ROOT / "native/metal/Metal.mm"
task = p.ROOT / "build/native/native/libkalandra-task-core-optimized.a"
inputs = [Path(__file__), fair / "DispatchCounters.inc", fair / "TransformerClosedMetal.cpp",
          source, p.ROOT / "native/metal/Metal.h", p.ROOT / "native/runtime/Tasks.h",
          p.ROOT / "native/runtime/Tasks.cpp", a.prepared / "TransformerProgram.h", task]
signature = {p.key(file): p.sha(file) for file in inputs}
manifest = build / "manifest.json"
if not manifest.exists():
    text = source.read_text()
    namespace = "namespace kalandra::runtime::metal {"
    assert text.count(namespace) == 1
    text = text.replace(namespace, '#include "DispatchCounters.inc"\n' + namespace)
    call = "klCounterEncoder(command, f.packet.numericProgram ? f.packet.numericProgram->pc : UINT32_MAX)"
    assert text.count("[command computeCommandEncoder]") == 4
    assert text.count("[command computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial]") == 1
    text = text.replace("[command computeCommandEncoder]", call)
    text = text.replace("[command computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial]", call)
    text, count = re.subn(r"\[encoder\s+setComputePipelineState:([^;]+)\];",
                          lambda match: match[0] + "\n      klCounterLabel(" + match[1] + ");", text)
    assert count == 5
    assert text.count("[command waitUntilCompleted];") == 1
    text = text.replace("[command waitUntilCompleted];", "[command waitUntilCompleted];\n      klCounterFinish(command);")
    (build / "Counters.mm").write_text(text)
    driver = (fair / "TransformerClosedMetal.cpp").read_text()
    assert driver.count("m::Options options;") == 1
    (build / "Driver.cpp").write_text(driver.replace("m::Options options;", "m::Options options; options.profile = true;"))
    shutil.copy2(a.prepared / "TransformerProgram.h", build / "TransformerProgram.h")
    p.run([p.CLANG, "-std=c++23", "-O2", "-fobjc-arc", "-fblocks", "-fno-objc-msgsend-class-selector-stubs",
           "-mmacosx-version-min=26.0", "-I" + str(p.ROOT / "native/metal"), "-I" + str(fair),
           "-c", build / "Counters.mm", "-o", build / "counters.o"])
    p.run([p.CLANG, "-std=c++23", "-O2", "-pthread", "-ffp-contract=off", "-mmacosx-version-min=26.0",
           "-I" + str(build), "-I" + str(fair), "-I" + str(p.ROOT / "native/metal"), build / "Driver.cpp",
           build / "counters.o", task, "-framework", "Foundation", "-framework", "Metal", "-o", build / "probe"])
    artifacts = [build / name for name in ("Counters.mm", "Driver.cpp", "TransformerProgram.h", "counters.o", "probe")]
    manifest.write_text(json.dumps(dict(signature=signature, artifacts={p.key(file): p.sha(file) for file in artifacts},
                                        commands=p.COMMANDS), indent=2) + "\n")
saved = json.loads(manifest.read_text())
assert saved["signature"] == signature
for file, digest in saved["artifacts"].items():
    assert p.sha(p.ROOT / file) == digest, file
if a.build_only:
    print("Diagnostic counter probe compiled; no GPU execution or timing performed")
    raise SystemExit(0)
assert a.output and a.reference and not a.output.exists()
for flag in ("MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION", "DYLD_INSERT_LIBRARIES"):
    assert os.getenv(flag) in (None, "", "0"), flag


def invoke(m, d, dump, rounds, pattern=0, validate=False):
    command = [build / "probe", a.bundle, m, d, 1, rounds, 0, dump, pattern, 2, 1]
    env = dict(os.environ)
    if validate:
        env.update(MTL_DEBUG_LAYER="1", MTL_SHADER_VALIDATION="1")
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, env=env, timeout=120)
    assert result.returncode == 0, (command, result.returncode, result.stderr[-4000:])
    samples = [json.loads(line) for line in result.stderr.splitlines() if line.startswith('{"dispatch_counter_call":')]
    assert len(samples) == (1 if dump else rounds + 2), (command, samples)
    assert all(len(row["passes"]) == (11 if m == 1 else 12) for row in samples)
    return result.stdout, samples


checks = []
for m, d, pattern in [(2, 3, 1), (3, 7, 1), (5, 17, 1), (17, 33, 1), (1, 64, 0), (1, 512, 0), (16, 512, 0), (64, 512, 0)]:
    expected = json.loads(p.run([a.reference, a.bundle, m, d, 1, 1, 0, 1, pattern, 2, 1]))
    actual, samples = invoke(m, d, 1, 1, pattern, True)
    assert json.loads(actual) == expected
    checks.append(dict(m=m, d=d, pattern=pattern, bit_identical=True, passes=len(samples[0]["passes"])))
records = []
for process in range(5):
    for m, d in ((1, 64), (1, 512), (16, 512), (64, 512)):
        output, samples = invoke(m, d, 0, 7)
        rows = p.observations(output, m * d * 4, 1, 7)
        records.append(dict(process=process, m=m, d=d, samples=samples, observations=rows))
        print(f"{m}x{d} process {process}: encoder counter samples recorded", flush=True)
assert signature == {p.key(file): p.sha(file) for file in inputs}
p.evidence(a.output, dict(contract=__doc__, acceptance=False, verification=checks, records=records),
           inputs + [manifest], [p.ROOT / file for file in saved["artifacts"]] + [a.reference, *a.bundle.glob("*.metallib")],
           p.ROOT / "build/native/native/kalandra")
