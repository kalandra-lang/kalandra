"""List or run targeted CTest groups without changing their test contracts.

Labels describe test families, not an automatic source-dependency analysis.
Execution runs in parallel by default using isolated per-test output paths and
explicit CTest RESOURCE_LOCK declarations for GPU and saturation suites.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

FEATURES = ('full', 'frontend', 'tooling', 'control', 'tasks', 'numerics', 'gpu', 'packaging', 'runtime', 'io')
VARIANTS = ('default', 'debug', 'optimized', 'sanitized', 'thread-sanitized')


def selection_args(features, variants, regex):
    result = []
    for prefix, values in (('feature', features), ('variant', variants)):
        if values:
            result += ['-L', '^'+prefix+'-('+'|'.join(sorted(set(values)))+')$']
    if regex:
        result += ['-R', regex]
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path('build/native'))
    parser.add_argument('--config', help='Build configuration for multi-config generators, e.g. Debug')
    parser.add_argument('--feature', choices=FEATURES, action='append', default=[])
    parser.add_argument('--variant', choices=VARIANTS, action='append', default=[])
    parser.add_argument('--match', help='Further restrict test names with a CTest regular expression')
    parser.add_argument('--list', action='store_true', help='Inspect the exact selection without executing')
    parser.add_argument('--report', type=Path, help='Write the selected names and original CTest commands')
    parser.add_argument('--junit', type=Path)
    parser.add_argument('-j', '--jobs', type=int, default=min(os.cpu_count() or 4, 8),
                        help='Parallel CTest worker count (default: min(cpu_count, 8))')
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error('--jobs must be at least 1')
    if not (args.build_dir/'CTestTestfile.cmake').is_file():
        parser.error('configure a BUILD_TESTING=ON build directory first')
    command = ['ctest', '--test-dir', str(args.build_dir),
               *(['-C', args.config] if args.config else []),
               *selection_args(args.feature, args.variant, args.match)]
    discovered = subprocess.run([*command, '--show-only=json-v1'], capture_output=True, text=True)
    if discovered.returncode:
        sys.stderr.write(discovered.stderr)
        return discovered.returncode
    tests = json.loads(discovered.stdout)['tests']
    if not tests:
        parser.error('selection matched no tests; nothing was run (multi-config builds may need --config)')
    if any('feature-full' not in next((p['value'] for p in t['properties'] if p['name']=='LABELS'), []) for t in tests):
        parser.error('build lacks current test labels; rerun CMake configuration')
    if args.report:
        args.report.write_text(json.dumps({'selection': command, 'tests': tests}, indent=2)+'\n')
    print(f'Selected {len(tests)} tests:', flush=True)
    if args.list:
        print('\n'.join(t['name'] for t in tests))
        return 0
    extra = ['--output-junit', str(args.junit)] if args.junit else []
    return subprocess.run([*command, '--parallel', str(args.jobs), '--output-on-failure', '--no-tests=error', *extra]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
