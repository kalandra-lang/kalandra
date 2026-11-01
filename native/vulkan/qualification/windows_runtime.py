"""Compile actual Windows Vulkan runtime objects and run CPU-only admission tests.

This is not a Windows GPU execution or installed compiler qualification.
"""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--llvm-root', type=Path, required=True)
p.add_argument('--headers', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--arch', choices=('x86_64', 'arm64'), required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[3]
source = repo/'native/vulkan'
root = a.output.resolve()
root.mkdir(parents=True, exist_ok=False)
cxx = a.llvm_root/'bin/clang++.exe'
readobj = a.llvm_root/'bin/llvm-readobj.exe'
flags = ['-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
         '-I'+str(source), '-I'+str(repo/'native/include'), '-isystem'+str(a.headers/'include')]
def run(command, **kwargs):
    return subprocess.run(list(map(str, command)), check=True, timeout=180, **kwargs)
objects = []
for name in ('Vulkan', 'VulkanLoader', 'SpirvInterface', 'Program', 'TensorBridge', 'Async', 'Language'):
    obj = root/(name+'.obj')
    run([cxx, *flags, '-c', source/(name+'.cpp'), '-o', obj])
    text = subprocess.check_output([readobj, '--file-headers', obj], text=True)
    assert ('IMAGE_FILE_MACHINE_AMD64' if a.arch == 'x86_64' else 'IMAGE_FILE_MACHINE_ARM64') in text
    objects.append(obj.name)
for name, files in [('interface', ['tests/SpirvInterfaceTests.cpp', 'SpirvInterface.cpp']),
                    ('path', ['tests/ExecutablePathTests.cpp']),
                    ('loader', ['tests/WindowsLoaderTests.cpp', 'VulkanLoader.cpp'])]:
    binary = root/(name+'.exe')
    run([cxx, *flags, *(source/file for file in files), '-o', binary])
    imports = subprocess.check_output([readobj, '--coff-imports', binary], text=True)
    assert 'vulkan-1.dll' not in imports.lower()
    if name == 'loader':
        # A local DLL must never intercept the system-only loader lookup. Any
        # accidental load exits 97 before a function can be published.
        decoy = root/'decoy.cpp'
        decoy.write_text('#include <windows.h>\nextern "C" BOOL WINAPI DllMain(HINSTANCE,DWORD reason,LPVOID) { if(reason==DLL_PROCESS_ATTACH) TerminateProcess(GetCurrentProcess(),97); return TRUE; }\n')
        run([cxx, '-shared', decoy, '-o', root/'vulkan-1.dll'])
    result = run([binary, *([root] if name == 'path' else [])], capture_output=True, text=True, cwd=root)
    print(result.stdout, end='', flush=True)
(root/'qualification.json').write_text(json.dumps({'architecture': a.arch, 'compiledObjects': objects,
    'checks': ['SPIR-V interface', 'executable-relative path', 'system-only loader with local DLL decoy', 'no Vulkan PE import'],
    'gpuExecution': False, 'installedCompiler': False}, indent=2)+'\n')
print('PASS: seven real Windows runtime objects and three native admission tests; GPU execution unqualified', flush=True)
