"""CPU-only isolated Vulkan and CUDA header ownership after sharing GPU declarations."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',required=True)
a=p.parse_args()
native=Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='kalandra GPU headers λ ') as folder:
    root=Path(folder)
    for backend in ('vulkan','cuda'):
        for name in (backend,'runtime','gpu-runtime','include/kalandra'):(root/name).mkdir(parents=True,exist_ok=True)
        for name in ('Language','Async','Program','Vulkan' if backend=='vulkan' else 'Cuda','TensorBridge','ExecutablePath'):
            shutil.copyfile(native/backend/(name+'.h'),root/backend/(name+'.h'))
        for source in (native/'runtime').glob('*.h'):shutil.copyfile(source,root/'runtime'/source.name)
        for source in (native/'include/kalandra').glob('*.h'):shutil.copyfile(source,root/'include/kalandra'/source.name)
        resources=[*(native/'gpu-runtime').glob('*Decl.inc'),native/'gpu-runtime/ExecutablePath.inc']
        for source in resources:shutil.copyfile(source,root/'gpu-runtime'/source.name)
        source=root/'consumer.cpp';source.write_text('#include "Language.h"\n#include "ExecutablePath.h"\nint main(){return 0;}\n')
        command=[a.compiler,'-std=c++23','-fsyntax-only','-I'+str(root/backend),'-I'+str(root/'include'),str(source)]
        result=subprocess.run(command,cwd=root,capture_output=True,text=True)
        assert result.returncode==0,result.stderr
        for resource in resources:
            path=root/'gpu-runtime'/resource.name;contents=path.read_bytes();path.unlink()
            try:
                result=subprocess.run(command,cwd=root,capture_output=True,text=True)
                assert result.returncode!=0 and resource.name in result.stderr,(backend,resource.name,result.stderr)
            finally:path.write_bytes(contents)
print('PASS: both backends use isolated owned headers; all five missing shared resources rejected without source fallback')
