"""Private Linux CUDA application qualification, using an existing root build.

No dependency installation and no GPU execution. Not the installed CLI target.
"""
import argparse
import pathlib
import subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=pathlib.Path, required=True)
p.add_argument('--source', type=pathlib.Path, required=True)
p.add_argument('--output', type=pathlib.Path, required=True)
p.add_argument('--clang', default='clang++')
p.add_argument('--test-disable-native-float64', action='store_true', help='Private capability ceiling; never changes arithmetic policy')
p.add_argument('--matmul-policy',choices=('fixed-tree','optimized'),default='fixed-tree')
p.add_argument('--arithmetic', choices=('hardware','software','hardware-with-software-fallback'), default='hardware')
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parents[2]
build = a.build.resolve() / 'native'
out = a.output.resolve()
def run(args): subprocess.run([str(v) for v in args], check=True)
run([build/'gpu-source/kalandra-cuda-supervisor', a.source.resolve(), out, a.arithmetic])
if a.test_disable_native_float64:
    path=out/'host.cpp';text=path.read_text()
    before='auto made=m::Runtime::create('
    assert text.count(before)==1
    if '.allowSoftwareFallback=true' in text:
        text=text.replace('{.allowSoftwareFallback=true}', '{.allowSoftwareFallback=true,.enableFloat64=false}')
    else:
        assert 'auto made=m::Runtime::create();' in text
        text=text.replace('auto made=m::Runtime::create();','auto made=m::Runtime::create({.enableFloat64=false});')
    path.write_text(text)

import shutil
for descriptor in out.glob('*.cuda-transform'):
    lines=descriptor.read_text().splitlines()
    assert len(lines)==3 and lines[0]=='kalandra-cuda-transform-v1'
    values=dict(line.split('=',1) for line in lines[1:])
    assert values['kind'] in ('transpose','softmaxRows','rmsNormRows')
    assert values['type'] in ('int8','uint8','int16','uint16','int32','uint32','int64','uint64','float32','float64')
    resource=build/'cuda/transforms'/(values['kind']+'-'+values['type']+'.ptx')
    shutil.copyfile(resource,descriptor.with_suffix('.ptx'))
for source in out.glob('kernel*.mlir'):
    target = source.with_suffix('.ptx')
    primary = 'software' if a.arithmetic == 'software' else 'hardware'
    run([build/'cuda-source/kalandra-cuda-lower', source, target, primary, 'sm_80'])
    if a.arithmetic == 'hardware-with-software-fallback':
        run([build/'cuda-source/kalandra-cuda-lower', source, str(target)+'.software.ptx', a.arithmetic, 'sm_80'])
if a.matmul_policy=='optimized' and a.arithmetic!='software':
    for marker in out.glob('*.cuda-matmul'):
        assert marker.read_text()=='kalandra-cuda-matmul-v1\ntype=float32\n'
        target=marker.with_suffix('.ptx')
        shutil.copyfile(target,str(target)+'.fixed-tree.ptx')
        target.with_suffix('.ptx.json').rename(str(target)+'.fixed-tree.ptx.json')
        shutil.copyfile(build/'cuda/kernels/matmul-warp64.ptx',target)
if list(out.glob('*.native.metal')) or list(out.glob('*.vulkan-transform')):
    raise RuntimeError('CUDA transform resources have not been integrated; refusing an incomplete application')
objects=[]
if (out/'cpu.ll').exists():
    run([a.clang,'-O2','-ffp-contract=off','-c',out/'cpu.ll','-o',out/'cpu.o']);objects.append(out/'cpu.o')
run([a.clang,'-std=c++23','-O2','-ffp-contract=off','-Wall','-Wextra','-Werror','-I'+str(root/'cuda'),'-I'+str(root/'include'),
     '-c',out/'host.cpp','-o',out/'host.o'])
run([a.clang,out/'host.o',*objects,build/'cuda/libkalandra-cuda-language.a',build/'cuda/libkalandra-cuda-program.a',
     build/'cuda/libkalandra-cuda.a',build/'libkalandra-native-runtime.a','-ldl','-pthread','-o',out/'program'])
print(out/'program')
