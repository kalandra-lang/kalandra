import json
from pathlib import Path
import sys
import tempfile
import unittest

from CompileJobs import compile_plan, run_compile_jobs


class CompileJobsTests(unittest.TestCase):
    def test_complete_original_corpus_for_both_backends(self):
        repo, compiler, root = Path('/repo'), Path('/compiler'), Path('/output')
        common = {'host-arrays': 'HostArrayTests.py', 'scalar-control': 'ScalarControlTests.py',
                  'device-arrays': 'DeviceArrayTests.py', 'device-array-entry': 'DeviceArrayEntryTests.py',
                  'device-array-results': 'DeviceArrayResultTests.py', 'device-array-main': 'DeviceArrayMainTests.py',
                  'host-aggregates': 'HostAggregateTests.py', 'device-bool': 'DeviceBoolTests.py'}
        for backend in ('cuda', 'vulkan'):
            plan = dict(compile_plan(backend, repo, compiler, root))
            self.assertEqual(len(plan), 13)
            self.assertEqual(set(plan), set(common) | {'applications', 'console-applications', 'filesystem-applications', 'parse-applications', 'wide-arguments' if backend=='cuda' else 'shared-capabilities'})
            self.assertEqual(plan['console-applications'], list(map(str, [sys.executable,
                repo/'native/tests/GpuConsoleSourceTests.py', '--compiler', compiler,
                '--target', backend, '--variant', 'optimized', '--compile-only',
                '--output', root/'console-applications'])))
            self.assertEqual(plan['filesystem-applications'], list(map(str, [sys.executable,
                repo/'native/tests/FileSystemSourceTests.py', '--compiler', compiler,
                '--target', backend, '--variant', 'optimized', '--compile-only',
                '--output', root/'filesystem-applications'])))
            self.assertEqual(plan['parse-applications'], list(map(str, [sys.executable,
                repo/'native/tests/ParseSourceTests.py', '--compiler', compiler,
                '--target', backend, '--variant', 'optimized', '--compile-only',
                '--output', root/'parse-applications'])))
            for name, script in common.items():
                self.assertEqual(plan[name], list(map(str, [sys.executable, repo/'native/gpu-source'/script,
                    '--compiler', compiler, '--backend', backend, '--variant', 'optimized', '--compile-only', '--root', root/name])))
            self.assertIn('--compile-only', plan['applications'])
            if backend == 'cuda':
                self.assertEqual(plan['wide-arguments'], list(map(str, [compiler, '--test-entry', 'main', '--target', 'cuda', '--arithmetic', 'software',
                    repo/'native/metal-source/indirect-arguments.k', '-O2', '-o', root/'wide-arguments'])))
            else:
                self.assertIn('--compile-only', plan['shared-capabilities'])
            # No case filter, reduced policy set, quick mode or shard is introduced.
            self.assertFalse(any(flag in cmd for cmd in plan.values() for flag in ('--case', '--quick', '--shard-count')))

    def test_actual_serial_and_parallel_jobs_preserve_all_commands(self):
        for workers in (1, 2):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                code = 'import time,json; print(json.dumps({"start":time.monotonic()}),flush=True); time.sleep(.2); print(json.dumps({"end":time.monotonic()}))'
                plan = [(str(n), [sys.executable, '-c', code]) for n in range(4)]
                report = run_compile_jobs(plan, root, workers)
                self.assertEqual([r['command'] for r in report['suites']], [cmd for _, cmd in plan])
                self.assertEqual([r['suite'] for r in report['suites']], ['0','1','2','3'])
                events = []
                for r in report['suites']:
                    first,last = Path(r['log']).read_text().splitlines()
                    events += [(json.loads(first)['start'],1), (json.loads(last)['end'],-1)]
                active = peak = 0
                for _, delta in sorted(events):
                    active += delta
                    peak = max(peak, active)
                self.assertEqual(active, 0)
                self.assertEqual(peak, workers)
                self.assertEqual(json.loads((root/'compile-jobs.json').read_text())['jobs'], workers)

    def test_failure_waits_for_and_reports_every_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            plan = [('bad', [sys.executable, '-c', 'raise SystemExit(7)']),
                    ('good', [sys.executable, '-c', 'import time; time.sleep(.1); print("finished")']),
                    ('late', [sys.executable, '-c', 'print("late")'])]
            with self.assertRaisesRegex(RuntimeError, 'bad'):
                run_compile_jobs(plan, root, 2)
            rows = json.loads((root/'compile-jobs.json').read_text())['suites']
            self.assertEqual([r['returncode'] for r in rows], [7,0,0])
            self.assertIn('finished', Path(rows[1]['log']).read_text())

    def test_timeout_and_launch_failure_are_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(RuntimeError):
                run_compile_jobs([('timeout', [sys.executable, '-c', 'import time; time.sleep(30)']),
                                  ('missing', [str(root/'absent')])], root, 2, timeout=.05)
            rows = json.loads((root/'compile-jobs.json').read_text())['suites']
            self.assertTrue(all(r['returncode'] is None and r['error'] for r in rows))

    def test_invalid_plan_never_starts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for plan in ([], [('x', []), ('x', [])], [('../x', [])], [('..', [])], [('a\\b', [])]):
                with self.assertRaises(ValueError):
                    run_compile_jobs(plan, root)
            with self.assertRaises(ValueError):
                run_compile_jobs([('x', [])], root, 3)
            self.assertFalse((root/'compile-logs').exists())


if __name__ == '__main__':
    unittest.main()
