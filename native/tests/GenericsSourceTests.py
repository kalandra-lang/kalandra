"""Execute concrete generic programs and reject invalid ownership/trait contracts."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--variant', choices=('debug', 'optimized', 'sanitized', 'thread-sanitized'),
                    required=True)
parser.add_argument('--target', choices=('cpu', 'metal', 'vulkan', 'cuda'), default='cpu')
parser.add_argument('--compile-only', action='store_true')
args = parser.parse_args()
compiler = args.compiler.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
flag = {'debug': '-O0', 'optimized': '-O2', 'sanitized': '--sanitize',
        'thread-sanitized': '--thread-sanitize'}[args.variant]
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'packaging'))
from GenericsCases import SOURCE, EXPECTED

prelude = 'from "std/io" import println, IoError\nfrom "std/fmt" import format\n'
main = 'fn main() -> unit ! (IoError | TaskError):\n'
ping = 'fn genericPing[T](value: T) -> T:\n  return value\n'
gpu_prefix = '''  genericDeviceProbeResult = try parallel:
    gpu = devices.gpu()
    try await spawn genericPing[i64](42) on gpu
  try println(format("{}", genericDeviceProbeResult))
'''


def run(name, source, expected, *, device=False):
    root = output / name
    root.mkdir(exist_ok=True)
    if args.target != 'cpu' and not device:
        # Host features must coexist with an actual specialized device call.
        header = next(line for line in source.splitlines(True) if line.startswith('fn main('))
        widened = header if 'TaskError' in header else header.replace('):', ' | TaskError):')
        assert 'TaskError' in widened, header
        source = ping + source.replace(header, widened + gpu_prefix, 1)
        expected = '42\n' + expected
    path = root / 'main.k'
    path.write_text(source, encoding='utf-8')
    binary = root / ('program.exe' if os.name == 'nt' else 'program')
    destination = binary if args.target == 'cpu' else root / 'bundle'
    command = [compiler, path, flag, '-o', destination]
    if args.target != 'cpu':
        command += ['--target', args.target]
    if args.target == 'metal':
        # Metal has no native f64; exercise the explicit, opt-in fallback too.
        command += ['--arithmetic', 'hardware-with-software-fallback']
    built = subprocess.run(command, capture_output=True, timeout=300)
    (root / 'build.log').write_bytes(built.stdout + built.stderr)
    assert built.returncode == 0, (name, built.returncode, built.stderr.decode(errors='replace'))
    if args.compile_only:
        print('PASS compiled', name, flush=True)
        return
    if args.target != 'cpu':
        binary = destination / binary.name
    for workers in ('1', '4'):
        completed = subprocess.run([binary], cwd=root, capture_output=True, timeout=120,
                                   env=dict(os.environ, KALANDRA_WORKERS=workers))
        (root / f'run-{workers}.log').write_bytes(completed.stdout + completed.stderr)
        stderr = completed.stderr
        if args.target == 'metal':
            stderr = b''.join(line for line in stderr.splitlines(keepends=True) if not re.fullmatch(
                rb'\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ program\[\d+:\d+\] Metal API Validation Enabled\r?\n?', line))
        assert completed.returncode == 0 and completed.stdout == expected.encode() and not stderr, (
            name, workers, completed.returncode, completed.stdout, stderr)
    print('PASS executed', name, flush=True)


run('portable', SOURCE, EXPECTED)
# Actual device algorithms use traits and generic local records/enums, not just a host bridge.
arithmetic = '''type Pair[T]:
  first: T
  second: T
enum Option[T]:
  None
  Some:
    value: T
trait Add:
  fn add(a: Self, b: Self) -> Self
'''
numeric_types = ('i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64')
for numeric in numeric_types:
    arithmetic += f'''impl Add for {numeric}:
  fn add(a: Self, b: Self) -> Self:
    return a + b
'''
arithmetic += '''fn sum[T: Add](left: T, right: T) -> T:
  pair = Pair[T] {first: left, second: right}
  result = Option[T].Some {value: Add.add(pair.first, pair.second)}
  return match result:
    Option.None: left
    Option.Some {value}: value
'''
body = ''
for numeric in numeric_types:
    if args.target == 'cpu':
        body += f'  value{numeric} = sum[{numeric}]({numeric}(20), {numeric}(22))\n'
    else:
        body += f'''  value{numeric} = try parallel:
    gpu = devices.gpu()
    try await spawn sum[{numeric}]({numeric}(20), {numeric}(22)) on gpu
'''
    body += f'  try println(format("{{}}", value{numeric}))\n'
run('all-numeric-device', prelude + arithmetic + main + body + '  return ()\n',
    '42\n' * len(numeric_types), device=True)

closures = '''trait Add:
  fn add(a: Self, b: Self) -> Self
impl Add for i64:
  fn add(a: Self, b: Self) -> Self:
    return a + b
fn makeAdder[T: Add](offset: T) -> fn(T) -> T:
  return fn(value: T): return Add.add(value, offset)
fn apply[T](value: T, callback: fn(T) -> T) -> T:
  return callback(value)
'''
if args.target == 'cpu':
    closure_body = '  value = apply[i64](20, makeAdder[i64](22))\n'
else:
    closure_body = '''  callback = makeAdder[i64](22)
  value = try parallel:
    gpu = devices.gpu()
    try await spawn apply[i64](20, callback) on gpu
'''
run('device-closure', prelude + closures + main + closure_body +
    '  try println(format("{}", value))\n  return ()\n', '42\n', device=True)

owned = '''from "std/buffer" import ByteBuffer, BufferError, byteBuffer, append, intoArray
from "std/time" import sleep, nanoseconds, TimeError
fn pass[T](own value: T, depth: i64) -> T ! TimeError:
  if depth == 0:
    return value
  try sleep(nanoseconds(0))
  return try pass(value, depth - 1)
fn direct(own value: ByteBuffer) -> ByteBuffer:
  return value
fn replace[T](mut values: [T], value: T) -> unit:
  values[0] = value
  return ()
fn main() -> unit ! (IoError | BufferError | TimeError | TaskError):
  original = try byteBuffer()
  try append(original, [40, 2])
  moved = try parallel:
    task = spawn pass(original, 2000)
    try await task
  values = intoArray(moved)
  second = try byteBuffer()
  try append(second, [43])
  returned = try parallel:
    task = spawn direct(second)
    alias = task
    try await alias
  ordinary = intoArray(returned)
  data = [20, 1]
  replace(data, 42)
  flags = [false]
  replace(flags, true)
  try println(format("{}:{}:{}:{}", values[0] + values[1], data[0], flags[0], ordinary[0]))
  return ()
'''
run('owned-recursion-and-mutation', prelude + owned, '42:42:true:43\n')

negative = {
    'double-await-owned-result': (
        'from "std/buffer" import ByteBuffer, byteBuffer, BufferError, intoArray\n'
        'fn move[T](own value:T)->T:\n  return value\n'
        'fn main()->unit ! (BufferError | TaskError):\n  b=try byteBuffer()\n'
        '  try parallel:\n    task=spawn move(b)\n    first=try await task\n'
        '    second=try await task\n    intoArray(first)\n  return ()\n', 'S081'),
    'borrowed-return': (
        'from "std/buffer" import ByteBuffer, byteBuffer, BufferError\n'
        'fn bad[T](value:T)->T:\n  return value\n'
        'fn main()->unit ! BufferError:\n  b=try byteBuffer()\n  moved=bad(b)\n  return ()\n',
        'S081'),
    'owned-use-after-move': (
        'from "std/buffer" import ByteBuffer, byteBuffer, BufferError\n'
        'fn move[T](own value:T)->T:\n  return value\n'
        'fn main()->i64 ! BufferError:\n  b=try byteBuffer()\n'
        '  moved=move(b)\n  return length(b)\n', 'S081'),
    'mut-reader': (
        'fn read[T](values:[T])->i64:\n  return length(values)\n'
        'fn edit[T](mut values:[T], value:T)->unit:\n  values[0]=value\n  return ()\n'
        'fn main()->unit ! TaskError:\n  values=[1,2]\n  try parallel:\n'
        '    task=spawn read(values)\n    edit(values,3)\n    try await task\n  return ()\n',
        'S077'),
    'owned-generic-field': (
        'from "std/buffer" import ByteBuffer\ntype Box[T]:\n  value:T\n'
        'fn invalid(value:Box[ByteBuffer])->unit:\n  return ()\n'
        'fn main()->unit:\n  return ()\n', 'S081'),
    'error-type-mismatch': (
        'fn invalid[E](value:i64)->i64 ! E:\n  return value\n'
        'fn main()->i64:\n  return invalid[i64](1)\n', 'S072'),
    'erased-index-expression': (
        'fn id[T](value:T)->T:\n  return value\n'
        'fn sideEffect()->i64:\n  return 0\n'
        'fn main()->i64:\n  return id[sideEffect()](42)\n', 'S082'),
}
for name, (source, code) in negative.items():
    root = output / name
    root.mkdir(exist_ok=True)
    path = root / 'main.k'
    path.write_text(source, encoding='utf-8')
    command = [compiler, path, '-O2', '-o', root / 'unexpected']
    if args.target != 'cpu':
        command += ['--target', args.target]
    if args.target == 'metal':
        # Metal has no native f64; exercise the explicit, opt-in fallback too.
        command += ['--arithmetic', 'hardware-with-software-fallback']
    rejected = subprocess.run(command, capture_output=True, timeout=120)
    (root / 'build.log').write_bytes(rejected.stdout + rejected.stderr)
    assert rejected.returncode == 1 and f'error[{code}]'.encode() in rejected.stderr, (
        name, rejected.returncode, rejected.stderr)
    print('PASS diagnosed', name, flush=True)
