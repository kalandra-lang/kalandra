"""Qualify native String half-open Unicode scalar slicing."""
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
    for selected in case['slices']:
        vectors.append((case['text'],selected['start'],selected['end'],selected['result']))
for n,(text,start,end,expected) in enumerate(vectors):
    exe=compile('slice-'+str(n),'fn cut(text:String,start:int64,end:int64)->String:\n  slice(text,start,end)\nfn main()->String:\n  cut('+literal(text)+','+str(start)+','+str(end)+')\n')
    run([exe],expected.encode()+b'\n')
widths=['int32','uint32','int64','uint64']
functions=[];checks=[]
for a in widths:
    for b in widths:
        name='cut_'+a+'_'+b
        functions.append('fn '+name+'(text:String,start:'+a+',end:'+b+')->String:\n  slice(text,start,end)\n')
        checks.append(name+'("aé👩",1,3)=="é👩"')
exe=compile('mixed-widths',''.join(functions)+'fn main()->Bool:\n  '+' and '.join(checks)+'\n')
run([exe],b'true\n')
for dtype in widths:
    cases=[('reversed','2','1'),('past-end','0','4'),('high-start','4294967295' if dtype=='uint32' else '2147483647' if dtype=='int32' else '18446744073709551615' if dtype=='uint64' else '9223372036854775807','3')]
    if not dtype.startswith('u'):cases.append(('negative-end','0','-1'))
    for label,start,end in cases:
        exe=compile('bounds-'+dtype+'-'+label,'fn cut(text:String,start:'+dtype+',end:'+dtype+')->String:\n  slice(text,start,end)\nfn main()->String:\n  cut("aé👩",'+start+','+end+')\n')
        result=subprocess.run([exe],capture_output=True)
        assert result.returncode and not result.stdout and b'BoundsFault' in result.stderr,result
(output/'cut.k').write_text('export fn cut(text:String,start:uint32,end:int64)->String:\n  slice(text,start,end)\n')
exe=compile('imported','from "./cut.k" import cut\nfn main():\n  cut("aé👩",1,3)+slice("a\\0b",1,2)\n')
run([exe],'é👩\0\n'.encode())
for attempt in range(1,13):
    env=dict(os.environ,KALANDRA_TEST_FAIL_ALLOC_AT=str(attempt),KALANDRA_TEST_REPORT_CLEANUP='1')
    result=subprocess.run([exe],capture_output=True,env=env)
    assert bool(result.returncode)==(attempt<=5),(attempt,result)
    if result.returncode:
        metrics=re.search(rb'cleanup: live=(\d+) allocations=(\d+) releases=(\d+)',result.stderr)
        assert not result.stdout and metrics and metrics[1]==b'0' and metrics[2]==metrics[3],result
    else:assert result.stdout=='é👩\0\n'.encode() and not result.stderr,result
exe=compile('pipeline','fn main()->String:\n  "aé👩"->slice(1,3)\n')
run([exe],'é👩\n'.encode())
exe=compile('callback','fn bytes(start:int64)->int64:\n  byteLength(slice("aé👩",start,3))\nfn main():\n  [0,1,2,3]->map(bytes)\n')
run([exe],b'[7, 6, 4, 0]\n')
exe=compile('single-evaluation','fn text()->String:\n  "aé👩"\nfn start()->int64:\n  ignored="start"\n  1\nfn end()->int64:\n  ignored="end"\n  3\nfn main()->String:\n  slice(text(),start(),end())\n')
run([exe],'é👩\n'.encode())
result=subprocess.run([exe],capture_output=True,env=dict(os.environ,KALANDRA_TEST_FAIL_ALLOC_AT='5'))
assert not result.returncode and result.stdout=='é👩\n'.encode() and not result.stderr,result
for name,start in [('source-fault','bad(0)'),('start-fault','good()')]:
    source='fn bad(value:int64)->String:\n  fault=1/value\n  "a"\nfn good()->String:\n  "a"\nfn start()->int64:\n  fault=1/zero()\n  0\nfn zero()->int64:\n  0\nfn end()->int64:\n  ignored="end"\n  1\nfn main()->String:\n  slice('+start+',start(),end())\n'
    exe=compile(name,source)
    result=subprocess.run([exe],capture_output=True,env=dict(os.environ,KALANDRA_TEST_FAIL_ALLOC_AT='1' if name=='source-fault' else '2'))
    assert result.returncode and not result.stdout and b'ArithmeticFault' in result.stderr,result
    assert (b'at source 2:' if name=='source-fault' else b'at source 7:') in result.stderr,result
exe=compile('short-circuit','fn cut(text:String,start:int64,end:int64)->String:\n  slice(text,start,end)\nfn main()->Bool:\n  true or cut("a",2,1)=="x"\n')
run([exe],b'true\n')
for n,expr in enumerate(['slice("abc",2,1)','slice("é",0,2)','slice("a",-1,1)','slice("a",0,-1)','slice("",1,1)','slice("a",0,0)[0]','slice()','slice("a",0)','slice("a",0,1,1)','slice(tensor([1,2]),0,1)','slice("a",true,1)','slice("a",0,1.0)']):
    source=output/f'invalid-{n}.k';source.write_text('fn main():\n  '+expr+'\n')
    paths=[output/'preserved',output/'preserved.ll',output/'preserved.mlir']
    for p in paths:p.write_bytes(b'keep-existing-artifact')
    result=subprocess.run(list(map(str,[compiler,source,'-o',paths[0],'--emit-llvm',paths[1],'--emit-mlir',paths[2],*extra])),capture_output=True)
    assert result.returncode and result.stderr and all(p.read_bytes()==b'keep-existing-artifact' for p in paths),result
print(f'{variant}: {len(vectors)} scalar slice vectors, 16 mixed integer signatures, dynamic bounds, imports/maps, single evaluation, fault order, cleanup and artifact preservation passed')
