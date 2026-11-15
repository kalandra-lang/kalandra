"""Qualify stable comparator sorting, failure atomicity and cancellation."""
import argparse
import json
import subprocess
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--clang', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.compiler = args.compiler.resolve()
args.output.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[2]
source = args.output / 'sort.k'
source.write_text('''from "std/cmp" import compare,sortBy,Ordering
fn comparator(a:i64,b:i64)->Ordering:
  divisor=if a<0:a-a
  else:1000
  return compare(a/divisor,b/1000)
fn run(mut values:[i64])->i64:
  sortBy(values,comparator)
  return length(values)
fn main()->unit:
  return ()
''')
variants = [('debug', '-O0', ['-O0']), ('optimized', '-O2', ['-O2']),
            ('sanitized', '--sanitize', ['-O1', '-g', '-fsanitize=address,undefined',
                                        '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]
if sys.platform != 'win32':
    variants.append(('thread-sanitized', '--thread-sanitize',
                     ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer']))
if sys.platform == 'win32':
    probe = args.output / 'sanitizer-probe.k'
    probe.write_text('fn main()->unit:\n  return ()\n')
    checked = subprocess.run([str(args.compiler), str(probe), '--sanitize', '-o',
                              str(args.output / 'sanitizer-probe.exe')],
                             capture_output=True, text=True, timeout=300)
    if checked.returncode == 2 and 'ASan/UBSan are unavailable in the pinned Windows ARM64 toolchain' in checked.stderr:
        variants = [v for v in variants if v[0] != 'sanitized']
        (args.output / 'unsupported.json').write_text(json.dumps(
            {'sanitized': checked.stderr.strip()}, indent=2) + '\n')
        print('UNSUPPORTED sanitized:', checked.stderr.strip(), flush=True)
    else:
        assert checked.returncode == 0, (checked.returncode, checked.stdout, checked.stderr)
records = []
def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return result
for variant, flag, flags in variants:
    binary = args.output.resolve() / (variant + ('.exe' if sys.platform == 'win32' else ''))
    ir = binary.with_suffix('.ll')
    run([args.compiler, source, '--test-entry', 'run', flag, '--emit-llvm', ir,
         '-o', str(binary) + '-standalone'])
    runtime = args.compiler.parent / (('' if sys.platform == 'win32' else 'lib') +
        'kalandra-native-runtime' + ('-' + variant if 'sanitized' in variant else '') +
        ('.lib' if sys.platform == 'win32' else '.a'))
    run([args.clang, '-std=c++23', '-Wno-override-module', '-pthread', *flags,
         *(['-fms-runtime-lib=static'] if sys.platform == 'win32' else []),
         repo / 'native/tests/OrderingSortRuntimeTests.cpp', ir, runtime, '-o', binary])
    result = run([binary])
    assert not result.stderr, result.stderr
    records.append(dict(variant=variant, result=result.stdout.strip()))
    (args.output / 'results.json').write_text(json.dumps(records, indent=2) + '\n')
    print(variant, result.stdout.strip(), flush=True)
