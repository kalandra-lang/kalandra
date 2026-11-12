"""Execute public iteration APIs with checked CPU and physical-device results."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'packaging'))
from LoopCases import CASES

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--variant', choices=('debug', 'optimized', 'sanitized', 'thread-sanitized'), required=True)
parser.add_argument('--target', choices=('cpu', 'metal', 'vulkan', 'cuda'), default='cpu')
parser.add_argument('--compile-only', action='store_true')
parser.add_argument('--case', action='append', dest='cases',
                    help='Run only this named case; repeat to select several cases.')
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
selected = set(args.cases or [])
executed = set()


def run(name, source, expected, fault=False):
    if selected and name not in selected:
        return
    executed.add(name)
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


numeric('match-loop-exits', CASES['match-loop-exits'][0].replace(
    'fn main()->i64:', 'fn answer()->i64:'), CASES['match-loop-exits'][1])

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

if args.target != 'cpu':
    run('host-cursor-through-gpu-await', '''from "std/iter" import iter, next, close
from "std/option" import Option
fn item(value:Option[i64])->i64:
  return match value:
    Option.None:-1
    Option.Some {value}:value
fn twice(value:i64)->i64:
  return value*2
fn main()->unit ! (IoError | TaskError):
  cursor=iter(steps(3))
  a=item(next(cursor))
  result=try parallel:
    gpu=devices.gpu()
    try await spawn twice(21) on gpu
  b=item(next(cursor))
  close(cursor)
  try println(format("{}:{}:{}",a,b,result))
  return ()
''', '0:1:42\n')

# These cursors execute on the host, including across a real GPU await. Device
# local cursor lowering is qualified separately from these host bridge cases.
array_imports = '''from "std/iter" import Iterator, iter, next, close
from "std/option" import Option
fn first[T](mut cursor:Iterator[T,never],fallback:T)->T:
  return match next(cursor):
    Option.None:fallback
    Option.Some {value}:value
fn marker(value:i64)->i64:
  return value+1
'''
gpu_pause = '''  checked=try parallel:
    gpu=devices.gpu()
    try await spawn marker(41) on gpu
''' if args.target != 'cpu' else '  checked=marker(41)\n'
run('cursor-array-replay-and-close', array_imports + '''fn main()->unit ! (IoError | TaskError):
  values=[3,1,2]
  left=iter(values)
  right=iter(values)
  a=first(left,-1)
''' + gpu_pause + '''  b=first(left,-1)
  close(left)
  c=first(right,-1)
  close(right)
  values[0]=9
  try println(format("{}:{}:{}:{}:{}",a,b,c,values[0],checked))
  return ()
''', '3:1:3:9:42\n')
# The shared GPU supervisor does not yet transport record/function arrays
# in host continuation slots. Keep their CPU coverage; do not silently claim
# that a host cursor bridge supplies that separate transport capability.
if args.target == 'cpu':
    run('cursor-array-managed-payloads', array_imports + '''type Box:
  values:[i64]
fn words()->Iterator[String,never]:
  return iter(["first","second"])
fn main()->unit ! (IoError | TaskError):
  a=[7,8]
  boxes=iter([Box {values:a}])
  value=first(boxes,Box {values:[]})
  close(boxes)
  value.values[0]=9
  cursor=words()
  word=first(cursor,"empty")
''' + gpu_pause + '''  other=first(cursor,"empty")
  close(cursor)
  try println(format("{}:{}:{}:{}",word,other,a[0],checked))
  return ()
''', 'first:second:9:42\n')
    run('cursor-array-callable-items', array_imports + '''fn main()->unit ! (IoError | TaskError):
  a=[7,8]
  callback=fn()->i64:return a[0]
  cursor=iter([callback])
  got=first(cursor,fn()->i64:return -1)
''' + gpu_pause + '''  close(cursor)
  a[0]=9
  try println(format("{}:{}",got(),checked))
  return ()
''', '9:42\n')
run('cursor-array-string-items', array_imports + '''fn words()->Iterator[String,never]:
  return iter(["first","second"])
fn main()->unit ! (IoError | TaskError):
  cursor=words()
  word=first(cursor,"empty")
''' + gpu_pause + '''  other=first(cursor,"empty")
  close(cursor)
  try println(format("{}:{}:{}",word,other,checked))
  return ()
''', 'first:second:42\n')
run('cursor-array-exhaustion-release', array_imports + '''fn main()->unit ! (IoError | TaskError):
  values=[3,1,2]
  cursor=iter(values)
  total=0
  while true:
    match next(cursor):
      Option.None:break
      Option.Some {value}:
        total=total+value
        ()
  values[0]=9
  absent=first(cursor,-1)
  values[0]=8
''' + gpu_pause + '''  close(cursor)
  try println(format("{}:{}:{}:{}",total,values[0],absent,checked))
  return ()
''', '6:8:-1:42\n')
run('cursor-array-task-transfer', array_imports + '''fn drain(own cursor:Iterator[i64,never])->i64:
  total=0
  while true:
    match next(cursor):
      Option.None:break
      Option.Some {value}:
        total=total+value
        ()
  close(cursor)
  return total
fn main()->unit ! (IoError | TaskError):
  values=[3,1,2]
  cursor=iter(values)
  total=try parallel:
    pending=spawn drain(cursor)
    try await pending
  values[0]=9
''' + gpu_pause + '''  try println(format("{}:{}:{}",total,values[0],checked))
  return ()
''', '6:9:42\n')
run('cursor-array-loop-lifetimes', array_imports + '''fn main()->unit ! (IoError | TaskError):
  values=[0]
  total=0
  i=0
  while i<1000:
    fresh=iter([i])
    total=total+first(fresh,-1)
    close(fresh)
    reader=iter(values)
    total=total+first(reader,-1)
    close(reader)
    values[0]=values[0]+1
    i=i+1
''' + gpu_pause + '''  try println(format("{}:{}:{}",total,values[0],checked))
  return ()
''', '999000:1000:42\n')

numeric_types=('i8','i16','i32','i64','u8','u16','u32','u64','f32','f64')
number_steps=''
for index, kind in enumerate(numeric_types):
    number_steps += f'''  c{index}=iter([{kind}(3),{kind}(5)])
  total=total+i64(first(c{index},{kind}(0)))+i64(first(c{index},{kind}(0)))
  close(c{index})
'''
run('cursor-array-numeric-types', array_imports + '''fn main()->unit ! (IoError | TaskError):
  total=0
''' + number_steps + gpu_pause + '''  bools=iter([true,false])
  a=first(bools,false)
  b=first(bools,true)
  close(bools)
  units=iter([(),()])
  x=first(units,())
  close(units)
  try println(format("{}:{}:{}:{}",total,a,b,checked))
  return ()
''', '80:true:false:42\n')

if args.target == 'cpu':
    cursor_imports = '''from "std/iter" import Iterator, iter, next, close
from "std/option" import Option
fn item(value:Option[i64])->i64:
  return match value:
    Option.None:-1
    Option.Some {value}:value
'''
    run('cursor-pure-entry', cursor_imports + '''fn invalid()->i64:
  return 0
fn main()->unit:
  cursor=iter(steps(7,8))
  if item(next(cursor)) != 7:
    unreachable=1/invalid()
  if item(next(cursor)) != -1:
    unreachable=1/invalid()
  close(cursor)
  return ()
''', '')
    run('cursor-item-and-error-signatures', cursor_imports + '''error Problem:
  Bad:
    message:String
type Node:
  value:i64
  child:Option[Node]
fn recursive(mut cursor:Iterator[Node,never])->Option[Node]:
  return next(cursor)
fn text(mut cursor:Iterator[String,Problem])->Option[String] ! Problem:
  return try next(cursor)
fn task(mut cursor:Iterator[i64,TaskError])->Option[i64] ! TaskError:
  return try next(cursor)
fn main()->unit:
  return ()
''', '')
    run('cursor-independent-sticky-exhaustion', cursor_imports + '''fn main()->unit ! IoError:
  source=steps(2,6,2)
  left=iter(source)
  right=iter(source)
  a=item(next(left))
  b=item(next(left))
  c=item(next(left))
  d=item(next(left))
  e=item(next(right))
  try println(format("{}:{}:{}:{}:{}",a,b,c,d,e))
  close(left)
  close(right)
  return ()
''', '2:4:-1:-1:2\n')
    run('cursor-match-break-and-continue', cursor_imports + '''fn main()->unit ! IoError:
  cursor=iter(steps(5))
  total=0
  while true:
    match next(cursor):
      Option.None: break
      Option.Some {value}:
        if value==2:
          continue
        total=total+value
        ()
  close(cursor)
  try println(format("{}",total))
  return ()
''', '8\n')
    run('cursor-temporary-owners-in-loop', cursor_imports + '''fn main()->unit ! IoError:
  total=0
  for index in range(1000):
    cursor=iter(steps(index,index+1))
    total=total+item(next(cursor))
  try println(format("{}",total))
  return ()
''', '499500\n')
    run('cursor-return-transfer-and-partial-close', cursor_imports + '''fn make()->Iterator[i64,never]:
  return iter(steps(10,20,3))
fn take(own cursor:Iterator[i64,never])->i64:
  value=item(next(cursor))
  close(cursor)
  return value
fn main()->unit ! IoError:
  cursor=make()
  a=item(next(cursor))
  b=take(cursor)
  try println(format("{}:{}",a,b))
  unused=iter(steps(9223372036854775807))
  close(unused)
  return ()
''', '10:13\n')
    run('cursor-iterable-static-dispatch', cursor_imports + '''from "std/iter" import Iterable
type Countdown:
  stop:i64
impl Iterable for Countdown:
  type Item=i64
  type Error=never
  fn iterator(values:Self)->Iterator[i64,never]:
    return iter(steps(values.stop,0,-1))
fn begin[S:Iterable](values:S)->Iterator[S.Item,S.Error]:
  return iter(values)
fn main()->unit ! IoError:
  source=Countdown {stop:3}
  cursor=begin(source)
  a=item(next(cursor))
  b=item(next(cursor))
  try println(format("{}:{}",a,b))
  close(cursor)
  return ()
''', '3:2\n')
    run('cursor-empty-and-overflow', cursor_imports + '''fn main()->unit ! IoError:
  empty=iter(steps(0))
  high=iter(steps(9223372036854775805,9223372036854775807,3))
  low=iter(steps(-9223372036854775806,-9223372036854775808,-3))
  a=item(next(empty))
  b=item(next(high))
  c=item(next(high))
  d=item(next(low))
  e=item(next(low))
  try println(format("{}:{}:{}:{}:{}",a,b,c,d,e))
  close(empty)
  close(high)
  close(low)
  return ()
''', '-1:9223372036854775805:-1:-9223372036854775806:-1\n')
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

if selected:
    assert executed == selected, f'Unknown or unavailable cases for {args.target}: {selected - executed}'
