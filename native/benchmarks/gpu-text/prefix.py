"""Compare CPU and GPU text prefix masks, including the GPU text upload.

The measured region ends when the mask is available on the host. It includes
the result download and excludes input generation and output verification.
Build both variants first, then time them sequentially with the same inputs.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'cooperative'))
from load_monitor import load


COMMON = '''from "std/time" import now, elapsed, asNanoseconds, TimeError
from "std/io" import println, IoError
from "std/fmt" import format

fn rowsOf(count: i64) -> [String]:
  samples = ["سلام 🧪", "", "e\\u{301}Kalandra", "سیب猫"]
  return generate(count, fn(i: i64) -> String: return samples[i % 4])

fn matchPrefix(row: String, initial: String) -> u8:
  if byteLength(row) == 0:
    return u8(0)
  return if row[0] == initial: u8(1)
  else: u8(0)
'''

GPU = '''fn prefix(rows: [String], initial: String) -> Tensor[u8, ?]:
  n = length(rows)
  return dispatch([n], [256], fn(item: WorkItem) -> u8:
    i = item.globalIndex(0)
    if i >= n:
      return u8(0)
    return matchPrefix(rows[i], initial)
  )

fn run(rows: [String], initial: String) -> [u8] ! TaskError:
  result: Tensor[u8, ?] = try parallel:
    device = devices.gpu()
    mask = try await spawn prefix(rows, initial) on device
    try await transfer(mask, devices.cpu())
  return array(result)
'''

CPU = '''fn run(rows: [String], initial: String) -> [u8] ! TaskError:
  return generate(length(rows), fn(i: i64) -> u8: return matchPrefix(rows[i], initial))
'''

MAIN = '''fn main() -> unit ! (TaskError | TimeError | IoError):
  rows = rowsOf(__COUNT__)
  initial = "س"
  start = try now()
  mask = try run(rows, initial)
  duration = try elapsed(start)
  wrong = if length(mask) == __COUNT__: 0
  else: 1
  for i in range(length(mask)):
    expected = if i % 4 == 0 or i % 4 == 3: u8(1)
    else: u8(0)
    if mask[i] != expected:
      wrong = wrong + 1
  try println(format("{} {}", asNanoseconds(duration), wrong))
  return ()
'''


def capture(command, env):
    run = subprocess.run([str(item) for item in command], capture_output=True, text=True,
                         timeout=180, env=env)
    if run.returncode:
        raise RuntimeError(f'{command}\n{run.stdout}\n{run.stderr}')
    return run.stdout, run.stderr


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--backend', choices=('metal', 'vulkan', 'cuda'), required=True)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--sizes', nargs='+', type=int, default=[16, 1024])
    parser.add_argument('--blocks', type=int, default=5)
    parser.add_argument('--modes', nargs='+', choices=('cpu', 'gpu'), default=('cpu', 'gpu'))
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    if any(size <= 0 for size in args.sizes) or args.blocks < 2:
        parser.error('positive sizes and at least two blocks are required')
    if len(set(args.modes)) != len(args.modes):
        parser.error('select each mode at most once')
    compiler, directory = args.compiler.resolve(), args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, 'KALANDRA_WORKERS': '1'}
    exe_suffix = '.exe' if os.name == 'nt' else ''
    commands = {}
    inputs = {}
    records = []
    for count in args.sizes:
        for kind in args.modes:
            source = directory / f'{kind}-{count}.k'
            imports = 'from "std/gpu" import dispatch, WorkItem\n' if kind == 'gpu' else ''
            code = imports + COMMON + '\n' + (GPU if kind == 'gpu' else CPU) + '\n' + MAIN
            source.write_text(code.replace('__COUNT__', str(count)), encoding='utf-8')
            path = directory / f'{kind}-{count}'
            flags = ['--target', args.backend] if kind == 'gpu' else []
            if kind == 'cpu':
                path = path.with_suffix(exe_suffix)
            _, diagnostics = capture([compiler, source, *flags, '-O2', '-o', path], env)
            binary = path / ('program' + exe_suffix) if kind == 'gpu' else path
            commands[count, kind] = binary
            inputs[f'{count}-{kind}'] = {
                'source': sha(source), 'binary': sha(binary), 'diagnostics': diagnostics,
                'shaders': {p.name: sha(p) for p in path.iterdir()
                            if p.suffix in ('.metallib', '.spv', '.ptx')}
                if kind == 'gpu' else {},
            }
    if args.prepare_only:
        return

    def sample(count, block, kind):
        raw, diagnostics = capture([commands[count, kind]], env)
        ns, wrong = (int(field) for field in raw.split())
        if wrong or ns <= 0:
            raise AssertionError((count, kind, ns, wrong, diagnostics))
        records.append(dict(rows=count, block=block, mode=kind, nanoseconds=ns,
                            output=raw, diagnostics=diagnostics, load=load()))

    complete = False
    try:
        load()
        time.sleep(0.5)
        for count in args.sizes:
            for kind in args.modes:
                sample(count, -1, kind)
            for block in range(args.blocks):
                order = ('cpu', 'gpu', 'gpu', 'cpu') if block % 2 == 0 else ('gpu', 'cpu', 'cpu', 'gpu')
                for kind in order:
                    if kind in args.modes:
                        sample(count, block, kind)
        complete = True
    finally:
        medians = {}
        for count in args.sizes:
            for kind in args.modes:
                times = [r['nanoseconds'] for r in records if r['rows'] == count and
                         r['mode'] == kind and r['block'] >= 0]
                if times:
                    medians.setdefault(count, {})[kind] = statistics.median(times)
        result = dict(host=platform.platform(), backend=args.backend, compiler=sha(compiler),
                      complete=complete, inputs=inputs, observations=records,
                      medianNanoseconds=medians)
        (directory / 'measurements.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(medians, indent=2))


if __name__ == '__main__':
    main()
