"""Hardware graph batching: APFloat, acknowledgement ownership and source execution."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True, type=Path)
parser.add_argument('--clang', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
compiler, clang = args.compiler.resolve(), args.clang.absolute()
llvm = clang.parent.parent
args.output.mkdir(parents=True, exist_ok=True)

def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    assert not any(x in result.stderr for x in ('AddressSanitizer', 'ThreadSanitizer', 'runtime error:')), result.stderr
    return result.stdout

bundle = args.output / 'bundle'
source = root / 'native/benchmarks/GraphBatch.k'
run([compiler, source, '--target', 'metal', '--arithmetic', 'hardware', '-O2', '-o', bundle])
assert 'matrix_graph_stages=4' in (bundle / 'manifest.txt').read_text()
assert 'kalandra_matmul_hardware_tiled' in (bundle / 'kernel0.metal').read_text()
assert 'kalandra_matmul_fused_tiled' in (bundle / 'kernel0-map1.metal').read_text()
scalar_source = args.output / 'scalar.k'
scalar_source.write_text('''fn scaled(a:Tensor[float32,?,?],b:Tensor[float32,?,?],scale:float32)->Tensor[float32,?,?]:
  first=matmul(a,b)*scale
  matmul(first,b)*scale
fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    x:Tensor[float32,2,2]=tensor([[1,0],[0,1]])
    gpu=devices.gpu()
    a=try await transfer(x,gpu)
    value=try await spawn scaled(a,a,0.5) on gpu
    try await transfer(value,devices.cpu())
''')
scalar = args.output / 'scalar'
run([compiler, scalar_source, '--target', 'metal', '--arithmetic', 'hardware', '-O2', '-o', scalar])
assert 'kalandra_matmul_fused_tiled' in (scalar / 'kernel0-map1.metal').read_text()
fallback_source = args.output / 'fallback.k'
fallback_source.write_text('''fn activate(a:Tensor[float32,?,?])->Tensor[float32,?,?]:
  relu(a)
fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    x:Tensor[float32,2,2]=tensor([[1,0],[0,1]])
    gpu=devices.gpu()
    a=try await transfer(x,gpu)
    value=try await spawn activate(a) on gpu
    try await transfer(value,devices.cpu())
''')
fallback = args.output / 'fallback'
run([compiler, fallback_source, '--target', 'metal', '--arithmetic', 'hardware', '-O2', '-o', fallback])
assert 'kalandra_matmul_fused_' not in (fallback / 'kernel0.metal').read_text()
indirect_source = args.output / 'indirect.k'
tensor = 'Tensor[float32,?,?]'
parameters = [f'c{i}:'+tensor for i in range(31)]
indirect_source.write_text('fn combine('+','.join(parameters)+')->'+tensor+':\n  '+
    '+'.join(f'c{i}' for i in range(31))+'\nfn main()->'+tensor+' ! TaskError:\n'
    '  try parallel:\n    x:Tensor[float32,2,2]=tensor([[1,1],[1,1]])\n'
    '    gpu=devices.gpu()\n    a=try await transfer(x,gpu)\n'
    '    value=try await spawn combine('+','.join(['a']*31)+') on gpu\n'
    '    try await transfer(value,devices.cpu())\n')
indirect = args.output / 'indirect'
run([compiler, indirect_source, '--target', 'metal', '--arithmetic', 'hardware', '-O2', '-o', indirect])
assert 'ArgumentBuffer' in (indirect / 'host.cpp').read_text()
variants = [('optimized', ['-O2'], 'libkalandra-task-core-optimized.a'),
            ('debug', ['-O0'], 'libkalandra-native-runtime.a'),
            ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'], 'libkalandra-native-runtime-sanitized.a'),
            ('thread-sanitized', ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer'], 'libkalandra-native-runtime-thread-sanitized.a')]
for variant, flags, runtime in variants:
    binary = args.output / variant
    run([clang, '-std=c++23', '-ffp-contract=off', '-pthread', '-Wall', '-Wextra', '-Werror', *flags,
         '-isystem', llvm / 'include', root / 'native/tests/GraphBatchRuntimeTests.cpp',
         compiler.parent / 'metal' / f'libkalandra-metal-{variant}.a', compiler.parent / runtime,
         '-L' + str(llvm / 'lib'), '-lLLVM', '-Wl,-rpath,' + str(llvm / 'lib'),
         '-framework', 'Foundation', '-framework', 'Metal', '-o', binary])
    print(variant + ': ' + run([binary, bundle, indirect, scalar, fallback]).strip(), flush=True)
    target = args.output / ('source-' + variant)
    flag = dict(optimized='-O2', debug='-O0', sanitized='--sanitize', **{'thread-sanitized':'--thread-sanitize'})[variant]
    run([compiler, source, '--target', 'metal', '--arithmetic', 'hardware', flag, '-o', target])
    assert int(run([target / 'program', target]).strip(), 16) == 0x43470000, variant
    print(variant + ': compiled source execution passed', flush=True)
