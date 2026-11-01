"""ELF import audit and fail-closed loader tests; never needs a Vulkan device."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--probe', type=Path)
p.add_argument('--application', type=Path)
p.add_argument('--cxx', required=True)
p.add_argument('--resource-dir', type=Path)
a = p.parse_args()
resource_flags = [] if a.resource_dir is None else ['-resource-dir='+str(a.resource_dir.resolve())]
assert a.probe or a.application, 'Select a runtime probe or actual source application'
programs = []
if a.probe:
    programs.append((a.probe.resolve(), False))
if a.application:
    programs.append((a.application.resolve(), True))
for binary, _ in programs:
    imports = subprocess.check_output(['readelf', '-d', str(binary)], text=True)
    assert not any('vulkan' in line.lower() for line in imports.splitlines() if 'NEEDED' in line), imports
with tempfile.TemporaryDirectory(prefix='kalandra-vulkan-loader-') as directory:
    root = Path(directory)
    incomplete = root/'incomplete.cpp'
    incomplete.write_text('''#include <cstdlib>
extern "C" int vkCreateInstance(const void*,const void*,void*) { std::abort(); }
''')
    subprocess.run([a.cxx, *resource_flags, '-shared', '-fPIC', str(incomplete), '-o', str(root/'libvulkan.so.1')], check=True)
    absent = root/'absent.cpp'
    absent.write_text('extern "C" void *dlopen(const char*,int) { return nullptr; }\n')
    subprocess.run([a.cxx, *resource_flags, '-shared', '-fPIC', str(absent), '-o', str(root/'absent.so')], check=True)
    for case, environment in [('incomplete', {'LD_LIBRARY_PATH': str(root)}),
                              ('absent', {'LD_PRELOAD': str(root/'absent.so')})]:
        for binary, application in programs:
            result = subprocess.run([binary], env=os.environ | environment, text=True,
                                    capture_output=True, timeout=30)
            if application:
                assert result.returncode == 1 and not result.stdout and result.stderr == 'Vulkan unavailable: 104\n', (case, result)
            else:
                assert result.returncode == 0 and not result.stderr and result.stdout.startswith('PASS:'), (case, result)
            print(case+': '+binary.name+' structured unavailable result', flush=True)
print('PASS: runtime has no loader import and both failure modes fail closed', flush=True)
