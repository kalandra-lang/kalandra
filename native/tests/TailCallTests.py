"""Scalar tail recursion retains logical admission, polls, faults and exact values."""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

SOURCE = '''fn gcd(a:int,b:int)->int:
  if b == 0:a
  else:gcd(b,a%b)
fn swap(a:int,b:int,n:int)->int:
  if n == 0:a*1000+b
  else:swap(b,a,n-1)
fn choose(a:int,b:int,n:int)->int:
  if n == 0:a
  else:
    if a%2 == 0:choose(a+b,-b,n-1)
    else:choose(a-b,b,n-1)
fn total(a:int,n:int)->int:
  if n == 0:a
  else:total(a+n,n-1)
fn divide(a:int,b:int,n:int)->int:
  if n == 0:a/b
  else:divide(a,b,n-1)
fn ordinary(a:int,n:int)->int:
  if n == 0:a
  else:ordinary(a,n-1)+1
fn tree(a:int,n:int)->int:
  if n == 0:gcd(a*13+7,a*7+11)
  else:tree(a*2,n-1)+tree(a*2+1,n-1)
fn helper(a:int)->int:
  a+1
fn mixed(a:int,n:int)->int:
  if n == 0:a
  else:mixed(helper(a),n-1)
fn mixed_self(a:int,n:int)->int:
  if n == 0:a
  else:
    if n%2 == 0:mixed_self(a+1,n-1)
    else:mixed_self(a,n-1)+1
fn run(kind:int,a:int,b:int,n:int)->int:
  if kind == 0:gcd(a,b)
  else:
    if kind == 1:swap(a,b,n)
    else:
      if kind == 2:choose(a,b,n)
      else:
        if kind == 3:total(a,n)
        else:
          if kind == 4:divide(a,b,n)
          else:
            if kind == 5:ordinary(a,n)
            else:
              if kind == 6:tree(a,n)
              else:
                if kind == 7:mixed(a,n)
                else:mixed_self(a,n)
fn main()->int:
  run(1,37,7,2)
'''
FLOAT = '''fn bounce(value:float64,n:int)->float64:
  if n == 0:value
  else:bounce(-value,n-1)
fn main()->float64:
  bounce(1.25,7)
'''
ELIGIBILITY = {
    'large-signature': ('''fn recurse(a:int,b:int,c:int,d:int,e:int,f:int,g:int,h:int,i:int,n:int)->int:
  if n == 0:a+b+c+d+e+f+g+h+i
  else:recurse(a,b,c,d,e,f,g,h,i,n-1)
fn main()->int:
  recurse(1,2,3,4,5,6,7,8,6,30)
''', '42\n', False),
    'owned-tensor': ('''fn recurse(values:Tensor[int,?],n:int)->int:
  if n == 0:values[0]
  else:recurse(values,n-1)
fn main()->int:
  values:Tensor[int,1]=tensor([42])
  recurse(values,30)
''', '42\n', False),
    'float-recurrence': ('''fn recurse(value:float64,n:int)->float64:
  if n == 0:value
  else:recurse(value*0.5+0.125,n-1)
fn main()->float64:
  recurse(2.0,4)
''', '0.359375\n', True),
}

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--compiler', type=Path, required=True)
    p.add_argument('--baseline', type=Path)
    p.add_argument('--arithmetic', choices=['hardware', 'software'], default='hardware')
    p.add_argument('--clang', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    records = []
    frozen_compilers = {str(c): hashlib.sha256(c.read_bytes()).hexdigest()
                        for c in (a.compiler, *([a.baseline] if a.baseline else []))}
    def run(args):
        r = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=240)
        assert r.returncode == 0, (args, r.returncode, r.stdout[-2000:], r.stderr[-5000:])
        return r
    variants = [('debug', '-O0', ['-O0']), ('optimized', '-O2', ['-O2']),
                ('sanitized', '--sanitize', ['-O1', '-g', '-fsanitize=address,undefined',
                                           '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]
    if sys.platform != 'win32':
        variants.append(('thread-sanitized', '--thread-sanitize',
                         ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer']))
    for name, flag, flags in variants:
        for floating, source, entry in [(False, SOURCE, 'run'), (True, FLOAT, 'bounce')]:
            label = name + ('-float' if floating else '-int')
            path = a.output / (label + '.k')
            path.write_text(source)
            outputs = []
            compilers = [('current', a.compiler)]
            if a.baseline:
                compilers.insert(0, ('baseline', a.baseline))
            for version, compiler in compilers:
                binary = a.output / (label + '-' + version)
                ir = binary.with_suffix('.ll')
                run([compiler, path, '--test-entry', entry, '--arithmetic', a.arithmetic,
                     '--emit-llvm', ir, flag,
                     '-o', str(binary) + '-standalone'])
                text = ir.read_text()
                if version == 'current':
                    assert 'tail.header' in text and 'tail.unwind' in text
                    if not floating:
                        def body(index):
                            return re.search(r'define [^\n]*@kalandra_fn_' + str(index) +
                                             r'\([^\n]*\) [^{]*\{(.*?)\n\}', text, re.S)[1]
                        assert 'tail.header' not in body(5), 'non-tail recursion was rewritten'
                        assert 'tail.header' not in body(6), 'binary recursion was rewritten'
                        assert 'load atomic i8' in body(0) and 'acquire' in body(0), \
                            'pure token-bound calls must retain an acquire poll per admission'
                        assert 'tail.header' in body(8) and 'tail.private.depth' not in body(8), \
                            'helper calls must retain ordinary admission'
                        assert 'tail.header' in body(9) and 'tail.private.depth' not in body(9), \
                            'mixed tail/non-tail self calls must retain ordinary admission'
                runtime = a.compiler.parent / (('' if sys.platform == 'win32' else 'lib') +
                    'kalandra-native-runtime' + ('-' + name if 'sanitized' in name else '') +
                    ('.lib' if sys.platform == 'win32' else '.a'))
                run([a.clang, '-std=c++23', '-Wno-override-module', '-ffp-contract=off', '-pthread',
                     *flags, *(['-DFLOAT_TAIL'] if floating else []),
                     *(['-fms-runtime-lib=static'] if sys.platform == 'win32' else []),
                     root / 'native/tests/TailCallRuntimeTests.cpp', ir, runtime, '-o', binary])
                r = run([binary])
                outputs.append(r.stdout)
                records.append(dict(variant=label, arithmetic=a.arithmetic, compiler=version,
                    compiler_sha256=frozen_compilers[str(compiler)],
                    cases=len(r.stdout.splitlines()),
                    state_sha256=hashlib.sha256(r.stdout.encode()).hexdigest(),
                    ir_sha256=hashlib.sha256(ir.read_bytes()).hexdigest(), result=r.stderr.strip()))
            assert len(set(outputs)) == 1, f'logical state/poll/value mismatch: {label}'
            print(label + ': ' + records[-1]['result'] +
                  ('; matches baseline exactly' if a.baseline else ''), flush=True)
        for label, (source, expected, eligible) in ELIGIBILITY.items():
            for version, compiler in compilers:
                binary = a.output / (name + '-' + label + '-' + version)
                path = binary.with_suffix('.k')
                ir = binary.with_suffix('.ll')
                path.write_text(source)
                run([compiler, path, '--arithmetic', a.arithmetic, flag,
                     '--emit-llvm', ir, '-o', binary])
                text = ir.read_text()
                if version == 'current':
                    assert ('tail.header' in text) == eligible, label
                result = run([binary]).stdout
                assert result == expected, (label, result)
                records.append(dict(variant=name + '-' + label, arithmetic=a.arithmetic,
                    compiler_sha256=frozen_compilers[str(compiler)],
                    compiler=version, cases=1, state_sha256=hashlib.sha256(result.encode()).hexdigest(),
                    ir_sha256=hashlib.sha256(ir.read_bytes()).hexdigest(), result=result.strip()))
    assert all(hashlib.sha256(Path(c).read_bytes()).hexdigest() == h for c, h in frozen_compilers.items()), 'compiler changed during qualification'
    (a.output / 'qualification.json').write_text(json.dumps(records, indent=2) + '\n')

if __name__ == '__main__':
    main()
