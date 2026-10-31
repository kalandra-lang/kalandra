"""Public compiler policy regression: hardware defaults and explicit software fallback."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--gpu', action='store_true')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
compiler = args.compiler.resolve()


def command(argv, ok=True):
    result = subprocess.run([str(x) for x in argv], capture_output=True, text=True, timeout=180)
    assert (result.returncode == 0) == ok, (argv, result.returncode, result.stdout, result.stderr)
    return result


def binary(name):
    return args.output / (name + ('.exe' if sys.platform == 'win32' else ''))


def compile_source(name, text, *flags, ok=True):
    source, output = args.output / f'{name}.k', binary(name)
    source.write_text(text)
    command([compiler, source, '-o', output, *flags], ok=ok)
    return output


def cpu(name, text, flags=(), expected=None):
    output = binary(name)
    compile_source(name, text, '--emit-llvm', output.with_suffix('.ll'), *flags)
    if expected is not None:
        assert command([output]).stdout.strip() == expected, name
    return output.with_suffix('.ll').read_text()


math_source = 'fn calculate(a:float64,b:float64)->float64:\n  (a+b)*(a-b)/b\nfn main()->float64:\n  calculate(8,2)\n'
worker = 'fn calculate(a:float64,b:float64)->float64:\n  (a+b)*(a-b)/b\nfn main()->float64 ! TaskError:\n  try parallel:\n    try await spawn calculate(8,2)\n'
for opt in ('-O0', '-O2'):
    for task, source in ((False, math_source), (True, worker)):
        for mode in ('hardware', 'software', 'hardware-with-software-fallback'):
            name = f'cpu-{opt}-{task}-{mode}'
            ir = cpu(name, source, (opt, '--arithmetic', mode), '30')
            floating = re.search(r'\b(fadd|fsub|fmul|fdiv) double\b', ir)
            assert bool(floating) == (mode != 'software'), name
            if mode == 'software':
                assert 'i128' in ir, name
# Default must be hardware, even without an explicit flag.
assert 'fmul double' in cpu('default', math_source, expected='30')
conversion = 'fn convert(a:float64)->int32:\n  int32(a)\nfn main()->int32:\n  convert(2.75)\n'
assert 'fptosi' in cpu('cast-hardware', conversion, expected='2')
assert 'fptosi' not in cpu('cast-software', conversion, ('--arithmetic', 'software'), '2')
assert 'sitofp' in cpu('int-to-double', 'fn convert(a:int64)->float64:\n  float64(a)\nfn main()->float64:\n  convert(9007199254740993)\n', expected='9007199254740992')
assert 'fptrunc' in cpu('double-to-single', 'fn convert(a:float64)->float32:\n  float32(a)\nfn main()->float32:\n  convert(2.75)\n', expected='2.75')
comparison = 'fn compare(a:float64,b:float64)->Bool:\n  a<b\nfn main()->Bool:\n  compare(1,2)\n'
assert 'fcmp' in cpu('compare-hardware', comparison, expected='true')
assert 'fcmp' not in cpu('compare-software', comparison, ('--arithmetic', 'software'), 'true')
for name, expression in [('nan', 'int32(0.0/0.0)'), ('infinity', 'int32(1.0/0.0)'), ('overflow', 'float32(1.0e100)')]:
    result_type = 'float32' if name == 'overflow' else 'int32'
    output = compile_source('fault-' + name, f'fn convert(x:float64)->{result_type}:\n  {result_type}(x)\nfn main()->{result_type}:\n  convert(' + ({'nan':'0.0/0.0','infinity':'1.0/0.0','overflow':'1.0e100'}[name]) + ')\n', '-O2')
    result = command([output], ok=False)
    assert result.returncode > 0 and 'Arithmetic' in result.stderr, (name, result.stderr)
# Bad/missing/duplicate policy flags must fail without replacing an existing output.
for flags in [('--arithmetic', 'unknown'), ('--arithmetic',), ('--arithmetic', 'hardware', '--arithmetic', 'software'),
              ('--matmul-policy', 'unknown'), ('--matmul-policy',),
              ('--matmul-policy', 'optimized', '--matmul-policy', 'fixed-tree')]:
    output = binary('invalid-policy')
    output.write_text('sentinel')
    compile_source('invalid-policy', math_source, *flags, ok=False)
    assert output.read_text() == 'sentinel'
matrix = ('fn main()->float32:\n'
          '  a:Tensor[float32,2,3]=tensor([[1,2,3],[4,5,6]])\n'
          '  b:Tensor[float32,3,2]=tensor([[1,2],[3,4],[5,6]])\n'
          '  reduceSum(matmul(a,b))\n')
for policy in ('optimized', 'fixed-tree'):
    cpu('cpu-matmul-' + policy, matrix, ('-O2', '--matmul-policy', policy), '163')
print('CPU hardware/software arithmetic, workers, comparisons, checked casts, invalid flags: passed', flush=True)

if args.gpu:
    def gpu_source(element):
        return (f'fn work(a:Tensor[{element},?])->{element}:\n  reduceSum(((a*3+1)-a)/2)\n'
                f'fn main()->{element} ! TaskError:\n  try parallel:\n    gpu=devices.gpu()\n'
                f'    values:Tensor[{element},3]=tensor([1,2,3])\n'
                '    resident=try await transfer(values,gpu)\n'
                '    try await spawn work(resident) on gpu\n')
    for mode in ('hardware', 'software', 'hardware-with-software-fallback'):
        output = compile_source('metal32-' + mode, gpu_source('float32'), '--target', 'metal', '-O2', '--arithmetic', mode)
        assert command([output / 'program']).stdout.strip() == struct.pack('>f', 7.5).hex()
        ir = (output / 'kernel0.lowered.mlir').read_text()
        assert bool(re.search(r'arith\.(addf|mulf)', ir)) == (mode != 'software'), mode
        assert f'arithmetic={mode}' in (output / 'manifest.txt').read_text()
    output = args.output / 'metal64-hardware'
    output.mkdir(exist_ok=True)
    (output / 'sentinel').write_text('preserved')
    before = {p.name: p.read_bytes() for p in output.iterdir() if p.is_file()}
    compile_source('metal64-hardware', gpu_source('float64'), '--target', 'metal', '-O2', ok=False)
    assert {p.name: p.read_bytes() for p in output.iterdir() if p.is_file()} == before
    for mode in ('software', 'hardware-with-software-fallback'):
        output = compile_source('metal64-' + mode, gpu_source('float64'), '--target', 'metal', '-O2', '--arithmetic', mode)
        assert command([output / 'program']).stdout.strip() == struct.pack('>d', 7.5).hex()
        assert not re.search(r'arith\.(addf|mulf)', (output / 'kernel0.lowered.mlir').read_text())
    scalar_cast = 'fn work(x:float32)->int32:\n  int32(x)\nfn main()->int32 ! TaskError:\n  try parallel:\n    try await spawn work(2.75) on devices.gpu()\n'
    output = compile_source('metal-cast', scalar_cast, '--target', 'metal', '-O2')
    assert command([output / 'program']).stdout.strip() == '00000002'
    assert 'arith.fptosi' in (output / 'kernel0.lowered.mlir').read_text()
    # Every integer width must use real Metal floating conversions in both directions.
    for numeric in ('int8', 'uint8', 'int16', 'uint16', 'int32', 'uint32', 'int64', 'uint64'):
        value = '2.75' if numeric.startswith('u') else '-2.75'
        cast = (f'fn work(x:float32)->float32:\n  float32({numeric}(x))\n'
                f'fn main()->float32 ! TaskError:\n  try parallel:\n    try await spawn work({value}) on devices.gpu()\n')
        output = compile_source('metal-cast-' + numeric, cast, '--target', 'metal', '-O2')
        expected = 2 if numeric.startswith('u') else -2
        assert command([output / 'program']).stdout.strip() == struct.pack('>f', expected).hex(), numeric
        ir = (output / 'kernel0.lowered.mlir').read_text()
        assert ('arith.fptoui' if numeric.startswith('u') else 'arith.fptosi') in ir, numeric
        assert ('arith.uitofp' if numeric.startswith('u') else 'arith.sitofp') in ir, numeric
    for numeric, value in (('int8', '128'), ('uint8', '-1')):
        cast = (f'fn work(x:float32)->{numeric}:\n  {numeric}(x)\n'
                f'fn main()->{numeric} ! TaskError:\n  try parallel:\n    try await spawn work({value}) on devices.gpu()\n')
        output = compile_source('metal-cast-fault-' + numeric, cast, '--target', 'metal', '-O2')
        fault = command([output / 'program'], ok=False)
        assert fault.returncode == 1 and 'Metal task failed' in fault.stderr, (numeric, fault.stderr)
    print('Metal hardware float32, explicit float64 fallback, software mode and hardware casts: passed', flush=True)

# Scalar/tensor float32 software mode must not leave vector hardware arithmetic behind.
vector_source = ('fn multiply(a:Tensor[float32,?,?],b:Tensor[float32,?,?])->Tensor[float32,?,?]:\n'
                 '  matmul(a,b)\nfn main()->float32:\n  0\n')
ir = cpu('software-vector', vector_source, ('--arithmetic', 'software', '--test-entry', 'multiply'))
assert not re.search(r'\b(fadd|fsub|fmul|fdiv)\b', ir)
if args.gpu:
    comparison = ('fn work(x:float32,y:float32)->float32:\n  if x<y:\n    1\n  else:\n    0\n'
                  'fn main()->float32 ! TaskError:\n  try parallel:\n    try await spawn work(1,2) on devices.gpu()\n')
    output = compile_source('metal-compare', comparison, '--target', 'metal', '-O2')
    assert command([output / 'program']).stdout.strip() == '3f800000'
    assert any('arith.cmpf' in p.read_text() for p in output.glob('kernel*.lowered.mlir'))
    mixed = ('fn narrow(a:Tensor[float32,?])->float32:\n  reduceSum(a*2)\n'
             'fn wide(a:Tensor[float64,?])->float64:\n  reduceSum(a*2)\n'
             'fn cpu(a:float64)->float64:\n  a*2\n'
             'fn main()->float64 ! TaskError:\n  try parallel:\n    gpu=devices.gpu()\n'
             '    a:Tensor[float32,1]=tensor([1])\n    b:Tensor[float64,1]=tensor([2])\n'
             '    x=try await transfer(a,gpu)\n    y=try await transfer(b,gpu)\n'
             '    p=try await spawn narrow(x) on gpu\n    q=try await spawn wide(y) on gpu\n'
             '    cpu(q)+float64(p)\n')
    output = compile_source('metal-mixed-fallback', mixed, '--target', 'metal', '-O2', '--arithmetic', 'hardware-with-software-fallback')
    assert command([output / 'program']).stdout.strip() == struct.pack('>d', 10).hex()
    assert 'arith.mulf' in (output / 'kernel0.lowered.mlir').read_text()
    assert 'arith.mulf' not in (output / 'kernel1.lowered.mlir').read_text()
    assert 'fmul double' in (output / 'cpu.ll').read_text()
    print('Mixed policy keeps CPU float64 and Metal float32 native; only Metal float64 falls back: passed')
