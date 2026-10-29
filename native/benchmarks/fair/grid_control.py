"""Same-kernel noise control for the frozen 2D-grid screen."""
import argparse
import json
import os
from pathlib import Path
import protocol as p

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--blocks', type=int, default=7)
a = parser.parse_args()
b = a.build.resolve()
assert not a.output.exists()
manifest = b / 'manifest.json'
frozen = json.loads(manifest.read_text())
for name, digest in frozen['artifacts'].items():
    assert p.sha(p.ROOT / name) == digest, name
for name, digest in frozen['signature'].items():
    if name != 'grid_mode':
        assert p.sha(p.ROOT / name) == digest, name
qualification = b / 'qualification.json'
checks = json.loads(qualification.read_text())['checks']
assert len(checks) == 84 and all(row['passed'] for row in checks)
for flag in ('MTL_DEBUG_LAYER', 'MTL_SHADER_VALIDATION', 'DYLD_INSERT_LIBRARIES', 'KALANDRA_PROBE_CAPTURE'):
    assert os.getenv(flag) in (None, '', '0')
shapes = [(16,512,512), (64,512,2048), (64,2048,512), (16,512,2048), (16,2048,512), (64,512,512)]
def invoke(cell, count, rounds):
    # Both labels intentionally invoke the identical original kernel.
    return p.observations(p.run([b/'screen', b/'study.metallib', 'baseline', *cell[1:], count, rounds, 0]), cell[1]*cell[3], count, rounds)
data = p.measure_paired([(v,*s) for s in shapes for v in ('baseline','current')], invoke, lambda c:c[1]*c[3], blocks=a.blocks, target_ms=30, adaptive=True, max_iterations=256)
data.update(contract=__doc__, acceptance=False, qualification_sha256=p.sha(qualification), scope='AA control; both labels execute the same frozen original kernel.')
p.evidence(a.output, data, [Path(__file__), Path(p.__file__), manifest, qualification], [p.ROOT/f for f in frozen['artifacts']], p.ROOT/'build/native/native/kalandra')
