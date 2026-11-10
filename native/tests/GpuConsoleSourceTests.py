"""Qualify host console ownership and scheduling around explicit GPU work."""
import argparse
import os
import re
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--target', choices=('metal', 'vulkan', 'cuda'), required=True)
p.add_argument('--variant', choices=('debug', 'optimized', 'sanitized', 'thread-sanitized'), required=True)
p.add_argument('--case', action='append', dest='cases', help='Run only the named fixture; repeat to select several.')
p.add_argument('--compile-only', action='store_true', help='Check application linking without claiming GPU execution.')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
flag = {'debug': '-O0', 'optimized': '-O2', 'sanitized': '--sanitize',
        'thread-sanitized': '--thread-sanitize'}[a.variant]
prelude = '''from "std/io" import println, readLine, Line, IoError
from "std/fmt" import format
fn double(value:i64)->i64:
  return value*2
'''

def application_stderr(data):
    # Metal's enabled validation layer announces itself through NSLog. Keep the
    # layer enabled and reject every other diagnostic, including validation errors.
    if a.target != 'metal':
        return data
    return b''.join(line for line in data.splitlines(keepends=True)
                    if not re.fullmatch(rb'\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ program(?:-link)?\[\d+:\d+\] Metal API Validation Enabled\r?\n?', line))

def run(name, source, data, expected, args=(), status=0):
    if a.cases and name not in a.cases:
        return
    directory = a.output / name
    directory.mkdir(exist_ok=True)
    path = directory / 'main.k'
    path.write_text(prelude + source, encoding='utf-8')
    bundle = directory / 'bundle'
    built = subprocess.run([a.compiler.resolve(), path.resolve(), '--target', a.target,
                            flag, '-o', bundle.resolve()], capture_output=True, timeout=180)
    (directory / 'build.log').write_bytes(built.stdout + built.stderr)
    assert built.returncode == 0, (name, built)
    if a.compile_only:
        print('PASS: compiled', name, flush=True)
        return
    executable = bundle / ('program.exe' if os.name == 'nt' else 'program')
    for workers in (1, 4):
        result = subprocess.run([executable.resolve(), *args], input=data, capture_output=True,
                                timeout=30, env=dict(os.environ, KALANDRA_WORKERS=str(workers)))
        (directory / f'run-{workers}.log').write_bytes(result.stdout + result.stderr)
        assert result.returncode == status and result.stdout == expected and not application_stderr(result.stderr), (name, result)
    if os.name != 'nt':
        # Resource lookup must use the executable, not argv[0] or the cwd.
        launch = directory / 'launch'
        launch.mkdir(exist_ok=True)
        link = launch / 'program-link'
        if not link.exists():
            link.symlink_to(executable.resolve())
        result = subprocess.run(['custom-argv-zero', *args], executable=str(link.absolute()),
                                cwd=launch, input=data, capture_output=True, timeout=30)
        assert result.returncode == status and result.stdout == expected and not application_stderr(result.stderr), (name, result)
    print('PASS:', name, flush=True)

run('streams', '''fn main()->unit ! (IoError | TaskError):
  answer=try parallel:
    gpu=devices.gpu()
    try await spawn double(21) on gpu
  try println(format("answer={}",answer))
  return ()
''', b'', b'answer=42\n')

run('text-tasks', '''fn readOne()->String ! IoError:
  line=try readLine(100)
  return match line:
    Line.End:"<end>"
    Line.Text {text}:text
fn identity(text:String)->String:
  return text
fn main()->unit ! (IoError | TaskError):
  return try parallel:
    gpu=devices.gpu()
    computation=spawn double(21) on gpu
    input=spawn readOne()
    text=try await input
    copied=spawn identity(text)
    answer=try await computation
    try println(format("{}:{}",answer,try await copied))
    ()
''', 'α\0é\n'.encode(), '42:α\0é\n'.encode())

run('line-errors', '''fn next()->String ! IoError:
  line=try readLine(2)
  return match line:
    Line.End:"<end>"
    Line.Text {text}:text
fn recover()->String:
  return next() catch fn(error):
    return match error:
      IoError.System {code,message}:format("system={}:{}",code,message)
      IoError.InvalidEncoding:"encoding"
      IoError.LineTooLong {limit}:format("limit={}",limit)
fn main()->unit ! (IoError | TaskError):
  answer=try parallel:
    gpu=devices.gpu()
    try await spawn double(21) on gpu
  try println(format("{}:{}",answer,recover()))
  try println(recover())
  try println(recover())
  try println(recover())
  return ()
''', b'long\n\xff\nok\n', b'42:limit=2\nencoding\nok\n<end>\n')

run('arguments-and-status', '''from "std/process" import ExitStatus
fn main(args:[String])->ExitStatus ! (IoError | TaskError):
  answer=try parallel:
    gpu=devices.gpu()
    try await spawn double(21) on gpu
  try println(format("{}:{}:{}:{}:{}",answer,length(args),args[0],args[1],args[2]))
  return ExitStatus {code:42}
''', b'', '42:3:é👩‍💻:hello world:\n'.encode(), ('é👩‍💻', 'hello world', ''), 42)

if not a.cases or 'cancel-pending-read' in a.cases:
    directory = a.output / 'cancel-pending-read'
    directory.mkdir(exist_ok=True)
    path = directory / 'main.k'
    path.write_text(prelude + '''fn waiting()->unit ! IoError:
  line=try readLine(100)
  return ()
fn divide(value:i64)->i64:
  return 42/value
fn main()->unit ! (IoError | TaskError):
  return try parallel:
    pending=spawn waiting()
    computation=spawn divide(0) on devices.gpu()
    try await computation
    try await pending
    ()
''', encoding='utf-8')
    bundle = directory / 'bundle'
    built = subprocess.run([a.compiler.resolve(), path.resolve(), '--target', a.target,
                            flag, '-o', bundle.resolve()], capture_output=True, timeout=180)
    (directory / 'build.log').write_bytes(built.stdout + built.stderr)
    assert built.returncode == 0, built
    executable = bundle / ('program.exe' if os.name == 'nt' else 'program')
    for workers in (() if a.compile_only else (1, 4)):
        child = subprocess.Popen([executable.resolve()], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 env=dict(os.environ, KALANDRA_WORKERS=str(workers)))
        try:
            child.wait(timeout=30)  # Keep stdin open: no EOF can unblock the read.
            stdout, stderr = child.stdout.read(), child.stderr.read()
            assert child.returncode == 1 and not stdout and b'1/4' in stderr, (child.returncode, stdout, stderr)
            assert b'Sanitizer' not in stderr and b'runtime error:' not in stderr, stderr
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=10)
            child.stdin.close()
            child.stdout.close()
            child.stderr.close()
    print('PASS: compiled pending-read fault application' if a.compile_only else
          'PASS: GPU arithmetic fault cancels pending console read; no EOF or worker starvation', flush=True)
