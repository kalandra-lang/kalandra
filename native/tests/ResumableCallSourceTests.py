"""Ordinary suspending calls use bounded-memory frames, not scheduler admission."""
import argparse
import os
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--variant', choices=('debug', 'optimized', 'sanitized', 'thread-sanitized'), required=True)
p.add_argument('--target', choices=('cpu', 'metal', 'vulkan', 'cuda'), default='cpu')
p.add_argument('--compile-only', action='store_true')
p.add_argument('--case', action='append', choices=(
    'deep-and-mutual', 'tail-retirement', 'owned-error', 'owned-file', 'repeated-results',
    'cancel-deep-sleeper', 'returned-callback', 'recursive-callback', 'borrowed-file',
    'mixed-callback-targets', 'fault-propagation', 'task-fault-catch',
    'tensor-results', 'tensor-capture', 'returned-task-error', 'mutable-array-call'))
a = p.parse_args()
a.compiler = a.compiler.resolve()
a.output = a.output.resolve()
flag = {'debug': '-O0', 'optimized': '-O2', 'sanitized': '--sanitize',
        'thread-sanitized': '--thread-sanitize'}[a.variant]
prelude = '''from "std/time" import sleep, nanoseconds, milliseconds, TimeError
from "std/io" import println, IoError
from "std/fmt" import format
'''

def run(name, source, expected, arguments=(), fault=False):
    if a.case and name not in a.case:
        return
    directory = a.output / name
    directory.mkdir(parents=True, exist_ok=True)
    if a.target != 'cpu':
        source = '''fn twice(n:i64)->i64:
  return n*2
''' + source
        source = re.sub(r'(fn main\([^\n]+\n)', r'\1  checked=try parallel:\n'
                        r'    gpu=devices.gpu()\n    try await spawn twice(21) on gpu\n'
                        r'  try println(format("{}",checked))\n', source)
        expected = '42\n' + expected
    path = directory / 'main.k'
    path.write_text(prelude + source, encoding="utf-8")
    binary = directory / ('program.exe' if os.name == 'nt' else 'program')
    destination = binary if a.target == 'cpu' else directory / 'app'
    command = [a.compiler, path, flag, '-o', destination]
    if a.target != 'cpu':
        command.extend(('--target', a.target))
    result = subprocess.run(command, capture_output=True, timeout=240)
    (directory / 'build.log').write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0, (name, result.stderr.decode(errors='replace'))
    if a.compile_only:
        return
    if a.target != 'cpu':
        binary = destination / binary.name
    for workers in ('1', '4'):
        result = subprocess.run([binary, *arguments], cwd=directory, capture_output=True,
                                env=dict(os.environ, KALANDRA_WORKERS=workers), timeout=90)
        (directory / f'run-{workers}.log').write_bytes(result.stdout + result.stderr)
        stderr = result.stderr
        if a.target == 'metal':
            stderr = b''.join(line for line in stderr.splitlines(keepends=True)
                if not re.fullmatch(rb'\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ '
                                    rb'program\[\d+:\d+\] Metal API Validation Enabled\r?\n?', line))
        if fault:
            diagnostic = (rb'ArithmeticFault at source [0-9]+:[0-9]+\r?\n' if a.target == 'cpu'
                          else (dict(metal='Metal', vulkan='Vulkan', cuda='CUDA')[a.target] +
                                r' task failed: 1/4\r?\n').encode())
            assert result.returncode == 1 and result.stdout == expected.encode() and re.fullmatch(diagnostic, stderr), (
                name, workers, result.returncode, result.stdout, stderr)
        else:
            assert result.returncode == 0 and result.stdout == expected.encode() and not stderr, (
                name, workers, result.returncode, result.stdout, stderr)
    print('PASS:', name, flush=True)

