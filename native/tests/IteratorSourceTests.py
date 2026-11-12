"""Execute public iteration APIs with checked CPU and physical-device results."""
import argparse
import os
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--variant', choices=('debug', 'optimized', 'sanitized', 'thread-sanitized'), required=True)
parser.add_argument('--target', choices=('cpu', 'metal', 'vulkan', 'cuda'), default='cpu')
parser.add_argument('--compile-only', action='store_true')
args = parser.parse_args()
args.compiler = args.compiler.resolve()
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
flag = {'debug': '-O0', 'optimized': '-O2', 'sanitized': '--sanitize',
        'thread-sanitized': '--thread-sanitize'}[args.variant]
prelude = '''from "std/iter" import Range, range as steps
from "std/io" import println, IoError
from "std/fmt" import format
'''


def run(name, source, expected, fault=False):
    root = args.output / name
    root.mkdir(exist_ok=True)
    path = root / 'main.k'
    path.write_text(prelude + source, encoding='utf-8')
    binary = root / ('program.exe' if os.name == 'nt' else 'program')
    destination = binary if args.target == 'cpu' else root / 'bundle'
    command = [args.compiler, path, flag, '-o', destination]
    if args.target != 'cpu':
        command.extend(['--target', args.target])
    built = subprocess.run(command, capture_output=True, timeout=240)
    (root / 'build.log').write_bytes(built.stdout + built.stderr)
    assert built.returncode == 0, (name, built)
    if args.compile_only:
        print('PASS compiled:', name, flush=True)
        return
    executable = binary if args.target == 'cpu' else destination / binary.name
    for workers in (1, 4):
        result = subprocess.run([executable], capture_output=True, timeout=60,
                                env=dict(os.environ, KALANDRA_WORKERS=str(workers)))
        (root / f'run-{workers}.log').write_bytes(result.stdout + result.stderr)
        stderr = result.stderr
        if args.target == 'metal':
            stderr = b''.join(line for line in stderr.splitlines(keepends=True)
                              if not re.fullmatch(rb'\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ program\[\d+:\d+\] Metal API Validation Enabled\r?\n?', line))
        if fault:
            assert result.returncode != 0 and not result.stdout and b'fault' in stderr.lower(), (name, result)
        else:
            assert result.returncode == 0 and result.stdout == expected.encode() and not stderr, (name, result)
    print('PASS executed:', name, flush=True)


def numeric(name, source, expected, arguments=''):
    if args.target == 'cpu':
        main = '''fn main()->unit ! IoError:
  try println(format("{}",answer()))
  return ()
'''
    else:
        main = '''fn main()->unit ! (IoError | TaskError):
  result=try parallel:
    gpu=devices.gpu()
    try await spawn answer() on gpu
  try println(format("{}",result))
  return ()
'''
    main = main.replace('answer()', f'answer({arguments})')
    run(name, source + main, f'{expected}\n')


numeric('range-defaults-and-replay', '''fn sum(values:Range)->i64:
  total=0
  for value in values:
    total=total+value
  return total
fn answer()->i64:
  values=steps(10,0,-2)
  return sum(values)+sum(steps(4))+sum(steps(2,5))+sum(values)
''', 75)

numeric('range-function-values-and-captures', '''fn answer()->i64:
  builder:fn(i64,i64,i64)->Range=steps
  values=builder(0,9,3)
  callback=fn()->i64:
    total=0
    for value in values:
      total=total+value
    return total
  return callback()+callback()
''', 18)

numeric('range-endpoints', '''fn count(values:Range)->i64:
  total=0
  for value in values:
    total=total+1
  return total
fn answer()->i64:
  a=count(steps(9223372036854775805,9223372036854775807,3))
  b=count(steps(-9223372036854775806,-9223372036854775808,-3))
  c=count(steps(10,0,1))
  d=count(steps(0,10,-1))
  e=count(steps(0))
  return a*10000+b*1000+c*100+d*10+e
''', 11000)

numeric('ranges-in-arrays-and-control', '''fn answer()->i64:
  total=0
  for values in [steps(0,5),steps(6,0,-2)]:
    for value in values:
      if value==2:
        continue
      if value==4:
        break
      total=total+value
  return total
''', 10)

numeric('range-lexical-shadowing', '''fn answer()->i64:
  range=fn(value:i64)->[i64]:
    return [41,value]
  total=0
  for value in range(1):
    total=total+value
  return total
''', 42)

numeric('range-dynamic-scalar-input', '''fn answer(start:i64)->i64:
  values=steps(start,start+10,2)
  total=0
  for value in values:
    total=total+value
  return total
''', 35, '3')

numeric('range-host-capture', '''fn answer(values:Range)->i64:
  total=0
  for value in values:
    total=total+value
  return total
''', 12, 'steps(1,10,3)')

if args.target == 'cpu':
    run('range-argument-order', '''fn mark(value:i64)->i64 ! IoError:
  try println(format("{}",value))
  return value
fn main()->unit ! IoError:
  values=steps(try mark(1),try mark(6),try mark(2))
  total=0
  for value in values:
    total=total+value
  for value in values:
    total=total+value
  try println(format("{}",total))
  return ()
''', '1\n6\n2\n18\n')
    for name, creation, traversal in (
        ('range-construction-zero-step', 'values=steps(0,0,zero())', ''),
        ('range-traversal-zero-step', 'values=Range {start:0,stop:0,step:zero()}',
         '  for value in values:\n    value+1\n')):
        run(name, 'fn zero()->i64:\n  return 0\nfn main()->unit:\n  ' + creation + '\n' +
            traversal + '  return ()\n', '', fault=True)
