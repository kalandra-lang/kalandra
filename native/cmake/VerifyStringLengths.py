"""Qualify native String count queries independently from future indexing/slicing."""
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

for index,case in enumerate(fixtures['text_cases']):
    exe=compile('counts-'+str(index),'fn scalarCount(text:String)->int64:\n  length(text)\nfn bytes(text:String)->int64:\n  byteLength(text)\nfn main():\n  text='+literal(case['text'])+'\n  [scalarCount(text),bytes(text)]\n')
    run([exe],f'[{case["scalar_length"]}, {case["byte_length"]}]\n'.encode())
(output/'queries.k').write_text('export fn measure(text:String)->int64:\n  length(text)*100+byteLength(text)\n')
exe=compile('imported','from "./queries.k" import measure\nfn main():\n  measure("سلام"+"\\0"+"👩‍💻")\n')
run([exe],b'820\n')
for failure in range(1,13):
    env=dict(os.environ,KALANDRA_TEST_FAIL_ALLOC_AT=str(failure),KALANDRA_TEST_REPORT_CLEANUP='1')
    result=subprocess.run([exe],env=env,capture_output=True)
    assert bool(result.returncode)==(failure<=5),(failure,result)
    if result.returncode:
        assert not result.stdout,result
        metrics=re.search(rb'cleanup: live=(\d+) allocations=(\d+) releases=(\d+)',result.stderr)
        assert metrics and metrics[1]==b'0' and metrics[2]==metrics[3],result.stderr
    else: assert result.stdout==b'820\n' and not result.stderr,result
exe=compile('pipeline','fn main():\n  ["é"->length,"é"->byteLength]\n')
run([exe],b'[1, 2]\n')
exe=compile('callback','fn count(value:int64)->int64:\n  value+length("👩‍💻")+byteLength("é")\nfn main():\n  [1,2]->map(count)\n')
run([exe],b'[6, 7]\n')
for name,call,expected in [('scalar','length',b'3\n'),('bytes','byteLength',b'11\n')]:
    exe=compile(name+'-single-evaluation','fn text()->String:\n  "👩‍💻"\nfn main()->int64:\n  '+call+'(text())\n')
    run([exe],expected)
    # One literal allocation: a second evaluation would hit the injected failure.
    env=dict(os.environ,KALANDRA_TEST_FAIL_ALLOC_AT='2')
    result=subprocess.run([exe],env=env,capture_output=True)
    assert not result.returncode and result.stdout==expected and not result.stderr,result
    exe=compile(name+'-short-circuit','fn explode(value:int64)->String:\n  fault=1/value\n  "a"\nfn main()->Bool:\n  true or '+call+'(explode(0))==0\n')
    run([exe],b'true\n')
    exe=compile(name+'-fault','fn explode(value:int64)->String:\n  fault=1/value\n  "a"\nfn main()->int64:\n  '+call+'(explode(0))\n')
    result=subprocess.run([exe],capture_output=True)
    assert result.returncode and not result.stdout and b'ArithmeticFault' in result.stderr,result

for index,expr in enumerate(['byteLength(1)','byteLength(true)','byteLength([1,2])','byteLength(tensor([1,2]))','shape("a")','length()','byteLength()','length("a","b")','byteLength("a","b")']):
    file=output/f'invalid-{index}.k';file.write_text('fn main():\n  '+expr+'\n')
    artifacts=[output/'preserved',output/'preserved.ll',output/'preserved.mlir']
    for p in artifacts:p.write_bytes(b'keep-existing-artifact')
    result=subprocess.run(list(map(str,[compiler,file,'-o',artifacts[0],'--emit-llvm',artifacts[1],'--emit-mlir',artifacts[2],*extra])),capture_output=True)
    assert result.returncode and result.stderr,result
    assert all(p.read_bytes()==b'keep-existing-artifact' for p in artifacts)
print(f'{variant}: {len(fixtures["text_cases"])} scalar/byte vectors, helpers/imports, pipelines/maps, single evaluation, short circuit, failure cleanup and rejected artifacts passed')
