"""Compare complete source fixtures from Metal's public capability matrix.

Software arithmetic fixes the numerical contract on both physical backends.
This supplements independent numerical oracles; matching backends alone is not
an independent proof of arithmetic correctness. Hosted mode only builds.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--backend', choices=('metal', 'vulkan'), required=True)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--reference', type=Path)
p.add_argument('--compile-only', action='store_true')
p.add_argument('--replay-only', action='store_true')
a = p.parse_args()
repo = Path(__file__).resolve().parents[3]
source = repo/'native/metal-source'
matrix = source/'capabilities.json'
a.output = a.output.resolve()
a.output.mkdir(parents=True, exist_ok=True)
a.compiler = a.compiler.resolve()
if a.compile_only and a.replay_only:
    p.error('Choose compilation or replay, not both')
if a.backend == 'vulkan' and not a.compile_only and not a.reference:
    p.error('Physical Vulkan parity requires the actual Metal reference report')
reference = None if not a.reference else json.loads(a.reference.read_text())
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
if reference:
    assert reference['backend'] == 'metal' and reference['arithmetic'] == 'software'
    assert reference['matrixSHA256'] == digest(matrix)
expected = {} if reference is None else {record['fixture']: record for record in reference['cases']}
report = {'backend': a.backend, 'arithmetic': 'software', 'matrixSHA256': digest(matrix),
          'compilerSHA256': digest(a.compiler), 'physicalExecution': not a.compile_only,
          'icd': os.environ.get('VK_DRIVER_FILES', os.environ.get('VK_ICD_FILENAMES')),
          'cases': []}
for row in json.loads(matrix.read_text())['rows']:
    if row['status'] != 'supported':
        continue
    for index, fixture in enumerate(row['fixtures']):
        path = source/fixture
        sha = digest(path)
        destination = a.output/(row['id']+'-'+str(index))
        if reference:
            assert expected[fixture]['sourceSHA256'] == sha, fixture
        if not a.replay_only:
            result = subprocess.run([a.compiler, '--target', a.backend, '--arithmetic', 'software',
                                     '-O2', path, '-o', destination], capture_output=True, text=True, timeout=180)
            (a.output/(row['id']+'-'+str(index)+'.compile.log')).write_text(result.stdout+result.stderr)
            assert not result.returncode, (fixture, result.stderr[-5000:])
        manifest = (destination/'manifest.txt').read_text().splitlines()
        assert 'arithmetic=software' in manifest or 'arithmetic_policy=software' in manifest, fixture
        assert 'matmul_policy=fixed-tree' in manifest, fixture
        case = {'fixture': fixture, 'sourceSHA256': sha, 'program': str(destination/'program'), 'runs': []}
        if not a.compile_only:
            for workers in (1, 2, 4):
                binary = destination/'program'
                result = subprocess.run([binary], capture_output=True, text=True, timeout=60,
                                        env=os.environ | {'KALANDRA_WORKERS': str(workers)})
                case['runs'].append({'workers': workers, 'status': result.returncode,
                                     'stdout': result.stdout, 'stderr': result.stderr})
                if len(case['runs']) == 1:
                    report['cases'].append(case)
                (a.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
                assert not result.returncode, (fixture, result)
                assert all(line.endswith(('Metal API Validation Enabled', 'Metal GPU Validation Enabled'))
                           for line in result.stderr.splitlines()), (fixture, result)
                wanted = expected[fixture]['stdout'] if reference else case['runs'][0]['stdout']
                assert result.stdout == wanted, (fixture, result.stdout, wanted)
            case['stdout'] = case['runs'][0]['stdout']
        else:
            report['cases'].append(case)
        (a.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
        print('PASS:', fixture, 'build only' if a.compile_only else 'workers 1/2/4', flush=True)
print('PASS:', len(report['cases']), 'complete shared capability fixtures;',
      'no physical execution' if a.compile_only else a.backend+' physical source parity', flush=True)
