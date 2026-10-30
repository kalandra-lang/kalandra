"""Qualify only the implemented literal/value subset of the String contract."""
import json
import os
import re
from pathlib import Path
import subprocess
import sys

compiler, clang, runtime, variant, output, source_dir = sys.argv[1:]
source_dir, output = Path(source_dir), Path(output)
output.mkdir(parents=True, exist_ok=True)
fixtures = json.loads((source_dir.parent / 'docs/design/fixtures/strings-semantics.json').read_text())
extra = {'debug': [], 'optimized': ['-O2'], 'sanitized': ['--sanitize'], 'thread-sanitized': ['--thread-sanitize']}[variant]
flags = {'debug': ['-O0'], 'optimized': ['-O2'], 'sanitized': ['-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all'], 'thread-sanitized': ['-O1','-fsanitize=thread']}[variant]
def run(args, expected=None, env=None):
    result = subprocess.run(list(map(str,args)), capture_output=True, env=env)
    assert result.returncode == 0, (args,result.returncode,result.stdout,result.stderr)
    assert not result.stderr, (args,result.stderr)
    if expected is not None: assert result.stdout == expected, (args,result.stdout,expected)
    return result

def compile(name, source):
    path = output / (name+'.k')
    path.write_text(source)
    exe = output/name
    run([compiler,path,'-o',exe,'--emit-llvm',str(exe)+'.ll','--emit-mlir',str(exe)+'.mlir',*extra])
    return exe

def literal(text):
    # JSON has a different Unicode escape syntax; keep raw scalars and spell controls explicitly.
    return '"'+''.join({'"':'\\"','\\':'\\\\','\n':'\\n','\r':'\\r','\t':'\\t','\0':'\\0'}.get(c,c) for c in text)+'"'

cases = [(literal(c['text']),c['text']) for c in fixtures['text_cases']]
cases += [(c['source'],c['decoded']) for c in fixtures['valid_literal_cases']]
cases += [(r'"\u{7f}\u{80}\u{7ff}\u{800}\u{ffff}\u{10000}"', ''.join(map(chr,[0x7f,0x80,0x7ff,0x800,0xffff,0x10000])))]
for index,(spelling,decoded) in enumerate(cases):
    exe=compile('literal-'+str(index),'fn identity(value:String)->String:\n  value\nfn main()->String:\n  identity('+spelling+')\n')
    run([exe],decoded.encode()+b'\n')

helper = output/'text.k'
helper.write_text('export fn select(flag:Bool,a:String,b:String)->String:\n  if flag:\n    a\n  else:\n    b\n')
exe=compile('module','from "./text.k" import select\nfn main()->String:\n  select(false,"unused","سلام\\0👩‍💻")\n')
run([exe],'سلام\0👩‍💻\n'.encode())
run([clang,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-override-module',*flags,
     source_dir/'tests/StringGeneratedTests.cpp',str(exe)+'.ll',runtime,'-o',output/'generated-harness'])
run([output/'generated-harness'])
for failure in range(1,9):
    env=dict(os.environ,KALANDRA_TEST_FAIL_ALLOC_AT=str(failure),KALANDRA_TEST_REPORT_CLEANUP='1')
    result=subprocess.run([exe],capture_output=True,env=env)
    if result.returncode:
        assert result.stdout == b'', result
        metrics=re.search(rb'cleanup: live=(\d+) allocations=(\d+) releases=(\d+)',result.stderr)
        assert metrics and metrics[1]==b'0' and metrics[2]==metrics[3], result.stderr
    else:
        assert result.stdout == 'سلام\0👩‍💻\n'.encode() and not result.stderr, result

for name,returned,expected in [('integer','int64',b'7\n'),('boolean','Bool',b'true\n'),('array','[int64]',b'[1, 2]\n')]:
    expr={'integer':'7','boolean':'true','array':'[1,2]'}[name]
    exe=compile(name,'fn main()->'+returned+':\n  ignored="owned text"\n  '+expr+'\n')
    run([exe],expected)

bad_sources=list(fixtures['invalid_literal_sources'])+['"\\u{123456','"a\rb"']
# Every rejected input must preserve previously emitted files.
for index,bad in enumerate(bad_sources):
    file=output/('invalid-'+str(index)+'.k');file.write_text('fn main()->String:\n  '+bad+'\n')
    paths=[output/'preserved',output/'preserved.ll',output/'preserved.mlir']
    for p in paths:p.write_bytes(b'keep-existing-artifact')
    result=subprocess.run([compiler,file,'-o',paths[0],'--emit-llvm',paths[1],'--emit-mlir',paths[2],*extra],capture_output=True)
    assert result.returncode != 0 and result.stderr, (bad,result)
    assert all(p.read_bytes()==b'keep-existing-artifact' for p in paths)
