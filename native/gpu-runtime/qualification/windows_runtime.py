"""Native Windows objects and admission tests for both GPU backends; no GPU substitute."""
import argparse
import json
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--llvm-root',type=Path,required=True)
p.add_argument('--headers',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--backend',choices=('cuda','vulkan'),required=True)
p.add_argument('--arch',choices=('x86_64','arm64'),required=True)
a=p.parse_args()
repo=Path(__file__).resolve().parents[3];source=repo/'native'/a.backend
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
cxx=a.llvm_root/'bin/clang++.exe';readobj=a.llvm_root/'bin/llvm-readobj.exe'
flags=['-std=c++23','-O2','-fms-runtime-lib=static','-DNOMINMAX','-D_CRT_SECURE_NO_WARNINGS','-Wall','-Wextra','-Werror',
       '-Wno-missing-field-initializers','-I'+str(source),'-I'+str(repo/'native/include'),'-isystem'+str(a.headers)]
def run(command,**kwargs):return subprocess.run(list(map(str,command)),check=True,timeout=240,**kwargs)
objects=['Cuda','Program','TensorBridge','Async','Language'] if a.backend=='cuda' else ['Vulkan','VulkanLoader','SpirvInterface','Program','TensorBridge','Async','Language']
for name in objects:
    target=out/(name+'.obj');run([cxx,*flags,'-c',source/(name+'.cpp'),'-o',target])
    text=subprocess.check_output([readobj,'--file-headers',target],text=True)
    assert ('IMAGE_FILE_MACHINE_AMD64' if a.arch=='x86_64' else 'IMAGE_FILE_MACHINE_ARM64') in text
# Use each backend's real loader and executable-location implementation.
path=out/'path.cpp'
path.write_text('#include "ExecutablePath.h"\n#include <filesystem>\nint main(int n,char**v){auto p=kalandra::runtime::'+a.backend+'::executableDirectoryUtf8();return n!=2||!p||*p!=v[1];}\n')
checks=[('path',[path]),('loader',[source/'tests/WindowsLoaderTests.cpp'])]
if a.backend=='vulkan':
    checks[1][1].append(source/'VulkanLoader.cpp')
    checks.append(('interface',[source/'tests/SpirvInterfaceTests.cpp',source/'SpirvInterface.cpp']))
for name,inputs in checks:
    binary=out/(name+'.exe');run([cxx,*flags,*inputs,'-o',binary])
    imports=subprocess.check_output([readobj,'--coff-imports',binary],text=True).lower()
    assert 'nvcuda.dll' not in imports and 'vulkan-1.dll' not in imports
    if name=='loader':
        decoy=out/'decoy.cpp'
        decoy.write_text('#include <windows.h>\nextern "C" BOOL WINAPI DllMain(HINSTANCE,DWORD r,LPVOID){if(r==DLL_PROCESS_ATTACH)TerminateProcess(GetCurrentProcess(),97);return TRUE;}\n')
        dll='nvcuda.dll' if a.backend=='cuda' else 'vulkan-1.dll'
        run([cxx,'-shared','-fms-runtime-lib=static',decoy,'-o',out/dll])
    result=run([binary,*([out] if name=='path' else [])],capture_output=True,text=True,cwd=out)
    print(result.stdout,end='',flush=True)
(out/'qualification.json').write_text(json.dumps({'backend':a.backend,'architecture':a.arch,
    'compiledObjects':objects,'checks':[name for name,_ in checks],
    'gpuExecution':False,'installedCompiler':False,'performanceQualified':False},indent=2)+'\n')
print('PASS:',a.backend,a.arch,'native objects and secure loader/path admission; GPU execution unqualified',flush=True)