run('deep-and-mutual', '''fn descend(n:i64)->i64 ! TimeError:
  if n==0:
    try sleep(try milliseconds(u64(1)))
    return 17
  return 1 + try descend(n-1)
fn even(n:i64)->bool ! TimeError:
  if n==0:
    try sleep(try milliseconds(u64(1)))
    return true
  return try odd(n-1,false)
fn odd(n:i64,unused:bool)->bool ! TimeError:
  if n==0:
    try sleep(try milliseconds(u64(1)))
    return false
  return try even(n-1)
fn main()->unit ! (TimeError | IoError | TaskError):
  try println(format("{}",try descend(20000)))
  try println(format("{}",try even(10000)))
  return ()
''', '20017\ntrue\n')

run('tail-retirement', '''fn descend(n:i64)->i64 ! TimeError:
  if n==0:
    try sleep(nanoseconds(u64(0)))
    return 42
  return try descend(n-1)
fn main()->unit ! (TimeError | IoError | TaskError):
  try println(format("{}",try descend(100000)))
  return ()
''', '42\n')

run('owned-error', '''error E:
  Bad:
    text:String
    values:[i64]
fn descend(n:i64,text:String)->i64 ! (E | TimeError):
  if n==0:
    try sleep(nanoseconds(u64(0)))
    fail E.Bad {text:text,values:[20,20]}
  return try descend(n-1,text+"")
fn main()->unit ! (IoError | TaskError):
  result=descend(2000,"é!") catch fn(error):
    return match error:
      E.Bad {text,values}:values[0]+values[1]+length(text)
      TimeError.Unavailable:0
      TimeError.OutOfRange:0
  try println(format("{}",result))
  return ()
''', '42\n')

run('owned-file', '''from "std/fs" import File, open, OpenMode, FsError, writeLine, close
fn passAlong(own file:File,n:i64)->File ! TimeError:
  if n==0:
    try sleep(nanoseconds(u64(0)))
    return file
  return try passAlong(file,n-1)
fn main(args:[String])->unit ! (FsError | TimeError | IoError | TaskError):
  file=try open(args[0],OpenMode.Create)
  returned=try passAlong(file,2000)
  try writeLine(returned,"complete")
  try close(returned)
  try println("owned")
  return ()
''', 'owned\n', ('owned.txt',))
if not a.compile_only and (not a.case or 'owned-file' in a.case):
    assert (a.output / 'owned-file' / 'owned.txt').read_bytes() == b'complete\n'

run('repeated-results', '''fn descend(n:i64)->[i64] ! TimeError:
  if n==0:
    try sleep(nanoseconds(u64(0)))
    return [20,22]
  return try descend(n-1)
fn main()->unit ! (TimeError | IoError | TaskError):
  total:i64=0
  for i in range(1000):
    values=try descend(64)
    total=total+values[0]+values[1]
  try println(format("{}",total))
  return ()
''', '42000\n')

run('cancel-deep-sleeper', '''fn descend(n:i64)->i64 ! TimeError:
  if n==0:
    try sleep(try milliseconds(u64(86400000)))
    return 17
  return 1+try descend(n-1)
fn broken(divisor:i64)->i64:
  return 42/divisor
fn main()->unit ! (TimeError | IoError | TaskError):
  result=parallel:
    sleeper=spawn descend(2000)
    failure=spawn broken(0)
    value=try await failure
    try await sleeper
    value
  recovered=result catch fn(error):return -1
  try println(format("{}",recovered))
  return ()
''', '-1\n')

run('returned-callback', '''fn makePause(values:[i64],text:String)->fn()->i64 ! TimeError:
  return fn()->i64 ! TimeError:
    try sleep(nanoseconds(u64(1)))
    return values[0]+values[1]+length(text)
fn invoke(callback:fn()->i64 ! TimeError)->i64 ! TimeError:
  return try callback()
fn main()->unit ! (TimeError | IoError | TaskError):
  callback=makePause([20,20],"é!")
  try println(format("{}",try invoke(callback)))
  return ()
''', '42\n')

run('recursive-callback', '''fn descend(n:i64)->i64 ! TimeError:
  if n==0:
    try sleep(try milliseconds(u64(1)))
    return 17
  callback=descend
  return 1+try callback(n-1)
fn main()->unit ! (TimeError | IoError | TaskError):
  try println(format("{}",try descend(5000)))
  return ()
''', '5017\n')

