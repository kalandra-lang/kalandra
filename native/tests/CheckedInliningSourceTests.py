"""Floating recursive calls preserve values, conversion faults and observed state."""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

SOURCE = '''fn tree(value:float64,n:int)->float64:
  if n == 0:value
  else:tree(value*0.5,n-1)+tree(value*(-0.25),n-1)
fn divide(value:float64,n:int,d:int)->float64:
  if n == 0:float64(int64(value)/d)
  else:divide(value,n-1,d)+0.0
fn rounded(value:float64,n:int)->float64:
  if n == 0:value*value-1
  else:rounded(value*0.5,n-1)+rounded(value*(-0.25),n-1)
fn run(value:float64,n:int,kind:int,d:int)->float64:
  if kind == 0:tree(value,n)
  else:
    if kind == 1:divide(value,n,d)
    else:rounded(value,n)
fn main()->float64:
  run(1.3,5,0,1)
'''

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--compiler', type=Path, required=True)
    p.add_argument('--baseline', type=Path)
    p.add_argument('--clang', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--arithmetic', choices=['hardware', 'software'], default='hardware')
    p.add_argument('--numeric', choices=['float32', 'float64'], default='float64')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    path = a.output / 'source.k'
    path.write_text(SOURCE.replace('float64', a.numeric))
    def run(args):
        r = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=240)
        assert r.returncode == 0, (args, r.returncode, r.stdout[-1000:], r.stderr[-3000:])
        return r
    variants = [('debug', '-O0', ['-O0']), ('optimized', '-O2', ['-O2']),
                ('sanitized', '--sanitize', ['-O1', '-g', '-fsanitize=address,undefined',
                                           '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]
    if sys.platform != 'win32':
        variants.append(('thread-sanitized', '--thread-sanitize',
                         ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer']))
    compilers = [('current', a.compiler)]
    if a.baseline:
        compilers.insert(0, ('baseline', a.baseline))
    frozen_compilers = {str(c): hashlib.sha256(c.read_bytes()).hexdigest() for _, c in compilers}
    records = []
    for name, flag, flags in variants:
        outputs = []
        for version, compiler in compilers:
            binary = a.output / (name + '-' + version)
            ir = binary.with_suffix('.ll')
            run([compiler, path, '--test-entry', 'run', '--arithmetic', a.arithmetic,
                 '--emit-llvm', ir, flag, '-o', str(binary) + '-standalone'])
            text = ir.read_text()
            assert not re.search(r'\b(fadd|fsub|fmul|fdiv)\s+(contract|fast|reassoc)\b', text)
            assert '@llvm.fma.' not in text and '@llvm.fmuladd.' not in text
            runtime = a.compiler.parent / (('' if sys.platform == 'win32' else 'lib') +
                'kalandra-native-runtime' + ('-' + name if 'sanitized' in name else '') +
                ('.lib' if sys.platform == 'win32' else '.a'))
            run([a.clang, '-std=c++23', '-Wno-override-module', '-ffp-contract=off', '-pthread',
                 *flags, *(['-DFLOAT32'] if a.numeric == 'float32' else []),
                 *(['-fms-runtime-lib=static'] if sys.platform == 'win32' else []),
                 root / 'native/tests/CheckedInliningRuntimeTests.cpp', ir, runtime, '-o', binary])
            r = run([binary])
            outputs.append(r.stdout)
            records.append(dict(variant=name, arithmetic=a.arithmetic, numeric=a.numeric, compiler=version,
                compiler_sha256=frozen_compilers[str(compiler)],
                cases=len(r.stdout.splitlines()), state_sha256=hashlib.sha256(r.stdout.encode()).hexdigest(),
                ir_sha256=hashlib.sha256(ir.read_bytes()).hexdigest(), result=r.stderr.strip()))
        assert len(set(outputs)) == 1, f'value/fault/location/poll mismatch: {name}'
        print(name + ': ' + records[-1]['result'] +
              ('; matches baseline exactly' if a.baseline else ''), flush=True)
    assert all(hashlib.sha256(Path(c).read_bytes()).hexdigest() == h for c, h in frozen_compilers.items()), 'compiler changed during qualification'
    (a.output / 'qualification.json').write_text(json.dumps(records, indent=2) + '\n')

if __name__ == '__main__':
    main()
