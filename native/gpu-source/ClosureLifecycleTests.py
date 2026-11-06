"""Run source-generated closure lifecycle checks with a Linux GPU sanitizer runtime.

The compiler may be a relocated release package. --runtime-build must be a
standalone native/cuda or native/vulkan ASan/UBSan build of this source revision.
This script installs nothing and executes the real generated device program.
Metal uses the corresponding CTest fixture with additional private fault hooks.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--backend', choices=('cuda', 'vulkan'), required=True)
p.add_argument('--runtime-build', type=Path, required=True)
p.add_argument('--cxx', type=Path, required=True)
p.add_argument('--root', type=Path, required=True)
a = p.parse_args()
compiler, build, root = (x.resolve() for x in
                         (a.compiler, a.runtime_build, a.root))
# Preserve clang++'s driver name: resolving its symlink to clang changes linking.
cxx = a.cxx.absolute()
native = Path(__file__).resolve().parents[1]
root.mkdir(parents=True, exist_ok=False)
cache = (build / 'CMakeCache.txt').read_text()
assert f'KALANDRA_{a.backend.upper()}_SANITIZERS:BOOL=ON' in cache

def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=240)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr, result.stderr
    return result.stdout

fixture = native / 'tests/fixtures/mixed-host-aggregates/gpu-host-closure-resident-result.k'
program = root / 'program'
run([compiler, fixture, '--target', a.backend, '-O2', '-o', program])
host = (program / 'host.cpp').read_text()
assert 'DeviceGraphArgument<GpuClosureCapture' in host
assert 'NumericInstructionKind::DeviceControl' in host
# Confirm the generated program itself, before replacing its CLI with the harness.
assert int(run([program / 'program']), 16) == 32
parts = ['language'] + (['async'] if a.backend == 'vulkan' else []) + ['program', '', 'task-core']
libraries = [build / ('libkalandra-' + a.backend + ('-' + part if part else '') + '.a')
             for part in parts]
assert all(path.is_file() for path in libraries), libraries
harness = native / 'tests/GpuClosureLifecycleTests.cpp'
executable = root / 'lifecycle'
flags = ['-std=c++23', '-O1', '-g', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-ffp-contract=off',
         '-Wall', '-Wextra', '-Werror', '-Wno-override-module']
# Standalone runtime targets provide Runtime.cpp and Tasks.cpp. Include the
# remaining native bridge definitions exactly once, with the same sanitizers.
extra = [native / 'runtime' / name for name in ('ValueScopes.cpp', 'TaskABI.cpp')]
run([cxx, *flags, '-I' + str(native / a.backend), '-I' + str(native / 'include'),
     '-I' + str(program), harness, program / 'cpu.ll', *extra,
     *libraries, '-pthread', '-ldl', '-o', executable])
output = run([executable, program])
(root / 'report.json').write_text(json.dumps({
    'backend': a.backend, 'sanitizers': ['address', 'undefined'],
    'compilerSHA256': hashlib.sha256(compiler.read_bytes()).hexdigest(),
    'harnessSHA256': hashlib.sha256(harness.read_bytes()).hexdigest(),
    'expected': 32, 'status': 'passed', 'output': output,
    'environment': {key: os.environ[key] for key in
                    ('VK_DRIVER_FILES', 'ASAN_OPTIONS', 'LSAN_OPTIONS', 'LD_PRELOAD')
                    if key in os.environ},
    'faultHookScope': 'native/admission failures; facade-held completion cancellation; '
                      'backend device/completion injection requires separate runtime tests',
}, indent=2) + '\n')
print(output, end='')
