"""Audit actual SPIR-V artifacts after the typed debug application suites."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--disassembler', type=Path, required=True)
args = parser.parse_args()
families = ('maps', 'reductions', 'matrices', 'scalars', 'conversions', 'tier2',
            'programs', 'matrix-programs', 'matrix-control')
files = []
for family in families:
    artifacts = list((args.root / f'numeric-{family}-cli-debug').rglob('*.spv'))
    assert artifacts, f'Run the {family} debug application suite first'
    files.extend(artifacts)
forbidden = re.compile(
    r'\b(?:OpTypeFloat\s+64|OpTypeInt\s+128|'
    r'OpF(?:Add|Sub|Mul|Div|Rem|Mod|Negate|Ord\w+|Unord\w+))\b')

def check(path):
    assembly = subprocess.check_output([str(args.disassembler), str(path)], text=True)
    return str(path) if forbidden.search(assembly) else None

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    failures = [path for path in pool.map(check, files) if path]
print(json.dumps({'shaders': len(files), 'forbiddenFloatingOr128BitArtifacts': failures}))
assert not failures
