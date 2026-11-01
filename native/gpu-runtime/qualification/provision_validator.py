"""Build the native Windows SPIR-V validator from the existing pinned vcpkg baseline."""
import argparse
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--directory', type=Path, required=True)
p.add_argument('--arch', choices=('x86_64', 'arm64'), required=True)
a = p.parse_args()
root = a.directory.resolve()
root.mkdir(parents=True, exist_ok=False)
repo = Path(__file__).resolve().parents[3]
baseline = json.loads((repo/'native/toolchain-proof/toolchain.json').read_text())['windowsDependencies']['vcpkgBaseline']
checkout = root/'vcpkg'
def run(command):
    subprocess.run(list(map(str, command)), check=True)
run(['git', 'clone', '--filter=blob:none', '--no-checkout', 'https://github.com/microsoft/vcpkg.git', checkout])
run(['git', '-C', checkout, 'checkout', '--detach', baseline])
run(['cmd', '/c', checkout/'bootstrap-vcpkg.bat', '-disableMetrics'])
manifest = root/'manifest'
manifest.mkdir()
(manifest/'vcpkg.json').write_text(json.dumps({'name': 'kalandra-windows-validator', 'version-string': '1',
    'builtin-baseline': baseline, 'dependencies': [{'name': 'spirv-tools', 'features': ['tools']}]}))
triplet = ('x64' if a.arch == 'x86_64' else 'arm64')+'-windows-static'
run([checkout/'vcpkg.exe', 'install', '--triplet='+triplet, '--host-triplet='+triplet,
     '--x-manifest-root='+str(manifest), '--x-install-root='+str(root/'installed'), '--disable-metrics'])
prefix = root/'installed'/triplet
validator = prefix/'tools/spirv-tools/spirv-val.exe'
notice = prefix/'share/spirv-tools/copyright'
if not validator.is_file() or not notice.is_file():
    raise RuntimeError('Native validator/notice missing')
run([validator, '--version'])
with Path(os.environ['GITHUB_ENV']).open('a') as env:
    env.write(f'KALANDRA_SPIRV_VAL={validator}\nKALANDRA_VULKAN_SPIRV_NOTICE={notice}\n')
