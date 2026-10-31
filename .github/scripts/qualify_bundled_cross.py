"""Exercise one host download against the other four supported CPU platforms."""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--system', required=True, choices=('linux','windows','macos'))
parser.add_argument('--host', required=True, choices=('x86_64','arm64'))
parser.add_argument('--assets', type=Path, required=True)
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--linux-sdk', type=Path, required=True)
parser.add_argument('--imports', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
a = parser.parse_args()
for system, target in (('linux','x86_64'),('linux','arm64'),('windows','x86_64'),('windows','arm64'),('macos','arm64')):
    if (system,target) == (a.system,a.host): continue
    command=[sys.executable,str(ROOT/'native/tests/CrossCompilationTests.py'),'build',
             '--system',system,'--target',target,'--output',str(a.output/(system+'-'+target)),
             '--host-archive',str(a.assets/f'kalandra-native-cpu-standalone-{a.system}-{a.host}.tar.gz')]
    if system == 'windows': command += ['--windows-sdk',str(a.sdk/target/'sdk')]
    elif system == 'linux': command += ['--linux-sdk',str(a.linux_sdk/('linux-cross-sdk-'+target))]
    else: command += ['--macos-imports',str(a.imports)]
    subprocess.run(command,check=True)
