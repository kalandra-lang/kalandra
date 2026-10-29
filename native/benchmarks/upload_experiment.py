"""Isolate shared upload storage with acknowledged or synchronous publication.

Benchmark-only source variants; neither aliases mutable host storage. The
synchronous alternative deliberately changes the external completion boundary
and is not a qualified production implementation.
"""
import argparse, io, json, os, subprocess, sys, tarfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p

parser = argparse.ArgumentParser(description=__doc__)
for name in ('evidence', 'build-dir', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
parser.add_argument('--policy', choices=('acknowledged', 'synchronous'), required=True)
parser.add_argument('--blocks', type=int, default=3)
args = parser.parse_args()
for name in ('MTL_DEBUG_LAYER', 'MTL_SHADER_VALIDATION', 'DYLD_INSERT_LIBRARIES'):
    assert os.getenv(name) in (None, '', '0'), name
assert not args.output.exists()
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=False)
data = json.loads(args.evidence.read_text())
accept = next(p.ROOT / f for f in data['artifact_sha256'] if f.endswith('/TransformerProgram.h')).parent
bundle = accept / 'bundle'
for f in [accept / 'gpu', *bundle.glob('*.metallib')]:
    assert p.sha(f) == data['artifact_sha256'][p.key(f)]
source = build / 'source'
source.mkdir()
with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['git', 'archive', 'dec5a66d', 'native'], cwd=p.ROOT))) as archive:
    archive.extractall(source, filter='data')
metal = source / 'native/metal/Metal.mm'
text = metal.read_text()
def replace(a, b):
    global text
    assert text.count(a) == 1, a
    text = text.replace(a, b)
replace('bool staging) {', 'bool staging, bool shared = false) {')
replace('options:staging ? MTLResourceStorageModeShared', 'options:(staging || shared) ? MTLResourceStorageModeShared')
replace('std::uint32_t index = 0) {', 'std::uint32_t index = 0, bool shared = false) {')
replace('allocateMetal(state, index, physical, false)', 'allocateMetal(state, index, physical, false, shared)')
replace('upload || compute || matrix, index);', 'upload || compute || matrix, index, upload && state->devices[index].hasUnifiedMemory);')
needle = '          if (compute || matrix) {\n            const auto &kernel = f.packet.kernel.state_;'
snapshot = '''          if (upload && state->devices[index].hasUnifiedMemory) {
            std::memcpy((*result)->metal->buffer.contents, source->hostView().data(), source->size);
            if ((*result)->metal->bytes > source->size)
              std::memset(static_cast<std::byte *>((*result)->metal->buffer.contents) + source->size,
                          0, (*result)->metal->bytes - source->size);
'''
if args.policy == 'synchronous':
    snapshot += '''            task.complete(&f.result);
            return;
'''
else:
    snapshot += '''            command = state->permit() ? [state->queues[index] commandBuffer] : nil;
            if (!command) {
              fail(task, f, std::int64_t(Error::Allocation));
              return;
            }
'''
replace(needle, snapshot + '          } else ' + needle.lstrip())
metal.write_text(text)
flags = ['-std=c++23', '-O2', '-pthread', '-mmacosx-version-min=26.0']
p.run([p.CLANG, *flags, '-fobjc-arc', '-fblocks', '-fno-objc-msgsend-class-selector-stubs', '-c', metal, '-o', build / 'Metal.o'])
p.run(['ar', 'rcs', build / 'metal.a', build / 'Metal.o'])
cpp = build / 'Driver.cpp'
cpp.write_text((p.ROOT / 'native/benchmarks/fair/TransformerGpu.cpp').read_text().replace('../../metal/Metal.h', str(source / 'native/metal/Metal.h')))
task = p.ROOT / 'build/native/native/libkalandra-task-core-optimized.a'
p.run([p.CLANG, *flags, '-ffp-contract=off', '-I' + str(accept), '-I' + str(p.ROOT / 'native/benchmarks/fair'), cpp, build / 'metal.a', task, '-framework', 'Foundation', '-framework', 'Metal', '-o', build / 'gpu'])
binaries = {'baseline': accept / 'gpu', 'current': build / 'gpu'}
shapes = [(1, 64), (1, 256), (1, 512), (16, 256), (64, 256), (64, 512)]
verification = []
for m, d in shapes:
    for mode in range(3):
        values = [json.loads(p.run([b, bundle, m, d, 1, 1, mode, 1, 1])) for b in binaries.values()]
        assert values[0] == values[1]
        verification.append(dict(m=m, d=d, mode=mode, bit_identical=True))
def invoke(cell, count, rounds):
    v, m, d, mode = cell
    return p.observations(p.run([binaries[v], bundle, m, d, count, rounds, mode, 0, 0]), m*d*4, count, rounds)
cells = [(v, m, d, mode) for m, d in shapes for mode in range(3) for v in binaries]
result = p.measure_paired(cells, invoke, lambda c: c[1]*c[2]*4, blocks=args.blocks, target_ms=30, adaptive=True, max_iterations=5000)
result.update(contract=__doc__, policy=args.policy, verification=verification, acceptance_sha256=p.sha(args.evidence))
p.evidence(args.output, result, [Path(__file__), metal, cpp, args.evidence], [*binaries.values(), build/'metal.a', task, *bundle.glob('*.metallib')], p.ROOT/'build/native/native/kalandra')