for index,hex_bytes in enumerate(fixtures['invalid_utf8_hex']):
    file=output/('invalid-utf8-'+str(index)+'.k')
    file.write_bytes(b'fn main()->String:\n  "'+bytes.fromhex(hex_bytes)+b'"\n')
    result=subprocess.run([compiler,file,'-o',output/'invalid-utf8',*extra],capture_output=True)
    assert result.returncode != 0, hex_bytes
print(f'{variant}: {len(cases)} literal vectors, modules, generated ownership, scalar/collection cleanup and invalid-source artifact preservation passed')

# Equality/concatenation reuse the accepted literal and Unicode conformance data.
operation_cases = list(fixtures['equality_cases']) + [
    {'left':'','right':'','equal':True},
    {'left':'a\0b','right':'a\0c','equal':False},
    {'left':'prefix','right':'prefix-more','equal':False},
    {'left':'👩‍💻','right':'👩‍💻','equal':True},
]
for index,case in enumerate(operation_cases):
    for operator,invert in [('==',False),('!=',True)]:
        name='equal-'+str(index)+('ne' if invert else 'eq')
        exe=compile(name,'fn same(a:String,b:String)->Bool:\n  a '+operator+' b\nfn main()->Bool:\n  same('+literal(case['left'])+','+literal(case['right'])+')\n')
        run([exe],b'true\n' if case['equal'] != invert else b'false\n')
for index,case in enumerate(fixtures['concatenation_cases']):
    exe=compile('concat-'+str(index),'fn append(a:String,b:String)->String:\n  a+b\nfn main()->String:\n  append('+literal(case['left'])+','+literal(case['right'])+')\n')
    run([exe],case['result'].encode()+b'\n')

(output/'operations.k').write_text('export fn append(a:String,b:String)->String:\n  a+b\n')
exe=compile('concat-owned','from "./operations.k" import append\nfn main()->String:\n  append(append("سلام","\\0"),append("👩‍💻",""))\n')
run([exe],'سلام\0👩‍💻\n'.encode())
run([clang,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-override-module',*flags,
     '-DSTRING_FAILURE_POSITIONS=32',source_dir/'tests/StringGeneratedTests.cpp',str(exe)+'.ll',runtime,'-o',output/'operations-harness'])
run([output/'operations-harness'])
for failure in range(1,33):
    env=dict(os.environ,KALANDRA_TEST_FAIL_ALLOC_AT=str(failure),KALANDRA_TEST_REPORT_CLEANUP='1')
    result=subprocess.run([exe],capture_output=True,env=env)
    if result.returncode:
        assert result.stdout == b'',result
        metrics=re.search(rb'cleanup: live=(\d+) allocations=(\d+) releases=(\d+)',result.stderr)
        assert metrics and metrics[1]==b'0' and metrics[2]==metrics[3],result.stderr
    else: assert result.stdout=='سلام\0👩‍💻\n'.encode() and not result.stderr,result

for name,expr,expected in [
    ('precedence','"a"+"b"=="ab"',b'true\n'),
    ('short-and','false and explode(0)=="x"',b'false\n'),
    ('short-or','true or explode(0)=="x"',b'true\n'),
]:
    exe=compile(name,'fn explode(value:int64)->String:\n  fault=1/value\n  "x"\nfn main()->Bool:\n  '+expr+'\n')
    run([exe],expected)
exe=compile('fault-order','fn explode(value:int64)->String:\n  fault=1/value\n  "x"\nfn main()->String:\n  explode(0)+explode(1)\n')
result=subprocess.run([exe],capture_output=True)
assert result.returncode and result.stdout==b'' and b'ArithmeticFault' in result.stderr,result
# String work inside a synchronous numeric/Bool callback still uses the ordinary CPU backend.
exe=compile('callback','fn positive(value:int64)->Bool:\n  "a"+"b"=="ab" and value>0\nfn main():\n  [-1,2]->map(positive)\n')
run([exe],b'[false, true]\n')
print(f'{variant}: {len(operation_cases)*2} exact equality cases, {len(fixtures["concatenation_cases"])} concatenation vectors, imported/chained ownership, 32 allocation positions, fault order and short-circuit/callback composition passed')
