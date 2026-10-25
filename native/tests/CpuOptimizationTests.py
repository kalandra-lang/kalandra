"""Qualify optimized CPU loops without timing thresholds or weakened numerical checks."""
import argparse
from pathlib import Path
import re
import sys
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True, type=Path)
parser.add_argument('--clang', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--quick', action='store_true')
parser.add_argument('--matrices-only', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
compiler, clang = args.compiler.resolve(), args.clang.absolute()
args.output.mkdir(parents=True, exist_ok=True)
source = '''fn dot(a:Tensor[NUM,?],b:Tensor[NUM,?])->NUM:
  reduceSum(a*b)
fn axpy(a:Tensor[NUM,?],b:Tensor[NUM,?])->NUM:
  reduceSum(a*3+b)
fn matrix(a:Tensor[NUM,?,?],b:Tensor[NUM,?,?])->NUM:
  reduceSum(matmul(a,b))
fn divide(a:Tensor[NUM,?],b:Tensor[NUM,?])->NUM:
  reduceSum((a+1)/(b+2))
fn matrixCells(a:Tensor[NUM,?,?],b:Tensor[NUM,?,?])->Tensor[NUM,?,?]:
  matmul(a,b)
fn main()->NUM:
  0
'''

def run(argv):
    result = subprocess.run([str(x) for x in argv], capture_output=True, text=True, timeout=240)
    assert result.returncode == 0, (argv, result.returncode, result.stdout, result.stderr)
    return result.stdout

variants = [('optimized', '-O2', ['-O2'])]
if not args.quick:
    variants += [('debug', '-O0', ['-O0']),
                 ('sanitized', '--sanitize', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]
    if sys.platform != 'win32':
        variants += [('thread-sanitized', '--thread-sanitize', ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer'])]
for numeric in ('float32', 'float64'):
    path = args.output / (numeric + '.k')
    path.write_text(source.replace('NUM', numeric))
    for variant, flag, flags in variants:
        for kind, name in enumerate(('dot', 'axpy', 'matrix', 'divide', 'matrixCells')):
            if args.matrices_only and kind not in (2, 4):
                continue
            output = args.output / f'{numeric}-{variant}-{name}'
            ir = output.with_suffix('.ll')
            run([compiler, path, '--test-entry', name, '--emit-llvm', ir, flag, '-o', str(output) + '-standalone'])
            text = ir.read_text()
            assert 'loop.chunks' in text
            assert not re.search(r'\b(fmul|fadd) contract\b', text)
            runtime = compiler.parent / (('' if sys.platform == 'win32' else 'lib') + 'kalandra-native-runtime' +
                ('-' + variant if 'sanitized' in variant else '') + ('.lib' if sys.platform == 'win32' else '.a'))
            run([clang, '-std=c++23', '-ffp-contract=off', '-Wno-override-module', '-pthread',
                 *flags, *(['-fms-runtime-lib=static'] if sys.platform == 'win32' else []), '-DCASE=' + str(kind), *(['-DFLOAT32'] if numeric == 'float32' else []),
                 root / 'native/tests/CpuOptimizationRuntimeTests.cpp', ir, runtime, '-o', output])
            run([output])
            print(f'{numeric} {variant} {name}: passed', flush=True)
print('Optimized CPU matrices: exact tree, per-cell/aggregate outputs, tails, strides, exceptional floats, allocation faults, cancellation and cleanup passed' if args.matrices_only else 'Optimized CPU loops: exact recursive tree reference, dense/positive/negative strides, exceptional floats, zero/odd/chunk tails, per-cell SIMD matrix results, seeded finite mantissas/exponents, rounding/FMA witnesses, isolated exceptional operands, allocation/shape faults and mid-loop cancellation passed')
for numeric in (() if args.matrices_only else ('float32', 'float64')):
    controls = [
        ('rank3', '?,?,?', 'tensor([1,2,3,4,5,6],[2,1,3])', '69'),
        ('empty-rank3', '?,?,?', 'tensor([],[2,0,3])', '0'),
        ('rank8', '?,?,?,?,?,?,?,?', 'tensor([1,2,3],[1,1,1,1,1,1,1,3])', '21'),
    ]
    for name, dims, literal, expected in controls:
        path = args.output / f'{numeric}-{name}.k'
        path.write_text(f'fn f(a:Tensor[{numeric},{dims}])->{numeric}:\n  reduceSum(a*3+1)\nfn main()->{numeric}:\n  f({literal})\n')
        output = path.with_suffix('')
        run([compiler, path, '-O2', '-o', output])
        assert run([output]).strip() == expected
    path = args.output / f'{numeric}-long.k'
    path.write_text(f'fn f(a:Tensor[{numeric},?])->{numeric}:\n  reduceSum(a' + '+1'*40 + f')\nfn main()->{numeric}:\n  f(tensor([1,2,3]))\n')
    run([compiler, path, '-O2', '-o', path.with_suffix('')])
    assert run([path.with_suffix('')]).strip() == '126'
if not args.matrices_only:
    print('Higher-rank fused maps, empty interior dimensions and long expression chains: passed')
