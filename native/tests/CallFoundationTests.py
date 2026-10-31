"""Deep execution and resource safety for the internal call convention."""
from OrdinaryTestEnvironment import qualified_variants
import argparse
import hashlib
import json
import os
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
source = a.output / 'calls.k'
source.write_text((root / 'native/tests/fixtures/call-foundation.k').read_text())
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
compiler_sha = sha(a.compiler)
variants = [('debug', '-O0', ['-O0']), ('optimized', '-O2', ['-O2']),
            ('sanitized', '--sanitize', ['-O1', '-g', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]
variants = qualified_variants(variants, a.compiler)
if sys.platform != 'win32':
    variants.append(('thread-sanitized', '--thread-sanitize',
                     ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer']))
env = dict(os.environ)
if sys.platform == 'win32':
    resources = list(a.clang.resolve().parent.parent.glob('lib/clang/*/lib/windows'))
    env['PATH'] = os.pathsep.join(map(str, resources)) + os.pathsep + env.get('PATH', '')


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                            timeout=240, env=env)
    assert result.returncode == 0, (command, result.returncode, result.stdout[-2000:],
                                    result.stderr[-4000:])
    return result.stdout


records = []
for name, flag, flags in variants:
    binary = a.output / (name + ('.exe' if sys.platform == 'win32' else ''))
    ir = binary.with_suffix('.ll')
    standalone = binary.with_name(binary.stem + '-standalone' + binary.suffix)
    run([a.compiler, source, '--test-entry', 'run', '--emit-llvm', ir, flag, '-o', standalone])
    text = ir.read_text()
    assert 'kalandra_recursive_enter_' not in text and 'tail.unwind' not in text
    runtime = a.compiler.parent / (('' if sys.platform == 'win32' else 'lib') +
        'kalandra-native-runtime' + ('-' + name if 'sanitized' in name else '') +
        ('.lib' if sys.platform == 'win32' else '.a'))
    debug_flags = ['-g', '-Wl,/debug'] if sys.platform == 'win32' else []
    run([a.clang, '-std=c++23', *debug_flags, '-Wno-override-module', '-ffp-contract=off', '-pthread', *flags,
         *(['-fms-runtime-lib=static'] if sys.platform == 'win32' else []),
         root / 'native/tests/CallFoundationRuntimeTests.cpp', ir, runtime, '-o', binary])
    result = run([binary])
    assert run([standalone]) == '1000000\n'
    terminal = a.output / ('terminal-' + name + '.k')
    terminal.write_text('fn terminal(value:int)->int:\n  1/(value-value)\nfn main()->int:\n  terminal(0)\n')
    terminal_binary = binary.with_name('terminal-' + binary.name)
    run([a.compiler, terminal, flag, '-o', terminal_binary])
    stopped = subprocess.run([str(terminal_binary)], capture_output=True, text=True,
                             timeout=30, env=env)
    assert stopped.returncode == 86 and 'ArithmeticFault' in stopped.stderr and not stopped.stdout, (
        name, stopped.returncode, stopped.stdout, stopped.stderr)
    records.append(dict(variant=name, compiler_sha256=compiler_sha, ir_sha256=sha(ir),
                        binary_sha256=sha(binary), runtime_sha256=sha(runtime), result=result.strip()))
    print(name + ': ' + result.strip(), flush=True)
assert sha(a.compiler) == compiler_sha, 'compiler changed during qualification'
(a.output / 'qualification.json').write_text(json.dumps(records, indent=2) + '\n')
