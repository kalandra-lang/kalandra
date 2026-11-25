"""Check custom copying sorts with raw-bit, strided and fault/cancel references."""
from OrdinaryTestEnvironment import configure, qualified_variants
import argparse
import json
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--clang', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.compiler = args.compiler.resolve()
configure(args.clang)
args.output.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[2]
variants = [('debug', '-O0', ['-O0']), ('optimized', '-O2', ['-O2']),
            ('sanitized', '--sanitize', ['-O1', '-g', '-fsanitize=address,undefined',
                                        '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]
if sys.platform != 'win32':
    variants.append(('thread-sanitized', '--thread-sanitize',
                     ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer']))
variants = qualified_variants(variants, args.compiler)
records = []

def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return result

for numeric_id, numeric in enumerate(('i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64')):
    source = args.output / (numeric + '.k')
    comparison = 'totalCompare' if numeric.startswith('f') else 'compare'
    source.write_text(f'''from "std/cmp" import sortedBy,compare,totalCompare,Ordering
fn comparator(a:{numeric},b:{numeric})->Ordering:
  return {comparison}(a,b)
fn ordered(values:[{numeric}])->[{numeric}]:
  return sortedBy(values,comparator)
fn main()->unit:
  return ()
''')
    for variant, flag, flags in variants:
        binary = args.output.resolve() / (numeric + '-' + variant + ('.exe' if sys.platform == 'win32' else ''))
        ir = binary.with_suffix('.ll')
        run([args.compiler, source, '--test-entry', 'ordered', flag, '--emit-llvm', ir,
             '--no-link'])
        runtime = args.compiler.parent / (('' if sys.platform == 'win32' else 'lib') +
            'kalandra-native-runtime' + ('-' + variant if 'sanitized' in variant else '') +
            ('.lib' if sys.platform == 'win32' else '.a'))
        run([args.clang, '-std=c++23', '-Wno-override-module', '-pthread', *flags,
             *(['-fms-runtime-lib=static'] if sys.platform == 'win32' else []),
             '-DSORT_COPY', '-DNUMERIC_TYPE=' + str(numeric_id),
             repo / 'native/tests/ArraySortRuntimeTests.cpp', ir, runtime, '-o', binary])
        result = run([binary])
        assert not result.stderr, result.stderr
        records.append(dict(numeric=numeric, variant=variant, result=result.stdout.strip()))
        (args.output / 'results.json').write_text(json.dumps(records, indent=2) + '\n')
        print(numeric, variant, result.stdout.strip(), flush=True)
