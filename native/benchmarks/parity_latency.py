"""Measure equivalent CPU and GPU applications, including transfers and allocation.

Each measurement launches a fresh process. Build time is excluded. Driver disk
caches are left in their ordinary state. All outputs must match the source
oracle before their elapsed times are accepted.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('phase', choices=('prepare', 'measure'))
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--backend', choices=('metal', 'vulkan', 'cuda'), required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
a.compiler = a.compiler.resolve()
a.out = a.out.resolve()
env = {**os.environ, 'KALANDRA_WORKERS': '1'}

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def execute(path, expected, gpu):
    start = time.perf_counter_ns()
    process = subprocess.run([path], capture_output=True, text=True, env=env, check=True, timeout=180)
    elapsed = time.perf_counter_ns() - start
    assert int(process.stdout.strip(), 16 if gpu else 10) == expected, (path, process.stdout, expected)
    return elapsed

def write_cases(root):
    root.mkdir()
    for size in (32, 1024, 8192):
        collection = f"""fn work(n:i64)->[i64]:
  return for i in range(n):
    if i%3==0:
      yield i+7
fn main()->i64 ! TaskError:
  return try parallel:
    values=try await spawn work({size}) on devices.gpu()
    length(values)+values[0]+values[length(values)-1]
"""
        tensor = f"""fn work(values:Tensor[i64, ?])->i64:
  result=0
  for i in range({size}):
    result=result+values[i]
  result
fn main()->i64 ! TaskError:
  values=tensor(generate({size},fn(i):i%7))
  try parallel:
    gpu=devices.gpu()
    resident=try await transfer(values,gpu)
    try await spawn work(resident) on gpu
"""
        cases = {
            'collection': (collection, (size+2)//3+14+3*((size-1)//3)),
            'tensor': (tensor, sum(i%7 for i in range(size))),
        }
        for name, (gpu, expected) in cases.items():
            cpu = gpu.replace(' on devices.gpu()', '').replace(' on gpu', '')
            cpu = cpu.replace('    gpu=devices.gpu()\n', '')
            cpu = cpu.replace('try await transfer(values,gpu)', 'values')
            (root/f'{name}-{size}-gpu.k').write_text(gpu)
            (root/f'{name}-{size}-cpu.k').write_text(cpu)
            (root/f'{name}-{size}.expected').write_text(str(expected)+'\n')

if a.phase == 'prepare':
    a.out.mkdir(parents=True, exist_ok=False)
    sources_dir = a.out / 'sources'
    write_cases(sources_dir)
    report = {'system': platform.platform(), 'backend': a.backend, 'compilerSha256': sha(a.compiler), 'cases': []}
    for oracle in sorted(sources_dir.glob('*.expected')):
        name = oracle.stem
        expected = int(oracle.read_text())
        cpu, gpu = a.out/f'{name}-cpu', a.out/f'{name}-gpu'
        sources = {mode: sources_dir/f'{name}-{mode}.k' for mode in ('cpu','gpu')}
        subprocess.run([a.compiler, sources['cpu'], '-O2', '-o', cpu], check=True)
        subprocess.run([a.compiler, sources['gpu'], '--target', a.backend, '-O2', '-o', gpu], check=True)
        for path, device in ((cpu,False),(gpu/'program',True)):
            execute(path, expected, device)
        files = {str(x.relative_to(a.out)): sha(x) for x in [cpu,*sorted(gpu.rglob('*'))] if x.is_file()}
        report['cases'].append({'name': name, 'expected': expected, 'files': files,
                                'sources': {key: sha(val) for key, val in sources.items()}})
    (a.out/'prepared.json').write_text(json.dumps(report, indent=2)+'\n')
else:
    prepared = json.loads((a.out/'prepared.json').read_text())
    assert prepared['backend'] == a.backend and prepared['compilerSha256'] == sha(a.compiler)
    results = []
    for case in prepared['cases']:
        name, expected = case['name'], case['expected']
        assert all(sha(a.out/f) == value for f, value in case['files'].items())
        assert all(sha(a.out/'sources'/f'{name}-{mode}.k') == value
                   for mode, value in case['sources'].items())
        paths = {'cpu': a.out/f'{name}-cpu', 'gpu': a.out/f'{name}-gpu'/'program'}
        def sample(key):
            return execute(paths[key], expected, key == 'gpu')
        for key in ('cpu','gpu'):
            for _ in range(3):
                sample(key)
        before = {key: [sample(key) for _ in range(5)] for key in paths}
        blocks = []
        for ordinal in range(9):
            order = ('cpu','gpu','gpu','cpu') if ordinal % 2 == 0 else ('gpu','cpu','cpu','gpu')
            blocks.append([(key, sample(key)) for key in order])
        summary = {key: statistics.median(t for block in blocks for mode,t in block if mode == key)
                   for key in paths}
        after = {key: [sample(key) for _ in range(5)] for key in paths}
        drift = {key: statistics.median(after[key]) / statistics.median(before[key]) - 1
                 for key in paths}
        results.append({'name': name, 'medianNs': summary, 'blocks': blocks,
                        'preControlNs': before, 'postControlNs': after, 'controlDrift': drift,
                        'withinControlTolerance': all(abs(value) <= 0.10 for value in drift.values())})
    (a.out/'measured.json').write_text(json.dumps({'prepared': prepared, 'results': results}, indent=2)+'\n')
