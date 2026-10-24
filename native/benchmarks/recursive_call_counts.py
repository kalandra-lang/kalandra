"""Count private recursive frame entries; instrumented times are not benchmarks."""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p
import source_run as source


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists()
    compiler = p.ROOT / 'build/native/native/kalandra'
    runtime = compiler.parent / 'libkalandra-task-core-optimized.a'
    directory = p.BUILD.parent / 'recursive-call-counts'
    directory.mkdir(exist_ok=True)
    original = directory / 'polynomial.ll'
    src = Path(__file__).resolve().parent / 'fair'
    inputs = [Path(__file__), src / 'source.k', src / 'SourceDriver.cpp',
              src / 'source_run.py', src / 'protocol.py', compiler, runtime]
    frozen = {path: p.sha(path) for path in inputs}
    p.run([compiler, src / 'source.k', '--test-entry', 'polynomial', '--emit-llvm',
           original, '-O2', '-o', directory / 'standalone'])
    insertion = '''  %profile.address = getelementptr [16 x i64], ptr @recursive_profile, i64 0, i64 %1
  %profile.previous = load volatile i64, ptr %profile.address
  %profile.next = add i64 %profile.previous, 1
  store volatile i64 %profile.next, ptr %profile.address
'''
    # This diagnostic targets the fair source's (x, remaining_depth) functions.
    # Inputs 9..14 recurse only to zero, so every counter index is in bounds.
    text, count = re.subn(
        r'^define internal[^\n]*@kalandra_fn_\d+\.recursive\.clear\.depth\([^\n]*\{\n',
        lambda match: match[0] + insertion, original.read_text(), flags=re.M)
    assert count == 2, 'Expected polynomial and branching private depth workers'
    ir = directory / 'instrumented.ll'
    ir.write_text(text + '\n@recursive_profile = global [16 x i64] zeroinitializer\n')
    driver = directory / 'counters.cpp'
    driver.write_text('''#include <cstdint>
#include <cstdio>
extern "C" std::int64_t recursive_profile[16];
struct Profile {
  ~Profile() {
    for (int i = 0; i < 16; ++i)
      std::fprintf(stderr, "%lld%c", (long long)recursive_profile[i], i == 15 ? '\\n' : ' ');
  }
} profile;
''')
    binary = directory / 'profile'
    p.run([p.CLANG, '-std=c++23', '-O2', '-ffp-contract=off', '-Wno-override-module',
           '-pthread', '-DKALANDRA_KERNEL', src / 'SourceDriver.cpp', driver, ir,
           runtime, '-o', binary])
    # protocol.run intentionally returns only stdout. Retain stderr counters too.
    import subprocess
    records = []
    for depth in range(9, 15):
        command = [str(binary), '0', str(depth), '1', '1', '17']
        result = subprocess.run(command, check=True, text=True, capture_output=True, timeout=60)
        outputs = [json.loads(line) for line in result.stdout.splitlines()]
        assert len(outputs) == 3
        assert all(row['checksum'] == source.expected(0, depth) for row in outputs)
        counters = list(map(int, result.stderr.split()))
        assert len(counters) == 16 and all(value % 3 == 0 for value in counters)
        counters = [value // 3 for value in counters]
        records.append(dict(depth=depth, private_frame_entries=sum(counters),
                            base_case_frame_entries=counters[0],
                            entries_by_remaining_depth=counters, command=command))
    assert all(p.sha(path) == digest for path, digest in frozen.items()), 'profile inputs changed'
    p.evidence(args.output, dict(
        scope='Diagnostic only: volatile counters at private depth-worker entries. '
              'Two warmups and one measured invocation, divided by three. '
              'Checksums match independent references. Instrumented times are discarded.',
        original_ir_sha256=p.sha(original), instrumented_ir_sha256=p.sha(ir), records=records),
        inputs[:-2], [binary, compiler, runtime], compiler)
    for row in records:
        print(row['depth'], row['private_frame_entries'], row['base_case_frame_entries'])


if __name__ == '__main__':
    main()
