"""Build Windows applications using private SDK inputs inside a receiver job."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--system', choices=['linux', 'macos'], required=True)
p.add_argument('--host', choices=['x86_64', 'arm64'], required=True)
p.add_argument('--assets', type=Path, required=True)
p.add_argument('--sdk', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
linker = Path(os.environ.get('KALANDRA_CROSS_LLD_ROOT', str(Path(os.environ['KALANDRA_LLVM_ROOT']) / 'bin'))) / 'lld-link'
for target in ('x86_64', 'arm64'):
    subprocess.run([sys.executable, str(ROOT / 'native/tests/CrossCompilationTests.py'), 'build',
                    '--system', 'windows', '--target', target, '--output', str(a.output / target),
                    '--host-archive', str(a.assets / f'kalandra-native-cpu-standalone-{a.system}-{a.host}.tar.gz'),
                    '--target-archive', str(a.assets / f'kalandra-native-cpu-standalone-windows-{target}.tar.gz'),
                    '--windows-sdk', str(a.sdk / target / 'sdk'), '--linker', str(linker)], check=True)
