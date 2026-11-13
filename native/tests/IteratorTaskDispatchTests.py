"""Run compiler-emitted next/close against a real coordinating native producer."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--clang', type=Path, required=True)
parser.add_argument('--runtime', type=Path, required=True)
parser.add_argument('--variant', choices=('debug', 'optimized', 'sanitized', 'thread-sanitized'), required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parent
windows = sys.platform == 'win32'
flags = {
    'debug': ['-O0'], 'optimized': ['-O2'],
    'sanitized': ['--sanitize'], 'thread-sanitized': ['--thread-sanitize'],
}[args.variant]
native = {
    'debug': ['-O0'], 'optimized': ['-O2'],
    'sanitized': ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'],
    'thread-sanitized': ['-O1', '-g', '-fsanitize=thread'],
}[args.variant]
environment = dict(os.environ)
if windows:
    resources = list(args.clang.resolve().parent.parent.glob('lib/clang/*/lib/windows'))
    environment['PATH'] = os.pathsep.join(map(str, resources)) + os.pathsep + environment.get('PATH', '')


def run(command, name):
    result = subprocess.run(list(map(str, command)), capture_output=True, timeout=180, env=environment)
    (args.output / (name + '.log')).write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0, (name, result)
    return result.stdout


ir = args.output / 'source.ll'
run([args.compiler, root / 'IteratorTaskDispatch.k', *flags, '--test-entry', 'worker',
     '--emit-llvm', ir, '-o', args.output / ('source.exe' if windows else 'source')], 'compile')
text = ir.read_text()
layouts = set(re.findall(r'call i32 @kalandra_generator_next_task_v1\([^\n]*, ptr(?: nonnull)? @([\w.$]+), ptr', text))
assert len(layouts) == 1, ('missing or ambiguous generated next layout', layouts)
name = layouts.pop()
assert re.search(r'^@' + re.escape(name) + r' = internal ', text, re.MULTILINE)
# Export only the test module's descriptor. Production keeps its private ABI;
# the real compiler-emitted copy/validation callbacks remain unchanged.
text = text.replace('@' + name + ' = internal ', '@' + name + ' = ')
text = re.sub('@' + re.escape(name) + r'\b', '@kalandra_iterator_dispatch_layout', text)
assert '@kalandra_generator_close_task_v1' in text
ir.write_text(text)
binary = args.output / ('dispatch.exe' if windows else 'dispatch')
run([args.clang, '-std=c++23', '-Wall', '-Wextra', '-Werror', '-Wno-override-module',
     *(['-fms-runtime-lib=static'] if windows else ['-pthread']), *native,
     root / 'IteratorTaskDispatchTests.cpp', ir, args.runtime, '-o', binary], 'link')
print(run([binary], 'run').decode(), end='', flush=True)
