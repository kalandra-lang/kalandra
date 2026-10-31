"""Native ARM64 scalar baseline on shared CI; exploratory, not a speed guarantee.

Equivalent bounded tail-recursive integer recurrences, compiled with the same
Clang and -O2. Time hot calls inside one executable, excluding compilation and
process startup. Retain ABBA/BAAB samples and an identical-C++ control. Validate
all checksums with an independent Python integer oracle. No timing threshold.
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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'packaging'))
from WindowsPlatform import MACHINES, pe_machine

SOURCE = '''fn work(value:int,rounds:int)->int:
  if rounds==0:value
  else:work((value*13+7)%1000003,rounds-1)
fn main()->int:
  work(17,1000000)
'''
REFERENCE = '''#include <cstdint>
extern "C" [[gnu::noinline]] std::int64_t reference(std::int64_t value, std::int64_t rounds) {
  if (rounds == 0) return value;
  return reference((value * 13 + 7) % 1000003, rounds - 1);
}
'''
DRIVER = '''#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
extern "C" std::int64_t reference(std::int64_t, std::int64_t);
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry_ordinary_direct_v1(std::int64_t, std::int64_t);
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  auto function = std::strcmp(argv[1], "cpp") == 0 ? reference : _mlir_ciface_kalandra_test_entry_ordinary_direct_v1;
  volatile auto warm = function(17, 1000);
  (void)warm;
  std::int64_t checksum = 0;
  std::int64_t values[4]{};
  const auto start = std::chrono::steady_clock::now();
  for (std::int64_t seed = 17; seed < 21; ++seed) {
    values[seed - 17] = function(seed, 1000000);
    checksum += values[seed - 17];
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("%.17g %lld %lld %lld %lld %lld\\n", seconds, static_cast<long long>(checksum),
              static_cast<long long>(values[0]), static_cast<long long>(values[1]),
              static_cast<long long>(values[2]), static_cast<long long>(values[3]));
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--clang', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != 'Windows' or platform.machine().lower() not in ('arm64', 'aarch64'):
        raise RuntimeError('Requires native Windows ARM64 hardware')
    args.compiler = args.compiler.resolve()
    args.clang = args.clang.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    source, reference, driver = [args.output / name for name in ('work.k', 'reference.cpp', 'driver.cpp')]
    for path, content in ((source, SOURCE), (reference, REFERENCE), (driver, DRIVER)):
        path.write_text(content, encoding='utf-8')
    ir, executable = args.output / 'work.ll', args.output / 'measure.exe'
    commands = []
    def run(command):
        commands.append(list(map(str, command)))
        result = subprocess.run(commands[-1], text=True, capture_output=True, timeout=180)
        if result.returncode:
            raise RuntimeError(f"{commands[-1]} failed ({result.returncode})\n{result.stdout}\n{result.stderr}")
        return result.stdout
    run([args.compiler, source, '-O2', '--test-entry', 'work', '--emit-llvm', ir, '-o', args.output / 'standalone.exe'])
    if '_mlir_ciface_kalandra_test_entry_ordinary_direct_v1' not in ir.read_text():
        raise RuntimeError('Expected ordinary scalar ABI was not emitted')
    runtime = args.compiler.parent / 'kalandra-task-core-optimized.lib'
    run([args.clang, '-std=c++23', '-O2', '-ffp-contract=off', '-Wno-override-module',
         '-fms-runtime-lib=static', '--rtlib=compiler-rt', driver, reference, ir, runtime, '-o', executable])
    for path in (args.compiler, args.clang, executable):
        if pe_machine(path) != MACHINES['windows-arm64']:
            raise RuntimeError(f'Benchmark must run native ARM64: {path}')
    expected = 0
    expected_values = []
    for seed in range(17, 21):
        value = seed
        for _ in range(1000000):
            value = (value * 13 + 7) % 1000003
        expected += value
        expected_values.append(value)
    samples = []
    for block, order in enumerate([['cpp'] * 4, *[['kalandra', 'cpp', 'cpp', 'kalandra']] * 3,
                                   *[['cpp', 'kalandra', 'kalandra', 'cpp']] * 3]):
        for position, engine in enumerate(order):
            seconds, checksum, *values = run([executable, engine]).strip().split()
            values = list(map(int, values))
            if int(checksum) != expected or values != expected_values or not float(seconds) > 0:
                raise RuntimeError(f'Incorrect {engine} benchmark result: checksum={checksum}, values={values}; expected {expected}, {expected_values}')
            samples.append(dict(block=block, position=position, engine=engine, seconds=float(seconds), checksum=int(checksum), values=values))
    medians = {engine: statistics.median(s['seconds'] for s in samples if s['block'] and s['engine'] == engine)
               for engine in ('cpp', 'kalandra')}
    control = [s['seconds'] for s in samples if not s['block']]
    control_ratio = max(control) / min(control)
    report = dict(scope=__doc__, platform=platform.platform(), processor=os.environ.get('PROCESSOR_IDENTIFIER'),
                  clang=run([args.clang, '--version']), iterationsPerInvocation=1000000, invocationsPerSample=4,
                  expectedChecksum=expected, expectedValues=expected_values, samples=samples, processMedianSeconds=medians,
                  kalandraToCpp=medians['kalandra'] / medians['cpp'], identicalControlMaxToMin=control_ratio,
                  timingClaimQualified=False,
                  caveat='Shared hosted VM; one valid-input scalar workload. No general performance claim or timing acceptance threshold.',
                  inputs={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (source, reference, driver, ir)},
                  toolSHA256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (args.compiler, args.clang, runtime, executable)},
                  commands=commands)
    (args.output / 'measurement.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('expectedChecksum', 'processMedianSeconds', 'kalandraToCpp', 'identicalControlMaxToMin', 'timingClaimQualified')}), flush=True)


if __name__ == '__main__':
    main()
