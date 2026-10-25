"""Qualify the real compiler/scheduler path, without the experimental IR rewriter."""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--clang', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
source = a.output / 'source.k'
source.write_text((root / 'native/experiments/cancellation/workloads.k').read_text().replace(
    'fn main()->float64:\n  tail(17,100)', '''fn run(kind:int,n:int)->float64:
  if kind==0:tail(17,n)
  else:
    if kind==1:mutual(17,n)
    else:owned(17,n)
fn main()->float64:
  run(0,100)'''))
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
compiler_sha = sha(a.compiler)
variants = [('debug', '-O0', ['-O0']), ('optimized', '-O2', ['-O2']),
            ('sanitized', '--sanitize', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'])]
if sys.platform != 'win32':
    variants.append(('thread-sanitized', '--thread-sanitize', ['-O1', '-g', '-fsanitize=thread']))


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True,
                            text=True, timeout=180)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    assert not result.stderr, (command, result.stderr)
    return result.stdout


records = []
for name, flag, flags in variants:
    binary = a.output / name
    ir = binary.with_suffix('.ll')
    run([a.compiler, source, '--test-entry', 'run', '--emit-llvm', ir, flag, '-o', str(binary) + '-standalone'])
    text = ir.read_text()
    assert 'kalandra_source_run_v4' in text
    if sys.platform == 'darwin':
        assert '__TEXT,__kalandra,regular,pure_instructions' in text and '+reserve-x28' in text
        assert re.search(r'i(?:8|64) 3, label %', text)
    runtime = a.compiler.parent / (('' if sys.platform == 'win32' else 'lib') +
        'kalandra-native-runtime' + ('-' + name if 'sanitized' in name else '') +
        ('.lib' if sys.platform == 'win32' else '.a'))
    # Linkers may emit their own diagnostics; the executable itself must not.
    subprocess.run(list(map(str, [a.clang, '-std=c++23', '-Wno-override-module', '-pthread',
        *flags, *(['-fms-runtime-lib=static'] if sys.platform == 'win32' else []),
        root / 'native/tests/NativeCancellationSource.cpp', ir, runtime, '-o', binary])),
        check=True, capture_output=True, text=True, timeout=180)
    result = run([binary])
    records.append(dict(variant=name, result=result.strip(), compiler_sha256=compiler_sha,
                        ir_sha256=sha(ir), binary_sha256=sha(binary), runtime_sha256=sha(runtime)))
    print(name + ': ' + result.strip(), flush=True)
assert sha(a.compiler) == compiler_sha
(a.output / 'qualification.json').write_text(json.dumps(records, indent=2) + '\n')
