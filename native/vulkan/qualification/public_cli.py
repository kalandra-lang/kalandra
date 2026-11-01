"""Qualify the actual development compiler command, without private adapters."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--compile-only', action='store_true')
args = parser.parse_args()
args.compiler = args.compiler.resolve()
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[3]
placed = repo/'native/metal-source/placed.k'
cases = []
for policy in ('hardware', 'software', 'hardware-with-software-fallback'):
    for optimization in ('-O0', '-O2'):
        cases.append(('placed-'+policy+optimization, [placed], policy, optimization, [5, 12, 21], 'f', 0))
cases.append(('mixed-cpu', [repo/'native/metal-source/cpu-spawn.k'], 'hardware', '-O2', [52, 54, 64, 66], 'f', 0))
for label, numeric, shape, expression, values, expected, fmt, fault in [
    ('transpose', 'int64', '2,2', 'transpose(x)', '[[1,2],[3,4]]', [1, 3, 2, 4], 'q', 0),
    ('softmax', 'float32', '1,2', 'softmaxRows(x)', '[[0,0]]', [.5, .5], 'f', 0),
    ('overflow', 'int8', '2', 'x+1', '[127,0]', [], 'b', 4)]:
    source = args.output/(label+'.k')
    rank = ','.join('?' for _ in shape.split(','))
    source.write_text(f'''fn work(x:Tensor[{numeric},{rank}])->Tensor[{numeric},{rank}]:
  {expression}
fn main()->Tensor[{numeric},{rank}] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    x:Tensor[{numeric},{shape}]=tensor({values})
    resident=try await transfer(x,gpu)
    result=try await spawn work(resident) on gpu
    try await transfer(result,devices.cpu())
''')
    cases.append((label, [source], 'hardware', '-O2', expected, fmt, fault))
project = args.output/'project'
project.mkdir(exist_ok=True)
(project/'main.k').write_text(placed.read_text())
(project/'kalandra.toml').write_text("format = 1\n[project]\nentry = 'main.k'\n")
cases.append(('project', ['--project', project/'kalandra.toml'], 'hardware', '-O2', [5, 12, 21], 'f', 0))
records = []
for name, source, policy, optimization, values, fmt, fault in cases:
    output = args.output/name
    result = subprocess.run(list(map(str, [args.compiler, '--target', 'vulkan', *source,
        '--arithmetic', policy, optimization, '-o', output])), capture_output=True, text=True, timeout=180)
    (args.output/(name+'.log')).write_text(result.stdout+result.stderr)
    assert not result.returncode, (name, result.stderr[-4000:])
    assert 'matmul_policy=fixed-tree' in (output/'manifest.txt').read_text()
    assert 'arithmetic_policy='+policy in (output/'manifest.txt').read_text()
    assert (output/'THIRD_PARTY_NOTICES.txt').read_bytes() == (repo/'native/vulkan/third_party/ggml/LICENSE').read_bytes()
    if not args.compile_only:
        width = struct.calcsize(fmt)
        expected = '['+','.join(f'{int.from_bytes(struct.pack("<"+fmt, value),"little"):0{width*2}x}' for value in values)+']\n'
        for device, icd in [('nvidia', 'nvidia_icd.x86_64.json'), ('intel', 'intel_icd.x86_64.json')]:
            for workers in (1, 2, 4):
                execution = subprocess.run([output/'program'], capture_output=True, text=True, timeout=60,
                    env=os.environ | {'VK_DRIVER_FILES': '/usr/share/vulkan/icd.d/'+icd,
                                      'KALANDRA_WORKERS': str(workers)})
                record = {'case': name, 'device': device, 'workers': workers,
                          'status': execution.returncode, 'stdout': execution.stdout, 'stderr': execution.stderr}
                records.append(record)
                (args.output/'results.json').write_text(json.dumps(records, indent=2)+'\n')
                if fault:
                    assert execution.returncode and not execution.stdout and f'Vulkan task failed: 1/{fault}' in execution.stderr, record
                else:
                    assert not execution.returncode and not execution.stderr and execution.stdout == expected, record
    print('PASS:', name, 'public command compilation'+(' only' if args.compile_only else ' and physical execution'), flush=True)

for label, options, message in [
    ('conflicting-sanitizers', ['--sanitize', '--thread-sanitize'], 'usage:'),
    ('thread-sanitize', ['--thread-sanitize'], 'sanitizer packages'),
    ('optimized', ['--matmul-policy', 'optimized'], 'optimized Vulkan matmul'),
    ('cross-cpu', ['--platform', 'linux-x86_64'], 'selects a CPU ABI')]:
    output = args.output/('reject-'+label)
    output.mkdir(exist_ok=True)
    sentinel = output/'sentinel'
    sentinel.write_text('unchanged')
    result = subprocess.run(list(map(str, [args.compiler, '--target', 'vulkan', placed, '-o', output, *options])), capture_output=True, text=True, timeout=30)
    assert result.returncode and message in result.stderr, (label, result)
    assert sentinel.read_text() == 'unchanged' and sorted(p.name for p in output.iterdir()) == ['sentinel']
for symlink in (False, True):
    output = args.output/('reject-symlink' if symlink else 'reject-source-collision')
    output.mkdir(exist_ok=True)
    source = output/'source.k'
    original = placed.read_text()
    source.write_text(original)
    target = output/'host.cpp'
    target.unlink(missing_ok=True)
    if symlink:
        victim = output/'victim'
        victim.write_text('unchanged')
        target.symlink_to(victim)
    else:
        target.hardlink_to(source)
    result = subprocess.run(list(map(str, [args.compiler, '--target', 'vulkan', source, '-o', output])), capture_output=True, text=True, timeout=30)
    assert result.returncode and ('symlink' if symlink else 'distinct') in result.stderr, result
    assert source.read_text() == original and not (output/'kernel0.mlir').exists()
    if symlink:
        assert victim.read_text() == 'unchanged'
print('PASS:', len(cases), 'public builds;', len(records), 'physical executions; six fail-closed/output-protection cases', flush=True)
