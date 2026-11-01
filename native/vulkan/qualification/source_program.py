"""Private complete-source qualification: build supervisor, SPIR-V and native host.

Not an installed compiler command or a release/distribution claim. Requires the
already configured project build and matching host LLVM tools; installs nothing.
"""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
for name in ('build', 'llvm-root', 'source', 'output'):
    p.add_argument('--' + name, type=Path, required=True)
p.add_argument('--cxx', type=Path, default=Path('/usr/bin/clang++'))
p.add_argument('--validator', type=Path, default=Path('/usr/bin/spirv-val'))
p.add_argument('--arithmetic', choices=('hardware', 'software'), default='hardware')
p.add_argument('--optimization', choices=('0', '2'), default='2')
a = p.parse_args()
repo = Path(__file__).resolve().parents[3]
a.output = a.output.resolve()
a.build = a.build.resolve()

def run(args):
    subprocess.run([str(x) for x in args], check=True)

tools = a.build / 'native/gpu-source'
run([tools/'kalandra-vulkan-supervisor', a.source, a.output, a.arithmetic])
artifacts = (a.output / 'artifacts.txt').read_text().splitlines()
for name in artifacts:
    if not name.startswith('kernel') or not name.endswith('.mlir'):
        continue
    source = a.output / name
    base = source.with_suffix('')
    run([tools/'kalandra-vulkan-lower', source, str(base)+'.lowered.mlir', a.arithmetic])
    run([a.llvm_root/'bin/mlir-opt', str(base)+'.lowered.mlir', '--verify-each',
         '--pass-pipeline=builtin.module(canonicalize,cse,convert-gpu-to-spirv,spirv.module(spirv-lower-abi-attrs,spirv-update-vce))',
         '-o', str(base)+'.converted.mlir'])
    run([tools/'kalandra-vulkan-extract', str(base)+'.converted.mlir', str(base)+'.kernel.mlir'])
    run([a.llvm_root/'bin/mlir-translate', '--no-implicit-module', '--serialize-spirv', str(base)+'.kernel.mlir', '-o', str(base)+'.spv'])
    run([a.validator, '--target-env', 'vulkan1.0', str(base)+'.spv'])
objects = []
if 'cpu.ll' in artifacts:
    run([a.cxx, '-O'+a.optimization, '-c', a.output/'cpu.ll', '-o', a.output/'cpu.o'])
    objects.append(a.output/'cpu.o')
runtime = a.build/'native/vulkan'
run([a.cxx, '-std=c++23', '-O'+a.optimization, '-Wall', '-Wextra', '-Werror', '-I'+str(repo/'native/vulkan'),
     '-I'+str(repo/'native/include'), a.output/'host.cpp', *objects,
     *[runtime/('libkalandra-vulkan'+suffix+'.a') for suffix in ('-language', '-async', '-program', '')],
     a.build/'native/libkalandra-native-runtime.a', '-lvulkan', '-pthread', '-o', a.output/'program'])
print(a.output/'program', flush=True)
