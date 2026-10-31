"""Require identical independently assembled host distributions and runtime data."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'native/packaging'))
from RuntimeBundles import digest

first, second = map(Path, sys.argv[1:])
a = {p.name:digest(p) for p in first.glob('*.tar.gz')}
b = {p.name:digest(p) for p in second.glob('*.tar.gz')}
if len(a) < 6 or a != b: raise RuntimeError('Bundled target distributions are not independently reproducible')
print(f'{len(a)} independent host/runtime archives are byte-identical')
