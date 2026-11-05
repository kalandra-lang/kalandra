"""Compile private source-array-result fixtures before public task admission.

Host code only transfers the checked result descriptor and opaque payload.
"""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--llvm', type=Path, required=True)
p.add_argument('--backend', choices=('metal', 'vulkan', 'cuda'), required=True)
p.add_argument('--root', type=Path, required=True)
p.add_argument('--metal', type=Path)
p.add_argument('--sdk', type=Path)
p.add_argument('--cross', default='spirv-cross')
p.add_argument('--validator', default='spirv-val')
p.add_argument('--all-types', action='store_true')
a = p.parse_args()
a.build = a.build.resolve()
a.root = a.root.resolve()
a.root.mkdir(parents=True, exist_ok=True)

def run(command):
    subprocess.run(list(map(str, command)), check=True, timeout=180)

types = ('int8', 'int16', 'int32', 'int64', 'uint8', 'uint16', 'uint32', 'uint64', 'float32', 'float64', 'Bool') if a.all_types else ('int',)
for typ, kind, body in [(typ, kind, body) for typ in types for kind, body in [('alias', 'values'), ('fresh', 'copy(values)')]]:
    case = kind + '-' + typ if a.all_types else kind
    source = a.root / (case + '.k')
    source.write_text(f'fn work(values:[{typ}])->[{typ}]:\n  ' + body + '\nfn main()->int:\n  0\n')
    out = a.root / case
    run([a.build / 'native/gpu-source/kalandra-vulkan-kernels', source, 'work', out, 'hardware'])
    program = json.loads((out / 'program.json').read_text())
    assert len(program['kernels']) == 1 and program['arrayResult']
    kernel = program['kernels'][0]
    assert kernel['scalarBuffers'] == [2, 3] and kernel['type'] == 'uint64'
    (out / 'slots').write_text(str(len(kernel['scalarTypes'])) + '\n')
    base = out / 'kernel0'
    if a.backend == 'cuda':
        run([a.build / 'native/cuda-source/kalandra-cuda-lower', str(base) + '.mlir',
             str(base) + '.ptx', 'hardware', 'sm_80'])
        continue
    if a.backend == 'metal':
        if not a.metal or not a.sdk:
            p.error('Metal requires --metal and --sdk')
        lower = a.build / 'native/metal-proof/kalandra-metal-strict64'
        policy = '--hardware'
        extract = a.build / 'native/metal-proof/kalandra-metal-extract'
    else:
        lower = a.build / 'native/gpu-source/kalandra-vulkan-lower'
        policy = 'hardware'
        extract = a.build / 'native/gpu-source/kalandra-vulkan-extract'
    run([lower, str(base) + '.mlir', str(base) + '.lowered.mlir', policy])
    run([a.llvm / 'bin/mlir-opt', str(base) + '.lowered.mlir', '--verify-each',
         '--pass-pipeline=builtin.module(canonicalize,cse,convert-gpu-to-spirv,spirv.module(spirv-lower-abi-attrs,spirv-update-vce))',
         '-o', str(base) + '.converted.mlir'])
    run([extract, str(base) + '.converted.mlir', str(base) + '.kernel.mlir'])
    run([a.llvm / 'bin/mlir-translate', '--no-implicit-module', '--serialize-spirv',
         str(base) + '.kernel.mlir', '-o', str(base) + '.spv'])
    run([a.validator, '--target-env', 'vulkan1.0', str(base) + '.spv'])
    if a.backend == 'metal':
        run([a.cross, str(base) + '.spv', '--msl', '--msl-version', '20300',
             '--output', str(base) + '.metal'])
        run([a.metal, '-mmacosx-version-min=26.0', '-isysroot', a.sdk, '-std=macos-metal2.3',
             '-fno-fast-math', '-ffp-contract=off', '-fmodules-cache-path=' + str(out / 'module-cache'),
             '-c', str(base) + '.metal', '-o', str(base) + '.air'])
        run([a.metal, str(base) + '.air', '-o', str(base) + '.metallib'])
print('PASS: source-emitted array-return fixtures compiled for runtime qualification')
