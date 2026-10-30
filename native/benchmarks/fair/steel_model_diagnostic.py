"""Short full-model diagnostic after failed AA controls; never acceptance evidence.

Retain every process and round. Compare frozen production, current fixed-tree,
Steel and MPSGraph without hiding small-workload or owned-upload regressions.
Run alone, after builds/tests, using a numerically qualified frozen candidate.
"""
import argparse
import json
import os
import statistics
from pathlib import Path
import protocol as p

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--build', type=Path, required=True)
ap.add_argument('--baseline', type=Path, required=True)
ap.add_argument('--baseline-bundle', type=Path, required=True)
ap.add_argument('--output', type=Path, required=True)
a = ap.parse_args()
b = a.build.resolve()
manifest = json.loads((b / 'manifest.json').read_text())
qualification = b / 'qualification.json'
assert qualification.exists()
for field in ('signature', 'artifacts'):
    for name, digest in manifest[field].items():
        assert p.sha(p.ROOT / name) == digest, name
records = []
for process in range(3):
    order = ('frozen', 'fixed-tree', 'steel', 'mps')
    if process % 2:
        order = tuple(reversed(order))
    for m, d, mode in ((1, 512, 0), (64, 512, 0), (512, 512, 0), (512, 512, 2)):
        for kind in order:
            count, rounds = 128, 5
            command = ([b / 'mps', m, d, count, rounds, mode, 0, 0, 1] if kind == 'mps' else
                       [a.baseline if kind == 'frozen' else b / 'gpu',
                        a.baseline_bundle if kind == 'frozen' else b / 'bundle',
                        m, d, count, rounds, mode, 0, 0, 2, 1])
            env = dict(os.environ)
            env.pop('KALANDRA_FIXED_TREE', None)
            if kind == 'fixed-tree':
                env['KALANDRA_FIXED_TREE'] = '1'
            output = p.run(command, env=env, timeout=180)
            rows = [json.loads(x) for x in output.splitlines()]
            timed = [x for x in rows if 'ns' in x]
            p.observations('\n'.join(map(json.dumps, timed)), m * d * 4, count, rounds)
            median = statistics.median(x['ns'] / count for x in timed if x['round'] >= 0)
            records.append(dict(process=process, group=[m, d, mode], kind=kind,
                                median_ns=median, environment_overrides=(
                                    {'KALANDRA_FIXED_TREE': '1'} if kind == 'fixed-tree' else {}),
                                rows=rows))
            print(process, [m, d, mode], kind, round(median / 1000, 2), 'us', flush=True)
p.evidence(a.output, dict(acceptance=False,
    reason='Identical-build controls failed, including user-confirmed idle retry. '
           'These diagnostic timings do not qualify precise speedups or regressions.',
    records=records), [Path(__file__), b / 'manifest.json', qualification],
    [b / 'gpu', b / 'mps', a.baseline, *(b / 'bundle').glob('*.metallib'),
     *a.baseline_bundle.glob('*.metallib')], b / 'kalandra')
