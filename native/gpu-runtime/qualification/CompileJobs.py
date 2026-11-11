"""Bounded scheduling for independent, compile-only Windows package suites.

The caller must finish this phase before hiding or replacing package resources.
Every suite retains its original command, timeout, assertions and output root.
"""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys
import time


def compile_plan(backend, repo, compiler, root):
    if backend == 'cuda':
        jobs = [
            ('applications', [sys.executable, repo/'native/cuda-source/qualification/applications.py',
              '--build', root, '--compiler', compiler, '--output', root/'applications', '--compile-only']),
            ('wide-arguments', [compiler, '--test-entry', 'main', '--target', 'cuda', '--arithmetic', 'software',
              repo/'native/metal-source/indirect-arguments.k', '-O2', '-o', root/'wide-arguments']),
        ]
    elif backend == 'vulkan':
        jobs = [
            ('applications', [sys.executable, repo/'native/vulkan/qualification/public_cli.py',
              '--compiler', compiler, '--output', root/'applications', '--compile-only']),
            ('shared-capabilities', [sys.executable, repo/'native/vulkan/qualification/shared_capabilities.py',
              '--backend', 'vulkan', '--compiler', compiler, '--output', root/'shared-capabilities', '--compile-only']),
        ]
    else:
        raise ValueError('unknown backend')
    jobs.append(('console-applications', [sys.executable, repo/'native/tests/GpuConsoleSourceTests.py',
                 '--compiler', compiler, '--target', backend, '--variant', 'optimized',
                 '--compile-only', '--output', root/'console-applications']))
    jobs.append(('filesystem-applications', [sys.executable, repo/'native/tests/FileSystemSourceTests.py',
                 '--compiler', compiler, '--target', backend, '--variant', 'optimized',
                 '--compile-only', '--output', root/'filesystem-applications']))
    jobs.append(('parse-applications', [sys.executable, repo/'native/tests/ParseSourceTests.py',
                 '--compiler', compiler, '--target', backend, '--variant', 'optimized',
                 '--compile-only', '--output', root/'parse-applications']))
    jobs.append(('path-applications', [sys.executable, repo/'native/tests/PathSourceTests.py',
                 '--compiler', compiler, '--target', backend, '--variant', 'optimized',
                 '--compile-only', '--output', root/'path-applications']))
    jobs.append(('environment-applications', [sys.executable,
                 repo/'native/tests/EnvironmentSourceTests.py', '--compiler', compiler,
                 '--target', backend, '--variant', 'optimized', '--compile-only',
                 '--output', root/'environment-applications']))
    jobs.append(('time-applications', [sys.executable,
                 repo/'native/tests/TimeSourceTests.py', '--compiler', compiler,
                 '--target', backend, '--variant', 'optimized', '--compile-only',
                 '--output', root/'time-applications']))
    for name, script in (
        ('host-arrays', 'HostArrayTests.py'), ('scalar-control', 'ScalarControlTests.py'),
        ('device-arrays', 'DeviceArrayTests.py'), ('device-array-entry', 'DeviceArrayEntryTests.py'),
        ('device-array-results', 'DeviceArrayResultTests.py'), ('device-array-main', 'DeviceArrayMainTests.py'),
        ('host-aggregates', 'HostAggregateTests.py'), ('device-bool', 'DeviceBoolTests.py'),
    ):
        jobs.append((name, [sys.executable, repo/'native/gpu-source'/script, '--compiler', compiler,
                     '--backend', backend, '--variant', 'optimized', '--compile-only', '--root', root/name]))
    return [(name, list(map(str, command))) for name, command in jobs]


def run_compile_jobs(plan, root, jobs=1, timeout=1800):
    if jobs not in (1, 2):
        raise ValueError('compile jobs must be 1 or 2')
    names = [name for name, _ in plan]
    if not names or len(set(names)) != len(names) or any(
        not name or Path(name).name != name or name in ('.', '..') or '\\' in name for name in names
    ):
        raise ValueError('expected nonempty, unique suite names without path components')
    logs = root/'compile-logs'
    logs.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()

    def execute(item):
        name, command = item
        begin = time.monotonic()
        result = {'suite': name, 'command': command, 'log': str(logs/(name+'.log'))}
        with (logs/(name+'.log')).open('w', encoding='utf-8') as log:
            try:
                child = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
                result['returncode'] = child.returncode
            except (OSError, subprocess.TimeoutExpired) as error:
                result['returncode'] = None
                result['error'] = str(error)
        result['seconds'] = time.monotonic()-begin
        print(f"{'PASS' if result['returncode'] == 0 else 'FAIL'}: {name} ({result['seconds']:.1f}s)", flush=True)
        return result

    # Results are restored to corpus order, regardless of scheduling.
    # Wait for ALL suites even after a failure; never hide later failures or leave
    # a compiler reading package resources when the caller starts removal probes.
    # Start the largest existing corpora first so the 143-case aggregate suite
    # does not become a serial tail. Reporting remains in original corpus order.
    priority = {'host-aggregates': 0, 'applications': 1, 'device-arrays': 2}
    scheduled = sorted(plan, key=lambda item: priority.get(item[0], 3)) if jobs > 1 else plan
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        by_name = {result['suite']: result for result in executor.map(execute, scheduled)}
    results = [by_name[name] for name in names]
    report = {'format': 1, 'jobs': jobs, 'seconds': time.monotonic()-started, 'suites': results}
    (root/'compile-jobs.json').write_text(json.dumps(report, indent=2)+'\n')
    failures = [r for r in results if r['returncode'] != 0]
    if failures:
        for result in failures:
            print(Path(result['log']).read_text(encoding='utf-8', errors='replace')[-16000:], file=sys.stderr)
        raise RuntimeError('failed compile suites: '+', '.join(r['suite'] for r in failures))
    return report
