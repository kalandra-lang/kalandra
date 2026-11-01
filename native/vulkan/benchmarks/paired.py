"""Sequential frozen baseline/candidate GPU API comparison; never builds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline', type=Path, required=True)
p.add_argument('--candidate', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--sizes', nargs='+', type=int, default=[128, 512, 1024])
p.add_argument('--blocks', type=int, default=3)
a = p.parse_args()
assert a.blocks >= 3
a.output.mkdir(parents=True, exist_ok=False)
roots = {tag: path.resolve() for tag, path in [('A', a.baseline), ('B', a.candidate)]}
manifests = {tag: json.loads((path/'manifest.json').read_text()) for tag, path in roots.items()}
def verify():
    for manifest in manifests.values():
        for name, expected in manifest['hashes'].items():
            with Path(name).open('rb') as stream:
                assert hashlib.file_digest(stream, 'sha256').hexdigest() == expected, name
verify()
# Allocator experiment: arithmetic and compiled kernels must remain identical.
for source in roots['A'].glob('*/kernel*.spv'):
    assert source.read_bytes() == (roots['B']/source.relative_to(roots['A'])).read_bytes()
assert (roots['A']/'cpu.o').read_bytes() == (roots['B']/'cpu.o').read_bytes()
(a.output/'manifests.json').write_text(json.dumps(manifests, indent=2)+'\n')
env = os.environ | {'VK_DRIVER_FILES': '/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json',
                    'OPENBLAS_NUM_THREADS': '1', 'OMP_NUM_THREADS': '1', 'OMP_DYNAMIC': 'FALSE'}
def snapshot():
    def cpu():
        ticks = list(map(int, Path('/proc/stat').read_text().splitlines()[0].split()[1:]))
        return sum(ticks), ticks[3]+ticks[4]
    first = cpu(); time.sleep(.5); last = cpu()
    return {'cpuBusyFraction': 1-(last[1]-first[1])/max(1, last[0]-first[0]),
            'gpuUtilizationAndMemory': subprocess.check_output(['nvidia-smi',
                '--query-gpu=utilization.gpu,memory.used', '--format=csv,noheader,nounits'], text=True).strip()}
records = []
for n in a.sizes:
    for pattern in (0, 1):
        for engine, policy in [('gpu-resident', 'fixed-tree'), ('gpu-resident', 'optimized'),
                               ('gpu-transfers', 'optimized')]:
            case = []
            for block in range(a.blocks):
                order = 'ABBA' if block % 2 == 0 else 'BAAB'
                for position, tag in enumerate(order):
                    label = f'{n}-{pattern}-{engine}-{policy}-{block}-{position}-{tag}'
                    manifest = manifests[tag]
                    command = [manifest['binary'], engine, str(n), str(pattern), '1',
                               str(roots[tag]/policy), manifest['blas'], '3']
                    before = snapshot(); started = time.time()
                    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=600)
                    after = snapshot()
                    raw = {'label': label, 'tag': tag, 'n': n, 'pattern': pattern, 'engine': engine,
                           'policy': policy, 'command': command, 'startUnix': started,
                           'status': result.returncode, 'before': before, 'after': after,
                           'stdout': result.stdout, 'stderr': result.stderr}
                    (a.output/(label+'.json')).write_text(json.dumps(raw, indent=2)+'\n')
                    assert result.returncode == 0, (label, result.stderr)
                    rows = [json.loads(line) for line in result.stdout.splitlines()]
                    assert rows[0]['kind'] == 'initialization' and rows[-1]['kind'] == 'correctness'
                    assert rows[-1]['immutable_inputs'] and rows[-1]['resources_retired']
                    samples = [row['ns']/row['iterations'] for row in rows if row['kind'] == 'sample']
                    assert len(samples) == 3
                    raw |= {'nsPerCall': statistics.median(samples), 'samplesNsPerCall': samples,
                            'initialization': rows[0], 'correctness': rows[-1]}
                    case.append(raw); records.append(raw)
                    (a.output/'results.json').write_text(json.dumps({'records': records}, indent=2)+'\n')
                    print(label, raw['nsPerCall'], 'ns/call', flush=True)
            fingerprints = {row['initialization']['input_fingerprint'] for row in case}
            assert len(fingerprints) == 1
            medians = {tag: statistics.median(row['nsPerCall'] for row in case if row['tag'] == tag)
                       for tag in 'AB'}
            summary = {'n': n, 'pattern': pattern, 'engine': engine, 'policy': policy,
                       'medianNsPerCall': medians, 'candidateOverBaseline': medians['B']/medians['A'],
                       'requiresLoadAndSpreadAudit': True}
            (a.output/f'summary-{n}-{pattern}-{engine}-{policy}.json').write_text(json.dumps(summary, indent=2)+'\n')
verify()
print('PASS: frozen paired measurements and numerical checks; audit loads/spread, retain regressions', flush=True)
