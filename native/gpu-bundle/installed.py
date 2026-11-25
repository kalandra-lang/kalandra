"""Qualify production-pinned combined CUDA/Vulkan archives after relocation; installs nothing."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tarfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--archive', type=Path, required=True)
p.add_argument('--root', type=Path, required=True)
p.add_argument('--physical', metavar='INTEL_ICD', type=Path)
p.add_argument('--bundled-targets', action='store_true')
a = p.parse_args()
a.archive = a.archive.resolve()
a.root = a.root.resolve()
a.root.mkdir(parents=True, exist_ok=False)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


assert a.archive.with_suffix('.gz.sha256').read_text().split()[0] == digest(a.archive)
name = 'kalandra-native-gpu-standalone'
prefix = a.root / 'relocated prefix λ' / name
with tarfile.open(a.archive) as archive:
    assert all(m.isdir() or m.isfile() for m in archive.getmembers())
    assert all(Path(m.name).parts[0] == name and '..' not in Path(m.name).parts for m in archive.getmembers())
    archive.extractall(prefix.parent, filter='data')
listed = {}
for line in (prefix / 'SHA256SUMS').read_text().splitlines():
    expected, relative = line.split('  ', 1)
    assert relative not in listed and (prefix / relative).resolve().is_relative_to(prefix)
    assert digest(prefix / relative) == expected, relative
    listed[relative] = expected
assert {str(path.relative_to(prefix)) for path in prefix.rglob('*') if path.is_file()} == set(listed) | {'SHA256SUMS'}

cuda_meta = json.loads((prefix / 'share/kalandra-native-cuda/toolchain.json').read_text())
assert cuda_meta['format'] == 1 and cuda_meta['llvmVersion'] == '23.1.3'
assert cuda_meta['cudaSDKIncluded'] is False and cuda_meta['optimizedMatmulCatalog'] == 1
vulkan_meta = json.loads((prefix / 'share/kalandra-native-vulkan/toolchain.json').read_text())
assert vulkan_meta['format'] == 1 and vulkan_meta['llvmVersion'] == '23.1.3'
assert digest(prefix / 'libexec/kalandra/spirv-val') == vulkan_meta['validatorBundledSHA256']
assert digest(prefix / 'share/licenses/kalandra-vulkan/SPIRV-Tools-COPYRIGHT') == vulkan_meta['validatorNoticeSHA256']

if a.bundled_targets:
    cpu = json.loads((prefix / 'share/kalandra-native-tools/toolchain.json').read_text())
    expected = {'linux-x86_64', 'linux-arm64', 'windows-x86_64', 'windows-arm64', 'macos-arm64'}
    assert set(cpu['bundledCpuTargets']) == expected
    assert {path.name for path in (prefix / 'lib/kalandra/targets').iterdir()} == expected

for relative in listed:
    base = Path(relative).name.lower()
    assert not any(x in base for x in ('libcuda.so', 'libcudart', 'libcublas', 'libnvrtc', 'libvulkan.so')), relative
    assert Path(relative).name not in ('cuda.h', 'cuda_runtime.h'), relative
    assert not base.endswith(('.cu', '.raw.ptx')), relative

repo = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(repo / 'native/packaging'))
from BundleELF import audit
from VersionCheck import verify_package_version
verify_package_version(prefix)
audit(prefix)

compiler = prefix / 'bin/kalandra'
source = a.root / 'cpu.k'
source.write_text('fn main()->i32:\n  return 42\n')
subprocess.run([compiler, '--test-entry', 'main', source, '-o', a.root / 'cpu'], check=True, timeout=90)
assert subprocess.check_output([a.root / 'cpu'], text=True, timeout=30) == '42\n'

bundle_cmd = [sys.executable, Path(__file__).with_name('qualify.py'),
              '--compiler', compiler, '--output', a.root / 'bundle-app']
if a.physical:
    bundle_cmd.extend(['--physical', a.physical])
subprocess.run(bundle_cmd, check=True, timeout=300)

placed = repo / 'native/metal-source/placed.k'
for backend, ext in (('cuda', '.ptx'), ('vulkan', '.spv')):
    out = a.root / f'single-{backend}'
    subprocess.run([compiler, '--test-entry', 'main', '--target', backend, '-O2', placed, '-o', out],
                   check=True, timeout=180)
    assert (out / 'program').is_file() and list(out.glob(f'*{ext}'))

example = Path(__file__).with_name('example.k')
checks = [
    'libexec/kalandra/clang++',
    'libexec/kalandra/kalandra-cuda-lower',
    'libexec/kalandra/kalandra-vulkan-lower',
    'libexec/kalandra/mlir-opt',
    'libexec/kalandra/spirv-val',
    'lib/kalandra/native/libkalandra-cuda.a',
    'lib/kalandra/native/libkalandra-vulkan.a',
    'lib/kalandra/native/headers/gpu-bundle/Language.h',
    'lib/kalandra/native/headers/gpu-runtime/ProgramDecl.inc',
    'lib/kalandra/toolchain/clang/include',
]
for index, relative in enumerate(checks):
    owned = prefix / relative
    saved = owned.with_name(owned.name + '.qualification-hidden')
    owned.rename(saved)
    try:
        output = a.root / f'missing-{index}'
        run = subprocess.run([compiler, example, '--target', 'cuda,vulkan', '-O2', '-o', output],
                             capture_output=True, text=True, timeout=90)
        assert run.returncode != 0 and not (output / 'program').exists(), (relative, run)
    finally:
        saved.rename(owned)

print('PASS: relocated combined CUDA+Vulkan/CPU compiler, multi-backend bundle, SDK-free ELF closure and ten owned-resource failure gates')
