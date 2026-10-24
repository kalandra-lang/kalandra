"""Independent arithmetic/state checks for recursive returns in all numeric widths and Bool."""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--baseline', type=Path)
p.add_argument('--clang', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
variants = [('debug', '-O0', ['-O0']), ('optimized', '-O2', ['-O2']),
            ('sanitized', '--sanitize', ['-O1', '-g', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]
if sys.platform != 'win32':
    variants.append(('thread-sanitized', '--thread-sanitize',
                     ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer']))
types = {**{s+str(w):'std::'+s+str(w)+'_t' for s in ('int','uint') for w in (8,16,32,64)},
         'float32':'float', 'float64':'double', 'Bool':'bool'}
compilers = [('current', a.compiler)]
if a.baseline: compilers.insert(0, ('baseline', a.baseline))
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
frozen = {str(c):sha(c) for _,c in compilers}
records = []
env = dict(os.environ)
if sys.platform == 'win32':
    resources = list(a.clang.resolve().parent.parent.glob('lib/clang/*/lib/windows'))
    env['PATH'] = os.pathsep.join(map(str, resources)) + os.pathsep + env.get('PATH', '')
def run(command):
    r = subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=240,env=env)
    assert r.returncode == 0, (command,r.returncode,r.stdout[-2000:],r.stderr[-3000:])
    return r.stdout
for numeric,ctype in types.items():
    source = a.output/(numeric+'.k')
    op = '==' if numeric == 'Bool' else '+'
    source.write_text(f'fn tree(v:{numeric},n:int)->{numeric}:\n  if n==0:v\n  else:tree(v,n-1){op}tree(v,n-1)\nfn main()->{numeric}:\n  tree('+('true' if numeric=='Bool' else '1')+',3)\n')
    for name,flag,flags in variants:
        outputs = []
        for version,compiler in compilers:
            binary=a.output/(numeric+'-'+name+'-'+version+('.exe' if sys.platform=='win32' else ''));ir=binary.with_suffix('.ll')
            standalone=binary.with_name(binary.stem+'-standalone'+binary.suffix)
            run([compiler,source,'--test-entry','tree','--emit-llvm',ir,flag,'-o',standalone])
            runtime=a.compiler.parent/(('' if sys.platform=='win32' else 'lib')+'kalandra-native-runtime'+('-'+name if 'sanitized' in name else '')+('.lib' if sys.platform=='win32' else '.a'))
            run([a.clang,'-std=c++23','-Wno-override-module','-ffp-contract=off','-pthread',*flags,
                 '-DTEST_SCALAR='+ctype,*(['-fms-runtime-lib=static'] if sys.platform=='win32' else []),
                 root/'native/tests/RecursiveScalarRuntimeTests.cpp',ir,runtime,'-o',binary])
            output=run([binary]);outputs.append(output)
            records.append(dict(numeric=numeric,variant=name,compiler=version,compiler_sha256=frozen[str(compiler)],
                                cases=int(output.split()[0]),state_sha256=hashlib.sha256(output.encode()).hexdigest(),
                                ir_sha256=sha(ir),result=output.strip()))
        assert len(set(outputs))==1,(numeric,name,outputs)
    print(numeric+': all build modes match independent references and baseline state',flush=True)
assert all(sha(Path(c))==h for c,h in frozen.items()), 'compiler changed during qualification'
(a.output/'qualification.json').write_text(json.dumps(records,indent=2)+'\n')
