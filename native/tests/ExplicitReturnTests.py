"""Exercise lexical returns, unit completion, and rejected legacy spellings."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--checker', required=True)
parser.add_argument('--output', required=True)
args = parser.parse_args()
root = Path(args.output)
root.mkdir(parents=True, exist_ok=True)

cases = {
    'conditional-expression': ('''fn main() -> i64:
  x = 42
  return if x > 10:
    x
  else:
    0
''', '42\n'),
    'nested-loops': ('''fn pick() -> i64:
  for i in range(10):
    for j in range(10):
      if i + j == 12:
        return i * j
  return 0
fn main() -> i64:
  return pick()
''', '27\n'),
    'match-exits': ('''enum Choice:
  First
  Second
fn pick(x: Choice) -> i64:
  return match x:
    Choice.First: return 20
    Choice.Second: return 22
fn main() -> i64:
  return pick(Choice.First) + pick(Choice.Second)
''', '42\n'),
    'closure-boundary': ('''fn main() -> i64:
  callback = fn(x: i64) -> i64:
    if x < 0:
      return -x
    return x
  value = callback(-20)
  return value + 22
''', '42\n'),
    'owned-array': ('''fn values(stop: bool) -> [i64]:
  result = [20, 22]
  if stop:
    return result
  return [0]
fn main() -> i64:
  result = values(true)
  return result[0] + result[1]
''', '42\n'),
    'parallel-exit': ('''fn work() -> i64:
  return 21
fn main() -> i64 ! TaskError:
  return try parallel:
    task = spawn work()
    value = try await task
    return value * 2
''', '42\n'),
    'exiting-expression-branch': ('''fn choose(flag: bool) -> i64:
  text = if flag:
    return 42
  else:
    "abc"
  return length(text) + 39
fn main() -> i64:
  return choose(true) + choose(false)
''', '84\n'),
    'abandoned-collection': ('''fn pick() -> i64:
  values = for i in range(10):
    if i == 7:
      return 42
    yield i
  return length(values)
fn main() -> i64:
  return pick()
''', '42\n'),
    'loop-expression-exit': ('''fn choose(run: bool) -> i64:
  return while run:
    break 42
  else:
    return 10
fn main() -> i64:
  return choose(true) + choose(false)
''', '52\n'),
    'handler-boundary': ('''error InputError:
  Bad
fn checked(value: i64) -> i64 ! InputError:
  if value < 0:
    fail InputError.Bad
  return value
fn main() -> i64:
  value = checked(-1) catch fn(error):
    if true:
      return 20
    return 22
  return value + 22
''', '42\n'),
    'handler-forwarded-error': ('''error FirstError:
  Bad
error SecondError:
  Bad
fn first() -> i64 ! FirstError:
  fail FirstError.Bad
fn second() -> i64 ! SecondError:
  fail SecondError.Bad
fn recover(flag: bool) -> i64 ! SecondError:
  return first() catch fn(error):
    if flag:
      return second()
    return 1
fn main() -> i64:
  value = recover(true) catch fn(error):
    return 42
  return value
''', '42\n'),
    'handler-fail-only': ('''error FirstError:
  Bad
error SecondError:
  Bad
fn first() -> i64 ! FirstError:
  fail FirstError.Bad
fn recover() -> i64 ! SecondError:
  return first() catch fn(error):
    fail SecondError.Bad
fn main() -> i64:
  return recover() catch fn(error):
    return 42
''', '42\n'),
    'owned-record-and-closure': ('''type Box:
  text: String
  values: [i64]
fn make(flag: bool) -> Box:
  if flag:
    return Box { text: "hi" + "!", values: [20, 22] }
  return Box { text: "no", values: [0, 0] }
fn factory(flag: bool) -> fn(i64) -> i64:
  offset = 20
  if flag:
    return fn(value: i64) -> i64: return value + offset
  return fn(value: i64) -> i64: return value
fn text(flag: bool) -> String:
  if flag:
    return "a" + "bc"
  return ""
fn main() -> i64:
  box = make(true)
  callback = factory(true)
  return box.values[0] + box.values[1] + length(box.text) + length(text(true)) + callback(16)
''', '84\n'),
    'owned-tensor': ('''fn values(flag: bool) -> Tensor[i64,?,?]:
  if flag:
    return tensor([[20, 22]])
  return tensor([[0, 0]])
fn main() -> i64:
  return reduceSum(values(true))
''', '42\n'),
    'owned-error-return': ('''error InputError:
  Bad
fn values(flag: bool) -> [i64] ! InputError:
  if flag:
    return [20, 22]
  fail InputError.Bad
fn main() -> i64:
  values = values(true) catch fn(error): return [0, 0]
  return values[0] + values[1]
''', '42\n'),
    'unit-array-entry': ('fn main() -> [unit]:\n  return [(), ()]\n', '[(), ()]\n'),
    'callable-expression-exit': ('''fn choose(flag: bool) -> fn(i64) -> i64:
  callback = if flag:
    return fn(value: i64) -> i64: return value + 40
  else:
    fn(value: i64) -> i64: return value + 20
  return callback
fn main() -> i64:
  first = choose(true)
  second = choose(false)
  return first(1) + second(1)
''', '62\n'),
    'returned-callable-task': ('fn choose(flag: bool) -> fn(i64) -> i64:\n  callback = if flag:\n    return fn(value: i64) -> i64: return value + 40\n  else:\n    fn(value: i64) -> i64: return value + 20\n  return callback\nfn apply(callback: fn(i64) -> i64) -> i64:\n  return callback(1)\nfn main() -> i64 ! TaskError:\n  return try parallel:\n    first = spawn apply(choose(true))\n    second = spawn apply(choose(false))\n    a = try await first\n    b = try await second\n    a + b\n', '62\n'),
    'unit-values': ('''type Box:
  done: unit
fn identity(value: unit) -> unit:
  return value
fn main() -> i64:
  box = Box { done: () }
  values = generate(5, fn(index: i64) -> unit: return box.done)
  mapped = map(values, fn(value: unit) -> unit: return identity(value))
  if mapped[0] == ():
    return length(mapped)
  return 0
''', '5\n'),
    'unit-task': ('''fn nothing() -> unit:
  return
fn main() -> i64 ! TaskError:
  return try parallel:
    observed = try await spawn nothing()
    if observed == ():
      return 42
    return 0
''', '42\n'),
    'parallel-owned-exit': ('''fn work() -> [i64]:
  return [20, 22]
fn values() -> [i64] ! TaskError:
  try parallel:
    task = spawn work()
    result = try await task
    return result
fn main() -> i64 ! TaskError:
  result = try values()
  return result[0] + result[1]
''', '42\n'),
    'error-only': ('''error InputError:
  Bad
fn reject() -> i64 ! InputError:
  fail InputError.Bad
fn failOnly() -> unit ! InputError:
  fail InputError.Bad
fn main() -> i64:
  observed = failOnly() catch fn(error): return ()
  return reject() catch fn(error): return 42
''', '42\n'),
    'unit-completion': ('''fn explicit() -> unit:
  return ()
fn bare() -> unit:
  return
fn natural() -> unit:
  explicit()
  return bare()
fn main() -> unit:
  return natural()
''', ''),
}
for name, (source, expected) in cases.items():
    path = root / (name + '.k')
    path.write_text(source)
    binary = root / (name + '.exe')
    for optimization in ('-O0', '-O2'):
        compiled = subprocess.run([args.compiler, str(path), optimization, '-o', str(binary)],
                                  text=True, capture_output=True, timeout=120)
        assert compiled.returncode == 0, (name, optimization, compiled.stderr)
        for workers in (1, 4):
            environment = dict(os.environ, KALANDRA_WORKERS=str(workers))
            result = subprocess.run([str(binary)], env=environment, text=True,
                                    capture_output=True, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, expected, ''), (
                name, optimization, workers, result)

uncaught = root / 'uncaught-unit.k'
uncaught.write_text('''error InputError:
  Bad
fn main() -> unit ! InputError:
  fail InputError.Bad
''')
for optimization in ('-O0', '-O2'):
    binary = root / 'uncaught-unit.exe'
    built = subprocess.run([args.compiler, str(uncaught), optimization, '-o', str(binary)],
                           text=True, capture_output=True, timeout=120)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
    assert result.returncode == 89 and not result.stdout and result.stderr.strip() == 'InputError.Bad', result

rejected = {
    'missing-return': 'fn main() -> i64:\n  42\n',
    'partial-return': 'fn main() -> i64:\n  if true:\n    return 42\n',
    'unreachable': 'fn main() -> i64:\n  return 42\n  return 0\n',
    'wrong-return': 'fn main() -> i64:\n  return true\n',
    'bare-numeric-return': 'fn main() -> i64:\n  return\n',
    'unit-value-discard': 'fn main() -> unit:\n  42\n',
    'unobserved-exit': '''fn work() -> i64:
  return 1
fn choose(flag: bool) -> i64 ! TaskError:
  return try parallel:
    task = spawn work()
    if flag:
      return 42
    value = try await task
    value
fn main() -> i64 ! TaskError:
  return try choose(true)
''',
    'immutable-parameter': 'fn bad(value: String) -> String:\n  value = "new"\n  return value\n',
    'unit-as-condition': 'fn main() -> i64:\n  return if ():\n    1\n  else:\n    0\n',
}
for old in ('int', 'uint', 'float', 'int32', 'uint32', 'float32', 'Bool'):
    rejected['old-' + old] = f'fn main() -> {old}:\n  return 0\n'
for name, source in rejected.items():
    path = root / (name + '.k')
    path.write_text(source)
    result = subprocess.run([args.checker, str(path)], text=True, capture_output=True, timeout=30)
    assert result.returncode != 0, (name, result.stdout)
print(f'{len(cases)} executable cases and {len(rejected)} rejected cases passed')
