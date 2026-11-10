"""Run actual GPU Bool/UInt8 identity and cancellation checks with typed descriptors."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--backend',choices=('metal','vulkan','cuda'),required=True)
p.add_argument('--root',type=Path,required=True)
p.add_argument('--driver',type=Path)
p.add_argument('--clang',type=Path)
p.add_argument('--metal-runtime',type=Path)
p.add_argument('--runtime',type=Path)
p.add_argument('--variant',choices=('optimized','sanitized','thread-sanitized'),default='optimized')
a=p.parse_args()
a.root=a.root.resolve();a.root.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[2]
def run(command):
    result=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=240)
    assert not result.returncode,(command,result.returncode,result.stdout,result.stderr)
    assert not any(x in result.stderr for x in ('AddressSanitizer','ThreadSanitizer','runtime error:')),result.stderr
    return result.stdout
source=a.root/'byte.k'
source.write_text('fn byte(n:u8)->u8:\n  return n\nfn main()->u8 ! TaskError:\n  return try parallel:\n    gpu=devices.gpu()\n    try await spawn byte(u8(1)) on gpu\n')
bundle=a.root/'byte'
run([a.compiler.resolve(), '--test-entry', 'main',source,'--target',a.backend,'-O2','-o',bundle])
if a.backend=='metal':
    assert a.clang and a.metal_runtime and a.runtime
    flags={'optimized':['-O2'],
           'sanitized':['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'],
           'thread-sanitized':['-O1','-g','-fsanitize=thread','-fno-omit-frame-pointer']}[a.variant]
    driver=a.root/'runtime-probe'
    run([a.clang,'-std=c++23','-Wall','-Wextra','-Werror',*flags,'-DKALANDRA_BOOL_METAL',
         repo/'native/gpu-source/DeviceBoolRuntimeTests.cpp',a.metal_runtime.resolve(),a.runtime.resolve(),
         '-framework','Foundation','-framework','Metal','-o',driver])
else:
    assert a.driver
    driver=a.driver.resolve()
shader=bundle/('kernel0'+{'metal':'.metallib','vulkan':'.spv','cuda':'.ptx'}[a.backend])
output=run([driver,shader]);assert 'PASS Bool GPU result identity' in output
print(output,flush=True)
(a.root/'report.json').write_text(json.dumps({'backend':a.backend,'variant':a.variant,'gpuExecution':True,
    'sourceSHA256':hashlib.sha256(source.read_bytes()).hexdigest(),
    'shaderSHA256':hashlib.sha256(shader.read_bytes()).hexdigest(),'stdout':output},indent=2)+'\n')
