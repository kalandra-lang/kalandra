"""List or run targeted CTest groups without changing their test contracts.

Labels describe test families, not an automatic source-dependency analysis.
Default execution remains serial; parallel package compilation is scheduled
separately in the Windows qualification harness.
"""
import argparse
import json
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
    args = parser.parse_args(argv)
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
    # Do not inherit unbounded CTEST_PARALLEL_LEVEL: several legacy suites
    # intentionally share generated output paths. Their scheduling is unchanged.
    return subprocess.run([*command, '--parallel', '1', '--output-on-failure', '--no-tests=error', *extra]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
