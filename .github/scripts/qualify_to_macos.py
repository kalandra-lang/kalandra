"""Cross-link on Linux/Windows using Kalandra-owned macOS import interfaces."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--system', choices=['linux', 'windows'], required=True)
p.add_argument('--host', choices=['x86_64', 'arm64'], required=True)
p.add_argument('--assets', type=Path, required=True)
p.add_argument('--imports', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
sdk = Path(os.environ['KALANDRA_LLVM_ROOT'])
suffix = '.exe' if a.system == 'windows' else ''
linker = sdk / 'bin' / ('ld64.lld' + suffix)
if not linker.is_file():
    # Some official SDKs omit this driver alias. Keep it beside the original
    # binary so its DLL/RPATH dependency resolution remains unchanged.
    shutil.copy2(sdk / 'bin' / ('lld' + suffix), linker)
lock = json.loads((ROOT / 'native/toolchain-proof/toolchain.json').read_text())
reported = subprocess.check_output([str(linker), '--version'], text=True)
if lock['llvmVersion'] not in reported: raise RuntimeError('Mach-O linker version mismatch')
subprocess.run([sys.executable, str(ROOT / 'native/tests/CrossCompilationTests.py'), 'build',
                '--system', 'macos', '--target', 'arm64', '--output', str(a.output),
                '--host-archive', str(a.assets / f'kalandra-native-cpu-standalone-{a.system}-{a.host}.tar.gz'),
                '--target-archive', str(a.assets / 'kalandra-native-cpu-standalone-macos-arm64.tar.gz'),
                '--macos-imports', str(a.imports), '--linker', str(linker)], check=True)
