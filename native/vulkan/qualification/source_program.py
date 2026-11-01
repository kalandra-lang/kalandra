"""Private complete-source qualification: build supervisor, SPIR-V and native host.

Not an installed compiler command or a release/distribution claim. Requires the
already configured project build and matching host LLVM tools; installs nothing.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser()
for name in ('build', 'llvm-root', 'source', 'output'):
    p.add_argument('--' + name, type=Path, required=True)
p.add_argument('--cxx', type=Path, default=Path('/usr/bin/clang++'))
p.add_argument('--validator', type=Path, default=Path('/usr/bin/spirv-val'))
p.add_argument('--arithmetic', choices=('hardware', 'software', 'hardware-with-software-fallback'), default='hardware')
p.add_argument('--optimization', choices=('0', '2'), default='2')
p.add_argument('--test-disable-native-float64',action='store_true',help='Private feature-ceiling qualification; preserves original generated host')
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
    if not name.endswith('.vulkan-transform'):
        continue
    lines=(a.output/name).read_text().splitlines()
    assert len(lines)==3 and lines[0]=='kalandra-vulkan-transform-v1'
    fields=dict(line.split('=',1) for line in lines[1:])
    kind,numeric=fields['kind'],fields['type']
    assert kind in ('transpose','softmaxRows','rmsNormRows')
    assert numeric in ('int8','uint8','int16','uint16','int32','uint32','int64','uint64','float32','float64')
    source=a.build/'native/vulkan/transforms'/f'{kind}-{numeric}.spv'
    destination=(a.output/name).with_suffix('.spv')
    assert not destination.is_symlink() and destination.resolve()!=a.source.resolve()
    shutil.copyfile(source,destination)
for name in artifacts:
    if not name.startswith('kernel') or not name.endswith('.mlir'):
        continue
    source = a.output / name
    native = source.with_suffix('')
    variants = [('software' if a.arithmetic == 'software' else 'hardware', native)]
    if a.arithmetic == 'hardware-with-software-fallback':
        # Same source graph and ABI. This lowers float64 arithmetic to integer
        # operations; supported native float32 operations remain unchanged.
        variants.append(('hardware-with-software-fallback', Path(str(native)+'.spv.software')))
    for policy, base in variants:
        run([tools/'kalandra-vulkan-lower', source, str(base)+'.lowered.mlir', policy])
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
host = a.output/'host.cpp'
if a.test_disable_native_float64:
    text=host.read_text()
    original=('auto made=m::Runtime::create({.allowSoftwareFallback=true});'
              if a.arithmetic=='hardware-with-software-fallback' else 'auto made=m::Runtime::create();')
    replacement=('auto made=m::Runtime::create({.allowSoftwareFallback=true,.enableFloat64=false});'
                 if a.arithmetic=='hardware-with-software-fallback' else 'auto made=m::Runtime::create({.enableFloat64=false});')
    assert text.count(original)==1,'unexpected private qualification host layout'
    host=a.output/'host-without-float64.cpp'
    host.write_text(text.replace(original,replacement))
run([a.cxx, '-std=c++23', '-O'+a.optimization, '-Wall', '-Wextra', '-Werror', '-I'+str(repo/'native/vulkan'),
     '-I'+str(repo/'native/include'), host, *objects,
     *[runtime/('libkalandra-vulkan'+suffix+'.a') for suffix in ('-language', '-async', '-program', '')],
     a.build/'native/libkalandra-native-runtime.a', '-lvulkan', '-pthread', '-o', a.output/'program'])
print(a.output/'program', flush=True)
