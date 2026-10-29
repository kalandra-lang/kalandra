"""Diagnostic only: bounded tasks versus indivisible and caller-blocking controls.

The two experimental controls deliberately remove the GPU work quantum; the
blocking control also blocks its task worker. Neither is a valid replacement
for the cancellable task API. They estimate whether a separately proved closed,
synchronous entry deserves implementation. Production sources are never edited.
Reported GPU intervals and encoding counters are inclusive diagnostics, not an
exclusive decomposition of wall time. MPSGraph remains a reference only.
"""
import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "fair"))
import protocol as p

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--evidence", type=Path, required=True)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
a = parser.parse_args()
assert not a.output.exists()
for flag in ("MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION", "DYLD_INSERT_LIBRARIES", "KALANDRA_GPU_PROFILE"):
    assert os.getenv(flag) in (None, "", "0"), flag
evidence = json.loads(a.evidence.read_text())


def verify():
    for collection in ("source_sha256", "artifact_sha256"):
        for file, digest in evidence[collection].items():
            assert p.sha(p.ROOT / file) == digest, file


verify()
build = a.build.resolve()
build.mkdir(parents=True, exist_ok=True)
frozen = p.ROOT / "benchmarks/.build/gpu-encoding-overlap-2026-10-29/acceptance"
bundle = p.ROOT / "benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle"
mps = p.ROOT / "benchmarks/.build/gpu-upload-2026-10-29/acceptance/mps"
source = p.ROOT / "native/metal/Metal.mm"
driver = (frozen / "Driver.cpp").read_text()
assert driver.count("m::Options options;") == 1
driver = driver.replace("m::Options options;", "m::Options options; options.profile=true;")
(build / "Driver.cpp").write_text(driver)
text = source.read_text()
needle = "if (f.progression && work) {\n                constexpr auto limit = UINT64_C(64) * 1024 * 1024;"
assert text.count(needle) == 1
# Change only prepared-progression checkpoint admission. Keep the ordinary
# graph limit, cost saturation and operation/fusion headroom unchanged.
indivisible = text.replace(needle, "if (f.progression && work) {\n                constexpr auto limit = UINT64_C(1) << 60;")
begin = indivisible.index("  static void submitCommand(Task &task, Frame &f,")
end = indivisible.index("  static void submitPreencodedProgression(", begin)
blocking = indivisible[:begin] + r'''
  // Benchmark-only closed-caller lower bound. Blocking a general task worker
  // here would break independent source-task progress. Never ship this patch.
  static void submitCommand(Task &task, Frame &f, id<MTLCommandBuffer> command) {
    auto state = f.packet.state;
    f.started = true;
    f.pendingCommand = nil;
    f.error = Error::Device;
    auto completion = task.suspendExternal();
    if (!completion) return;
    const auto ordinal = ++state->submissions;
    [command commit];
    [command waitUntilCompleted];
    {
      std::unique_lock lock(state->gateMutex);
      state->gate.wait(lock, [&] {
        return !state->hold && (!state->holdSubmission || state->holdSubmission != ordinal);
      });
    }
    if (state->options.profile && command.GPUEndTime >= command.GPUStartTime)
      state->gpuNanoseconds += std::uint64_t((command.GPUEndTime-command.GPUStartTime)*1e9);
    f.succeeded = command.status == MTLCommandBufferStatusCompleted &&
                  !state->failCompletion.exchange(false);
    ++state->completions;
    --state->inFlight;
    f.active = false;
    completion.signal();
  }
''' + indivisible[end:]
objects = {"bounded": frozen / "metal.a"}
generated = [build / "Driver.cpp"]
for name, content in (("indivisible", indivisible), ("blocking", blocking)):
    file = build / (name + ".mm")
    file.write_text(content)
    obj = build / (name + ".o")
    p.run([p.CLANG, "-std=c++23", "-O2", "-fobjc-arc", "-fblocks",
           "-fno-objc-msgsend-class-selector-stubs", "-mmacosx-version-min=26.0",
           "-I" + str(p.ROOT / "native/metal"), "-c", file, "-o", obj])
    generated.append(file)
    objects[name] = obj
for name, obj in objects.items():
    p.run([p.CLANG, "-std=c++23", "-O2", "-pthread", "-ffp-contract=off",
           "-mmacosx-version-min=26.0", "-I" + str(frozen),
           "-I" + str(p.ROOT / "native/benchmarks/fair"), build / "Driver.cpp",
           obj, frozen / "task.a", "-framework", "Foundation", "-framework", "Metal",
           "-o", build / name])


def command(backend, m, d, mode, count, rounds, dump=0, pattern=0):
    if backend == "mps":
        return [mps, m, d, count, rounds, mode, dump, pattern, 1]
    return [build / backend, bundle, m, d, count, rounds, mode, dump, pattern, 1, 1, 1, 1, 1, 1]


validation = dict(os.environ, MTL_DEBUG_LAYER="1", MTL_SHADER_VALIDATION="1")
checks = []
cases = sorted({(r["m"], r["d"], r["pattern"]) for r in evidence["verification"]})
assert len(cases) == 13
for m, d, pattern in cases:
    reference = json.loads(p.run([frozen / "gpu", bundle, m, d, 1, 1, 0, 1, pattern, 1, 1, 1, 1, 1, 1], env=validation))
    for name in objects:
        for mode in range(3):
            actual = json.loads(p.run(command(name, m, d, mode, 1, 1, 1, pattern), env=validation))
            assert actual == reference, (name, m, d, mode, pattern)
            checks.append(dict(backend=name, m=m, d=d, mode=mode, pattern=pattern, bit_identical=True))
    print(f"{m}x{d}: every output bit matches in all diagnostic controls", flush=True)


def invoke(cell, count, rounds):
    backend, m, d, mode = cell
    rows = p.observations(p.run(command(backend, m, d, mode, count, rounds)), m*d*4, count, rounds)
    assert all(row["gpu_ns"] > 0 for row in rows)
    if backend != "mps":
        commands = (5 if (m, d) == (64, 512) else 3 if (m, d) == (16, 512) else 1) if backend == "bounded" else 1
        assert all(row["submissions"] == commands*count for row in rows), (cell, count, rows)
        assert all(row["dispatches"] > 0 and row["encoding_ns"] > 0 for row in rows)
    return rows


cells = [(name, m, d, mode) for m, d in ((1, 64), (1, 512), (16, 512), (64, 512))
         for name in (*objects, "mps") for mode in range(3)]
result = p.measure(cells, invoke, lambda c: c[1]*c[2]*4, processes=5, rounds=3,
                   target_ms=30, adaptive=True, max_iterations=5000)
verify()
result.update(contract=__doc__, verification=checks,
              parent_evidence_sha256=p.sha(a.evidence),
              production_candidate=False, cancellation_qualification=False,
              source_snapshots={p.key(file): file.read_text() for file in generated})
p.evidence(a.output, result, [Path(__file__), Path(p.__file__), a.evidence, source, *generated],
           [*objects.values(), *[build/name for name in objects], frozen/"task.a", mps,
            *bundle.glob("*.metallib")], frozen/"kalandra")
