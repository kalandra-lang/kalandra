"""Execute lexical returns and unit values through a physical GPU backend."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--target', choices=('metal', 'vulkan', 'cuda'), required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)

cases = {
    'loop-exit': ('''fn work() -> i64:
  for i in range(10):
    if i == 7:
      return i * 6
  return 0
''', 'i64', False, 42),
    'closure-exit': ('''fn work() -> i64:
  callback = fn(value: i64) -> i64:
    if value < 0:
      return -value
    return value
  return callback(-42)
''', 'i64', False, 42),
    'handler-exit': ('''error InputError:
  Bad
fn checked() -> i64 ! InputError:
  fail InputError.Bad
fn work() -> i64:
  result = checked() catch fn(error):
    if true:
      return 42
    return 0
  return result
''', 'i64', False, 42),
    'owned-exit': ('''fn work() -> [i64]:
  values = [20, 22]
  for i in range(3):
    if i == 1:
      return values
  return [0, 0]
''', 'i64', True, 42),
    'abandoned-collection': ('''fn choose() -> i64:
  values = for i in range(10):
    if i == 7:
      return 42
    yield i
  return length(values)
fn work() -> i64:
  callback = fn() -> i64: return choose()
  return callback()
''', 'i64', False, 42),
    'handler-loop-channel': ('''error InputError:
  Bad
fn reject() -> i64 ! InputError:
  fail InputError.Bad
fn fallback() -> i64:
  return reject() catch fn(error):
    return while false:
      continue
    else:
      42
fn work() -> i64:
  return fallback()
''', 'i64', False, 42),
    'handler-match-replacement': ('''error InputError:
  Invalid:
    value: i64

error OutputError:
  Replaced:
    values: [i64]

fn validate(value: i64) -> i64 ! InputError:
  fail InputError.Invalid { value: value }

fn replace(value: i64) -> i64 ! OutputError:
  converted = validate(value) catch fn(error):
    return match error:
      InputError.Invalid { value }: fail OutputError.Replaced { values: [value, 2] }
  return try converted

fn work() -> i64:
  return replace(40) catch fn(error):
    return match error:
      OutputError.Replaced { values }: values[0] + values[1]
''', 'i64', False, 42),
    'unit-values': ('''fn work() -> i64:
  values = generate(5, fn(index: i64) -> unit: return ())
  mapped = map(values, fn(value: unit) -> unit: return value)
  if mapped[0] == ():
    return length(mapped)
  return 0
''', 'i64', False, 5),
    'unit-array-exit': ('''fn work() -> [unit]:
  return [(), ()]
''', '[unit]', False, '[(),()]\n'),
    'unit-exit': ('''fn work() -> unit:
  for i in range(3):
    if i == 1:
      return
''', 'unit', False, None),
}

for name, (worker, result_type, owned, expected) in cases.items():
    main = f'''fn main() -> {result_type} ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    result = try await spawn work() on gpu
'''
    main += ('    result[0] + result[1]\n' if owned else '    result\n')
    path = args.output / (name + '.k')
    path.write_text(worker + main)
    for optimization in ('-O0', '-O2'):
        bundle = args.output / (name + optimization)
        compiled = subprocess.run([args.compiler, path, '--target', args.target,
                                   optimization, '-o', bundle], text=True,
                                  capture_output=True, timeout=240)
        assert compiled.returncode == 0, (name, compiled.stdout, compiled.stderr)
        executed = subprocess.run([bundle / 'program'], text=True,
                                  capture_output=True, timeout=120)
        assert executed.returncode == 0, (name, executed.stdout, executed.stderr)
        if expected is None:
            assert executed.stdout == '', (name, executed.stdout)
        elif isinstance(expected, str):
            assert executed.stdout == expected, (name, executed.stdout)
        else:
            assert int(executed.stdout.strip(), 16) == expected, (name, executed.stdout)
    print(name + ': passed', flush=True)
print(f'{len(cases)} physical {args.target} return/unit cases passed')
