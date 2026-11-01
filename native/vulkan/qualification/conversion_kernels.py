"""Generate checked all-destination kernels per source type; no CPU substitute."""
import argparse
from pathlib import Path
import subprocess
import re
p=argparse.ArgumentParser()
for name in ('build','llvm-root','output'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--audit-only',action='store_true')
a=p.parse_args();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
tools=a.build.resolve()/'native/gpu-source'
def run(command):subprocess.run(list(map(str,command)),check=True)
for mode in ('hardware','software','hardware-with-software-fallback'):
    for name in ('int8','uint8','int16','uint16','int32','uint32','int64','uint64','float32','float64'):
        base=a.output/mode/name;base.parent.mkdir(parents=True,exist_ok=True)
        if not a.audit_only:
            run([tools/'kalandra-vulkan-conversion-kernels',name,str(base)+'.mlir',str(base)+'.cpu.mlir',mode,'--vulkan'])
            run([tools/'kalandra-vulkan-lower',str(base)+'.mlir',str(base)+'.lowered.mlir',mode])
            run([a.llvm_root/'bin/mlir-opt',str(base)+'.lowered.mlir','--verify-each',
              '--pass-pipeline=builtin.module(canonicalize,cse,convert-gpu-to-spirv,spirv.module(spirv-lower-abi-attrs,spirv-update-vce))',
              '-o',str(base)+'.converted.mlir'])
            run([tools/'kalandra-vulkan-extract',str(base)+'.converted.mlir',str(base)+'.kernel.mlir'])
            run([a.llvm_root/'bin/mlir-translate','--no-implicit-module','--serialize-spirv',str(base)+'.kernel.mlir','-o',str(base)+'.spv'])
            run(['spirv-val','--target-env','vulkan1.0',str(base)+'.spv'])
        assembly=subprocess.check_output(['spirv-dis',str(base)+'.spv'],text=True)
        doubles=re.findall(r'(%\w+) = OpTypeFloat 64',assembly)
        singles=re.findall(r'(%\w+) = OpTypeFloat 32',assembly)
        if mode=='hardware':
            assert doubles and 'OpCapability Float64' in assembly,(mode,name)
            assert re.search(r'Op(?:Convert[SU]ToF|ConvertFTo[SU]|FConvert)\b',assembly),(mode,name)
            if name not in ('float32','float64'):
                assert re.search(r'OpConvert[SU]ToF '+re.escape(doubles[0])+r'\b',assembly),(mode,name)
        else:
            assert not doubles and 'OpCapability Float64' not in assembly,(mode,name)
            if mode=='software' or name=='float64':
                assert not singles,(mode,name)
            else:
                assert singles,(mode,name,'native binary32 unexpectedly disappeared')
        print('PASS:',mode,name,'all destination artifacts audited' if a.audit_only else 'all destination artifacts compiled/validated/audited',flush=True)
