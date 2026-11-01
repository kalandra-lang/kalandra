"""CPU-only CUDA CLI qualification: successful builds and protected source outputs."""
import argparse
from pathlib import Path
import subprocess
import tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
a=p.parse_args();compiler=a.compiler.resolve()
repo=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='kalandra CUDA command λ ') as folder:
    root=Path(folder)
    source=root/'program.k'
    text=(repo/'native/metal-source/placed.k').read_bytes();source.write_bytes(text)
    def run(*args):return subprocess.run([compiler,'--target','cuda',*args],capture_output=True,text=True,timeout=120)
    for optimization in ('-O0','-O2'):
        output=root/optimization
        result=run(optimization,source,'-o',output)
        assert result.returncode==0,result.stderr
        assert (output/'program').is_file() and list(output.glob('*.ptx'))
    for flag in ('--sanitize','--thread-sanitize','--definitely-not-a-flag'):
        output=root/'rejected'
        result=run(flag,source,'-o',output)
        assert result.returncode==2 and not output.exists(),(flag,result)
        assert source.read_bytes()==text
    for name in ('host.cpp','kernel0.ptx','kernel0.ptx.json','kernel0.ptx.fixed-tree.ptx',
                 'kernel0.ptx.fixed-tree.ptx.json','THIRD_PARTY_NOTICES.txt','program'):
        output=root/('alias-'+name);output.mkdir()
        (output/name).symlink_to(source)
        result=run(source,'-o',output)
        assert result.returncode==1 and source.read_bytes()==text,(name,result)
    retired=root/'retired.kalandra';retired.write_bytes(text)
    result=run(retired,'-o',root/'retired-output')
    assert result.returncode==1 and not (root/'retired-output').exists(),result
    result=run('--target-triple','aarch64-unknown-linux-gnu',source,'-o',root/'cross')
    assert result.returncode==2 and not (root/'cross').exists(),result
print('PASS: O0/O2 builds, unsupported options, retired extension, cross-target rejection and protected output symlinks')
