"""Qualify production-pinned Vulkan archives after relocation; installs nothing.

Compilation still requires the platform C++ development headers. Physical GPU
execution is explicit; hosted checks never substitute a software device.
"""
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
p.add_argument('--execute', action='store_true')
p.add_argument('--bundled-targets', action='store_true')
a = p.parse_args()
a.archive = a.archive.resolve()
a.root = a.root.resolve()
a.root.mkdir(parents=True, exist_ok=False)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

assert a.archive.with_suffix('.gz.sha256').read_text().split()[0] == digest(a.archive)
name = 'kalandra-native-vulkan-standalone'
prefix = a.root/'relocated prefix λ'/name
with tarfile.open(a.archive) as archive:
    assert all(member.isdir() or member.isfile() for member in archive.getmembers())
    assert all(Path(member.name).parts[0] == name and '..' not in Path(member.name).parts
               for member in archive.getmembers())
    archive.extractall(prefix.parent, filter='data')
listed = {}
for line in (prefix/'SHA256SUMS').read_text().splitlines():
    expected, relative = line.split('  ', 1)
    assert relative not in listed and (prefix/relative).resolve().is_relative_to(prefix)
    assert digest(prefix/relative) == expected, relative
    listed[relative] = expected
assert {str(path.relative_to(prefix)) for path in prefix.rglob('*') if path.is_file()} == set(listed) | {'SHA256SUMS'}
metadata = json.loads((prefix/'share/kalandra-native-vulkan/toolchain.json').read_text())
assert metadata['format'] == 1 and metadata['llvmVersion'] == '23.1.3'
if a.bundled_targets:
    cpu = json.loads((prefix/'share/kalandra-native-tools/toolchain.json').read_text())
    expected = {'linux-x86_64', 'linux-arm64', 'windows-x86_64', 'windows-arm64', 'macos-arm64'}
    assert set(cpu['bundledCpuTargets']) == expected
    assert {path.name for path in (prefix/'lib/kalandra/targets').iterdir()} == expected

assert digest(prefix/'libexec/kalandra/spirv-val') == metadata['validatorBundledSHA256']
assert digest(prefix/'share/licenses/kalandra-vulkan/SPIRV-Tools-COPYRIGHT') == metadata['validatorNoticeSHA256']
repo = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(repo/'native/packaging'))
from BundleELF import audit
from VersionCheck import verify_package_version
verify_package_version(prefix)
audit(prefix)
compiler = prefix/'bin/kalandra'
source = a.root/'cpu.k'
source.write_text('fn main()->i32:\n  return 42\n')
subprocess.run([compiler, '--test-entry', 'main', source, '-o', a.root/'cpu'], check=True, timeout=60)
assert subprocess.check_output([a.root/'cpu'], text=True, timeout=30) == '42\n'
script = Path(__file__).with_name('public_cli.py')
command = [sys.executable, script, '--compiler', compiler, '--output', a.root/'applications']
if not a.execute:
    command.append('--compile-only')
subprocess.run(command, check=True, timeout=1200)
subprocess.run([sys.executable, repo/'native/vulkan/tests/loader.py',
                '--application', a.root/'applications/matrix-hardware/program',
                '--cxx', prefix/'libexec/kalandra/clang++',
                '--resource-dir', prefix/'lib/kalandra/toolchain/clang'], check=True, timeout=90)
# Compile host array helpers after relocation as well as the shader-only cases.
# This checks the native scope runtime symbols and all private header dependencies.
arrays = [sys.executable, repo/'native/gpu-source/HostArrayTests.py',
          '--compiler', compiler, '--backend', 'vulkan', '--variant', 'optimized',
          '--root', a.root/'host-arrays']
if not a.execute:
    arrays.append('--compile-only')
subprocess.run(arrays, check=True, timeout=1200)
control = [sys.executable, repo/'native/gpu-source/ScalarControlTests.py',
           '--compiler', compiler, '--backend', 'vulkan', '--variant', 'optimized',
           '--root', a.root/'scalar-control']
if not a.execute:
    control.append('--compile-only')
subprocess.run(control, check=True, timeout=1200)
# Exercise invocation-local allocation and collection through relocated resources.
device_arrays = [sys.executable, repo/'native/gpu-source/DeviceArrayTests.py',
                 '--compiler', compiler, '--backend', 'vulkan',
                 '--variant', 'optimized', '--root', a.root/'device-arrays']
if not a.execute:
    device_arrays.append('--compile-only')
subprocess.run(device_arrays, check=True, timeout=1200)
array_entries = [sys.executable, repo/'native/gpu-source/DeviceArrayEntryTests.py',
                 '--compiler', compiler, '--backend', 'vulkan',
                 '--variant', 'optimized', '--root', a.root/'device-array-entry']
if not a.execute:
    array_entries.append('--compile-only')
subprocess.run(array_entries, check=True, timeout=1200)
array_results = [sys.executable, repo/'native/gpu-source/DeviceArrayResultTests.py',
                 '--compiler', compiler, '--backend', 'vulkan',
                 '--variant', 'optimized', '--root', a.root/'device-array-results']
if not a.execute:
    array_results.append('--compile-only')
subprocess.run(array_results, check=True, timeout=1200)
array_main = [sys.executable, repo/'native/gpu-source/DeviceArrayMainTests.py',
              '--compiler', compiler, '--backend', 'vulkan',
              '--variant', 'optimized', '--root', a.root/'device-array-main']
if not a.execute:
    array_main.append('--compile-only')
subprocess.run(array_main, check=True, timeout=1200)
host_aggregates = [sys.executable, repo/'native/gpu-source/HostAggregateTests.py',
                   '--compiler', compiler, '--backend', 'vulkan',
                   '--variant', 'optimized', '--root', a.root/'host-aggregates']
if not a.execute:
    host_aggregates.append('--compile-only')
subprocess.run(host_aggregates, check=True, timeout=1200)



booleans = [sys.executable, repo/'native/gpu-source/DeviceBoolTests.py',
            '--compiler', compiler, '--backend', 'vulkan',
            '--variant', 'optimized', '--root', a.root/'device-bool']
if not a.execute:
    booleans.append('--compile-only')
subprocess.run(booleans, check=True, timeout=1200)

# Individually remove required owned resources. An installed compiler must not
# reuse its compiled-in build-tree paths or ambient PATH when one is missing.
for relative in ['libexec/kalandra/clang++', 'libexec/kalandra/mlir-opt',
                 'libexec/kalandra/kalandra-vulkan-lower', 'libexec/kalandra/spirv-val',
                 'lib/kalandra/native/libkalandra-vulkan.a',
                 'lib/kalandra/native/headers/vulkan',
                 'lib/kalandra/toolchain/clang/include']:
    owned = prefix/relative
    saved = owned.with_name(owned.name+'.qualification-hidden')
    owned.rename(saved)
    try:
        output = a.root/('missing-'+owned.name)
        result = subprocess.run([compiler, '--test-entry', 'main', '--target', 'vulkan', repo/'native/metal-source/placed.k',
                                 '-O2', '-o', output], text=True, capture_output=True, timeout=60)
        assert result.returncode == 1 and not (output/'program').exists(), (relative, result)
        assert any(message in result.stderr for message in ('bundled Vulkan', 'installed Vulkan resource')), (relative, result)
    finally:
        saved.rename(owned)
print('PASS: relocated owned tools, ELF closure, native CPU, source GPU builds, missing-loader diagnostics and seven missing-resource gates', flush=True)
