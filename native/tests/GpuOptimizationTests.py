"""Qualify hardware GPU optimization against the independent APFloat oracle."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True, type=Path)
parser.add_argument('--clang', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--quick', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
compiler = args.compiler.resolve()
clang = args.clang.absolute()
llvm = clang.parent.parent
args.output.mkdir(parents=True, exist_ok=True)

def run(command):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=360)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return result.stdout

source = args.output / 'kernels.k'
source.write_text((root / 'native/benchmarks/languages/gpu.k').read_text().replace('float64', 'float32'))
bundle = args.output / 'bundle'
run([compiler, source, '--target', 'metal', '--arithmetic', 'hardware', '-O2', '-o', bundle])
assert 'kalandra_matmul_hardware_tiled' in (bundle / 'kernel2.metal').read_text()
assert 'kalandra_matmul_hardware_block64' in (bundle / 'kernel2.metal').read_text()
assert 'kalandra_reduce_sum_hardware_block' in (bundle / 'kernel0-reduce.metal').read_text()
variants = [('optimized', ['-O2'], 'libkalandra-task-core-optimized.a')]
if not args.quick:
    variants += [('debug', ['-O0'], 'libkalandra-native-runtime.a'),
                 ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'], 'libkalandra-native-runtime-sanitized.a'),
                 ('thread-sanitized', ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer'], 'libkalandra-native-runtime-thread-sanitized.a')]
for variant, flags, runtime in variants:
    binary = args.output / variant
    run([clang, '-std=c++23', '-ffp-contract=off', '-pthread', '-Wall', '-Wextra', '-Werror', *flags,
         '-isystem', llvm / 'include', root / 'native/tests/GpuOptimizationRuntimeTests.cpp',
         compiler.parent / 'metal' / f'libkalandra-metal-{variant}.a', compiler.parent / runtime,
         '-L' + str(llvm / 'lib'), '-lLLVM', '-Wl,-rpath,' + str(llvm / 'lib'),
         '-framework', 'Foundation', '-framework', 'Metal', '-o', binary])
    print(variant + ': ' + run([binary, bundle]).strip(), flush=True)
