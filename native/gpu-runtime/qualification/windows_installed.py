"""Relocated native Windows GPU compiler/package checks; physical GPU tests deferred."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--backend', choices=('cuda', 'vulkan'), required=True)
p.add_argument('--archive', type=Path, required=True)
p.add_argument('--root', type=Path, required=True)
p.add_argument('--arch', choices=('x86_64', 'arm64'), required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(repo/'native/packaging'))
from BundlePE import audit, imports
from VersionCheck import verify_package_version


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    return subprocess.run(list(map(str, command)), check=True, timeout=1800, **kwargs)


root = a.root.resolve()
root.mkdir(parents=True, exist_ok=False)
archive = a.archive.resolve()
assert archive.with_suffix('.gz.sha256').read_text().split()[0] == digest(archive)
name = f'kalandra-native-{a.backend}-standalone'
prefix = root/'relocated prefix λ'/name
with tarfile.open(archive) as package:
    members = package.getmembers()
    assert all((m.isdir() or m.isfile()) and Path(m.name).parts[0] == name and
               '..' not in Path(m.name).parts and not Path(m.name).is_absolute() for m in members)
    assert len({m.name for m in members}) == len(members)
    package.extractall(prefix.parent, filter='data')
listed = {}
for line in (prefix/'SHA256SUMS').read_text().splitlines():
    expected, relative = line.split('  ', 1)
    assert relative not in listed and (prefix/relative).resolve().is_relative_to(prefix)
    assert digest(prefix/relative) == expected, relative
    listed[relative] = expected
assert {p.relative_to(prefix).as_posix() for p in prefix.rglob('*') if p.is_file()} == set(listed) | {'SHA256SUMS'}
for relative in listed:
    base = Path(relative).name.lower()
    assert base not in ('cuda.h', 'cuda_runtime.h', 'nvcuda.dll', 'vulkan-1.dll'), relative
    assert not any(x in base for x in ('cudart', 'cublas', 'nvrtc')), relative
    assert not base.endswith(('.cu', '.raw.ptx')), relative
metadata = json.loads((prefix/f'share/kalandra-native-{a.backend}/toolchain.json').read_text())
assert metadata['format'] == 1 and metadata['llvmVersion'] == '23.1.3'
assert metadata['platform'] == 'windows-'+a.arch
assert metadata['windowsGpuExecutionQualified'] is False and metadata['windowsGpuPerformanceQualified'] is False
if a.backend == 'vulkan':
    assert metadata['validatorBundledSHA256'] == digest(prefix/'libexec/kalandra/spirv-val.exe')
    assert metadata['validatorNoticeSHA256'] == digest(prefix/'share/licenses/kalandra-vulkan/SPIRV-Tools-COPYRIGHT')
audit(prefix)
verify_package_version(prefix)
compiler = prefix/'bin/kalandra.exe'
source = root/'cpu.k'
source.write_text('fn main()->int32:\n  42\n')
run([compiler, source, '-o', root/'cpu.exe'])
assert subprocess.check_output([root/'cpu.exe'], text=True, timeout=30) == '42\n'
if a.backend == 'cuda':
    run([sys.executable, repo/'native/cuda-source/qualification/applications.py', '--build', root,
         '--compiler', compiler, '--output', root/'applications', '--compile-only'])
    probe = root/'applications/hardware/float32/program.exe'
else:
    run([sys.executable, repo/'native/vulkan/qualification/public_cli.py', '--compiler', compiler,
         '--output', root/'applications', '--compile-only'])
    run([sys.executable, repo/'native/vulkan/qualification/shared_capabilities.py', '--backend', 'vulkan',
         '--compiler', compiler, '--output', root/'shared-capabilities', '--compile-only'])
    probe = root/'applications/matrix-hardware/program.exe'
for application in (root/'applications').rglob('program.exe'):
    # Real GPU programs must not statically depend on either driver or an SDK DLL.
    dependencies = [name.lower() for name in imports(application)]
    assert not any(any(s in name for s in ('cuda', 'vulkan', 'nvrtc', 'cublas')) for name in dependencies)
assert probe.is_file()
# Only test actual driver absence when the hosted machine genuinely lacks it.
# Never replace a physical GPU with a mock or software implementation.
driver = Path(os.environ['SystemRoot'])/'System32'/('nvcuda.dll' if a.backend == 'cuda' else 'vulkan-1.dll')
absent = not driver.is_file()
if absent:
    result = subprocess.run([probe], capture_output=True, text=True, timeout=30)
    label = 'CUDA' if a.backend == 'cuda' else 'Vulkan'
    assert result.returncode == 1 and not result.stdout and result.stderr == label+' unavailable: 104\n', result
placed = repo/'native/metal-source/placed.k'
checks = ['libexec/kalandra/clang++.exe', f'libexec/kalandra/kalandra-{a.backend}-lower.exe',
          f'lib/kalandra/native/kalandra-{a.backend}.lib',
          f'lib/kalandra/native/headers/{a.backend}', 'lib/kalandra/toolchain/clang/include']
if a.backend == 'vulkan':
    checks += ['libexec/kalandra/mlir-opt.exe', 'libexec/kalandra/spirv-val.exe']
for index, relative in enumerate(checks):
    owned = prefix/relative
    saved = owned.with_name(owned.name+'.qualification-hidden')
    owned.rename(saved)
    try:
        output = root/f'missing-{index}'
        result = subprocess.run([compiler, '--target', a.backend, placed, '-O2', '-o', output],
                                capture_output=True, text=True, timeout=180)
        assert result.returncode == 1 and not (output/'program.exe').exists(), (relative, result)
    finally:
        saved.rename(owned)
report = {'backend': a.backend, 'architecture': a.arch, 'archiveSHA256': digest(archive),
          'installedCompiler': True, 'cpuExecution': True, 'gpuSourceBuilds': len(list((root/'applications').rglob('program.exe'))),
          'missingResourceChecks': checks, 'missingSystemDriverTested': absent,
          'gpuExecution': False, 'performanceQualified': False}
(root/'qualification.json').write_text(json.dumps(report, indent=2)+'\n')
print('PASS: relocated Windows compiler, CPU execution, real GPU source compilation, PE closure and missing resources; physical GPU unqualified')