run('borrowed-file', '''from "std/fs" import File, open, OpenMode, FsError, read, writeAll, close
fn inspect(file:File,n:i64)->u8 ! FsError:
  if n==0:
    values=try read(file,1)
    return values[0]
  return try inspect(file,n-1)
fn main(args:[String])->unit ! (FsError | IoError | TaskError):
  writer=try open(args[0],OpenMode.Create)
  try writeAll(writer,[42])
  try close(writer)
  file=try open(args[0],OpenMode.Read)
  try println(format("{}",try inspect(file,2000)))
  try close(file)
  return ()
''', '42\n', ('borrowed.txt',))

run('mixed-callback-targets', '''fn paused()->i64:
  sleep(nanoseconds(u64(1))) catch fn(error):return ()
  return 42
fn immediate()->i64:
  return 17
fn choose(pause:bool)->fn()->i64:
  return if pause:paused
  else:immediate
fn main()->unit ! (IoError | TaskError):
  first=choose(true)
  second=choose(false)
  try println(format("{}",first()))
  try println(format("{}",second()))
  return ()
''', '42\n17\n')

faulting = '''fn descend(n:i64,divisor:i64)->i64 ! (TimeError | TaskError):
  if n==0:
    try sleep(nanoseconds(u64(1)))
    return 42/divisor
  return 1+try descend(n-1,divisor)
'''
run('fault-propagation', faulting + '''fn main()->unit ! (IoError | TaskError):
  number=descend(2000,0) catch fn(error):return 42
  try println(format("{}",number))
  return ()
''', '', fault=True)

run('task-fault-catch', faulting + '''fn main()->unit ! (IoError | TaskError):
  result=parallel:
    try await spawn descend(2000,0)
  number=result catch fn(error):
    return match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          _:0
      _:0
  try println(format("{}",number))
  return ()
''', '42\n')

run('tensor-results', '''fn descend(n:i64)->Tensor[f32,2]:
  if n==0:
    sleep(nanoseconds(u64(1))) catch fn(error):return ()
    return tensor([20,22])
  return descend(n-1)
fn main()->unit ! (IoError | TaskError):
  total:f32=0
  for i in range(100):
    values=descend(64)
    total=total+reduceSum(values)
  try println(format("{}",total))
  return ()
''', '4200\n')

run('tensor-capture', '''fn makePause(values:Tensor[f32,2])->fn()->f32:
  return fn()->f32:
    sleep(nanoseconds(u64(1))) catch fn(error):return ()
    return reduceSum(values)
fn main()->unit ! (IoError | TaskError):
  callback=makePause(tensor([20,22]))
  try println(format("{}",callback()))
  return ()
''', '42\n')

run('returned-task-error', '''fn broken(divisor:i64)->i64:
  return 42/divisor
fn descend(n:i64)->i64 ! TaskError:
  if n==0:
    return try parallel:
      try await spawn broken(0)
  return try descend(n-1)
fn combine(n:i64)->i64 ! (TimeError | TaskError):
  if n==0:
    try sleep(nanoseconds(u64(1)))
    return try parallel:
      try await spawn broken(0)
  return try combine(n-1)
fn main()->unit ! (IoError | TaskError):
  number=descend(2000) catch fn(error):
    return match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          _:0
      _:0
  try println(format("{}",number))
  second=combine(2000) catch fn(error):return 42
  try println(format("{}",second))
  result=parallel:
    try await spawn descend(2000)
  third=result catch fn(error):return 42
  try println(format("{}",third))
  return ()
''', '42\n42\n42\n')

run('mutable-array-call', '''fn increment(mut values:[i64],n:i64)->unit ! TimeError:
  if n==0:
    try sleep(try milliseconds(u64(1)))
    values[0]=values[0]+1
    return ()
  return try increment(values,n-1)
fn main()->unit ! (TimeError | IoError | TaskError):
  values=[0]
  for i in range(10):
    try increment(values,2000)
  try println(format("{}",values[0]))
  return ()
''', '10\n')
