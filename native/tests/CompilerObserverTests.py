"""Keep numerical observation explicit while application compilation stays strict."""
import argparse
from pathlib import Path
import subprocess
import os
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--observer',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
path=a.output/'main.k';path.write_text('fn total(x:i64)->i64:\n  return x+1\nfn main()->i64:\n  return total(41)\n', encoding='utf-8')
exe=a.output/('program.exe' if os.name=='nt' else 'program')
plain=subprocess.run(list(map(str,[a.compiler,path,'-o',exe])),capture_output=True,timeout=60)
assert plain.returncode and b'[S081]' in plain.stderr,plain
for flags in ([],['--test-entry','total']):
    result=subprocess.run(list(map(str,[a.observer,path,*flags,'-o',exe])),capture_output=True,timeout=180)
    assert result.returncode==0 and not result.stderr,result
    result=subprocess.run([exe],capture_output=True,timeout=15)
    assert result.returncode==0 and not result.stderr and result.stdout.replace(b'\r\n',b'\n')==b'42\n',result
version=subprocess.run([a.compiler,'--version'],capture_output=True,timeout=15)
observed=subprocess.run([a.observer,'--version'],capture_output=True,timeout=15)
assert version.returncode==observed.returncode==0 and version.stdout==observed.stdout and not observed.stderr
directory=a.output/'paths with spaces é'
directory.mkdir(exist_ok=True)
source=directory/'source λ.k'
source.write_text('fn main()->i64:\n  return 42\n',encoding='utf-8')
program=directory/('result with spaces.exe' if os.name=='nt' else 'result with spaces')
result=subprocess.run([a.observer,source,'-o',program],capture_output=True,timeout=180)
assert result.returncode==0,result
result=subprocess.run([program],capture_output=True,timeout=15)
assert result.returncode==0 and result.stdout.replace(b'\r\n',b'\n')==b'42\n' and not result.stderr,result
for name in ('missing name', 'missing"quote\\', 'missing\\'):
    arguments=[source,'--test-entry',name,'-o',program]
    direct=subprocess.run([a.compiler,*arguments],capture_output=True,timeout=60)
    forwarded=subprocess.run([a.observer,*arguments],capture_output=True,timeout=60)
    assert direct.returncode!=0 and (direct.returncode,direct.stdout,direct.stderr)==(forwarded.returncode,forwarded.stdout,forwarded.stderr),(name,direct,forwarded)
print('PASS: numerical observer opts in explicitly, preserves existing exports and forwards version; real application compiler stays strict')
