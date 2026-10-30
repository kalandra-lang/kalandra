"""Exhaustive small-domain and full-width signed remainder differential checks."""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--baseline', type=Path)
p.add_argument('--clang', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
source = a.output / 'remainder.k'
source.write_text('fn run(a:int,b:int)->int:\n  a%b\nfn main()->int:\n  run(37,7)\n')
variants = [('debug', '-O0', ['-O0']), ('optimized', '-O2', ['-O2']),
            ('sanitized', '--sanitize', ['-O1', '-g', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]
if sys.platform != 'win32':
    variants.append(('thread-sanitized', '--thread-sanitize',
                     ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer']))
compilers = [('current', a.compiler)]
if a.baseline:
    compilers.insert(0, ('baseline', a.baseline))
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
frozen = {str(c): sha(c) for _, c in compilers}
records = []
def run(command):
    r = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=240)
    assert r.returncode == 0, (command, r.returncode, r.stdout[-2000:], r.stderr[-3000:])
    return r.stdout
for name, flag, flags in variants:
    outputs = []
    for version, compiler in compilers:
        binary = a.output / (name + '-' + version)
        ir = binary.with_suffix('.ll')
        run([compiler, source, '--test-entry', 'run', '--emit-llvm', ir, flag,
             '-o', str(binary) + '-standalone'])
        runtime = a.compiler.parent / (('' if sys.platform == 'win32' else 'lib') +
            'kalandra-native-runtime' + ('-' + name if 'sanitized' in name else '') +
            ('.lib' if sys.platform == 'win32' else '.a'))
        run([a.clang, '-std=c++23', '-Wno-override-module', '-pthread', *flags,
             *(['-fms-runtime-lib=static'] if sys.platform == 'win32' else []),
             root / 'native/tests/SignedRemainderRuntimeTests.cpp', ir, runtime, '-o', binary])
        output = run([binary])
        outputs.append(output)
        records.append(dict(variant=name, compiler=version, compiler_sha256=frozen[str(compiler)],
                            cases=int(output.split()[0]), state_sha256=hashlib.sha256(output.encode()).hexdigest(),
                            ir_sha256=sha(ir), result=output.strip()))
    assert len(set(outputs)) == 1, (name, outputs)
    print(name + ': ' + outputs[-1].strip(), flush=True)
assert all(sha(Path(c)) == digest for c, digest in frozen.items()), 'compiler changed during qualification'
(a.output / 'qualification.json').write_text(json.dumps(records, indent=2) + '\n')
