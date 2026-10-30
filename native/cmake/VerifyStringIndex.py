"""Qualify native String indexing independently from future slicing."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
compiler, variant, output, source_dir = sys.argv[1:]
source_dir, output = Path(source_dir), Path(output)
output.mkdir(parents=True, exist_ok=True)
fixtures = json.loads((source_dir.parent/'docs/design/fixtures/strings-semantics.json').read_text())
extra={'debug':[],'optimized':['-O2'],'sanitized':['--sanitize'],'thread-sanitized':['--thread-sanitize']}[variant]
def run(args, expected=None):
    result=subprocess.run(list(map(str,args)),capture_output=True)
    assert not result.returncode and not result.stderr,(args,result)
    if expected is not None: assert result.stdout==expected,(args,result.stdout,expected)
    return result

def compile(name,text):
    file=output/(name+'.k');file.write_text(text)
    exe=output/name
    run([compiler,file,'-o',exe,'--emit-llvm',str(exe)+'.ll','--emit-mlir',str(exe)+'.mlir',*extra])
    return exe

def literal(text):
    return '"'+''.join({'"':'\\"','\\':'\\\\','\n':'\\n','\r':'\\r','\t':'\\t','\0':'\\0'}.get(c,c) for c in text)+'"'


vectors=[]
for case in fixtures['text_cases']:
    for selected in case['indices']:
        vectors.append((case['text'],selected['index'],selected['result']))
for n,(text,position,expected) in enumerate(vectors):
    exe=compile('index-'+str(n),'fn pick(text:String,index:int64)->String:\n  text[index]\nfn main()->String:\n  pick('+literal(text)+','+str(position)+')\n')
    run([exe],expected.encode()+b'\n')
for dtype in ['int32','uint32','int64','uint64']:
    exe=compile('width-'+dtype,'fn pick(text:String,index:'+dtype+')->String:\n  text[index]\nfn main()->String:\n  pick("aé👩",2)\n')
    run([exe],'👩\n'.encode())
    for label,position in [('end','3'),('high','4294967295' if dtype=='uint32' else '2147483647' if dtype=='int32' else '18446744073709551615' if dtype=='uint64' else '9223372036854775807')]+([] if dtype.startswith('u') else [('negative','-1')]):
        exe=compile('bounds-'+dtype+'-'+label,'fn pick(text:String,index:'+dtype+')->String:\n  text[index]\nfn main()->String:\n  pick("aé👩",'+position+')\n')
        result=subprocess.run([exe],capture_output=True)
        assert result.returncode and not result.stdout and b'BoundsFault' in result.stderr,result
(output/'pick.k').write_text('export fn pick(text:String,index:uint64)->String:\n  text[index]\n')
exe=compile('imported','from "./pick.k" import pick\nfn main():\n  pick("سلام",2)+"👩‍💻"[1]+"a\\0b"[1]\n')
run([exe],'ا‍\0\n'.encode())
for attempt in range(1,13):
    env=dict(os.environ,KALANDRA_TEST_FAIL_ALLOC_AT=str(attempt),KALANDRA_TEST_REPORT_CLEANUP='1')
    result=subprocess.run([exe],capture_output=True,env=env)
    assert bool(result.returncode)==(attempt<=8),(attempt,result)
    if result.returncode:
        metrics=re.search(rb'cleanup: live=(\d+) allocations=(\d+) releases=(\d+)',result.stderr)
        assert not result.stdout and metrics and metrics[1]==b'0' and metrics[2]==metrics[3],result
    else: assert result.stdout=='ا‍\0\n'.encode() and not result.stderr,result
exe=compile('callback','fn selected(index:int64)->int64:\n  byteLength("aé👩"[index])\nfn main():\n  [0,1,2]->map(selected)\n')
run([exe],b'[1, 2, 4]\n')
exe=compile('single-evaluation','fn text()->String:\n  "aé"\nfn position()->int64:\n  ignored="position"\n  1\nfn main()->String:\n  text()[position()]\n')
run([exe],'é\n'.encode())
result=subprocess.run([exe],capture_output=True,env=dict(os.environ,KALANDRA_TEST_FAIL_ALLOC_AT='4'))
assert not result.returncode and result.stdout=='é\n'.encode() and not result.stderr,result
exe=compile('fault-order','fn text(value:int64)->String:\n  fault=1/value\n  "a"\nfn position()->int64:\n  ignored="position"\n  0\nfn main()->String:\n  text(0)[position()]\n')
result=subprocess.run([exe],capture_output=True,env=dict(os.environ,KALANDRA_TEST_FAIL_ALLOC_AT='1'))
assert result.returncode and not result.stdout and b'ArithmeticFault' in result.stderr,result
exe=compile('short-circuit','fn pick(text:String,index:int64)->String:\n  text[index]\nfn main()->Bool:\n  true or pick("",0)=="x"\n')
run([exe],b'true\n')
for n,expr in enumerate(['"é"[1]','""[0]','"a"[-1]','("é"+"x")[2]','"é"[0][1]','"a"[true]','"a"[1.0]','"a"[0,0]']):
    source=output/f'invalid-{n}.k';source.write_text('fn main():\n  '+expr+'\n')
    paths=[output/'preserved',output/'preserved.ll',output/'preserved.mlir']
    for p in paths:p.write_bytes(b'keep-existing-artifact')
    result=subprocess.run(list(map(str,[compiler,source,'-o',paths[0],'--emit-llvm',paths[1],'--emit-mlir',paths[2],*extra])),capture_output=True)
    assert result.returncode and result.stderr and all(p.read_bytes()==b'keep-existing-artifact' for p in paths),result
print(f'{variant}: {len(vectors)} scalar index vectors, four integer widths, dynamic bounds, imports/maps, single evaluation, fault order, cleanup and artifact preservation passed')
