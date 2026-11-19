import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import windows_physical as physical


class WindowsPhysicalTests(unittest.TestCase):
    def test_independent_integer_and_float_words(self):
        self.assertEqual(physical.numeric_output('i8', [-128, -1, 127], tensor=True),
                         b'[80,ff,7f]\n')
        self.assertEqual(physical.numeric_output('f64', [5], tensor=False),
                         b'4014000000000000\n')
        _, reference = physical.matrix('i8', [[1, 2, 3], [0, 2, 1]],
                                       [[2, 1], [1, 3], [0, 2]])
        self.assertEqual(reference, b'[04,0d,02,08]\n')

    def test_stdout_bytes_and_missing_resource(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            placed = root/'bundle/cuda/placed'
            placed.mkdir(parents=True)
            (placed/'program.exe').touch()
            (placed/'kernel0.ptx').write_bytes(b'kernel')
            scratch = root/'working directory λ'
            scratch.mkdir()
            binary = 'cuda/placed/program.exe'
            good = b'\x00\xc3\xa9\n'
            with patch.object(physical.subprocess, 'run') as child:
                child.return_value = subprocess.CompletedProcess([], 0, good[:-1] + b'\r\n', b'')
                self.assertTrue(physical.run_case(root/'bundle', {
                    'path': binary, 'stdout': good.hex(),
                }, 4, scratch)['passed'])
                self.assertEqual(child.call_args.kwargs['env']['KALANDRA_WORKERS'], '4')
                child.return_value = subprocess.CompletedProcess([], 1, b'', b'CUDA task failed: 1/104\r\n')
                case = {'path': binary, 'stdout': '', 'missing': 'kernel0.ptx',
                        'exit': 1, 'stderrPrefix': 'CUDA task failed:'}
                self.assertTrue(physical.run_case(root/'bundle', case, 1, scratch)['passed'])
                self.assertTrue((placed/'kernel0.ptx').is_file())
                self.assertFalse((scratch/'cuda/kernel0.ptx').exists())
                child.return_value = subprocess.CompletedProcess([], 1, good, b'CUDA task failed: 1/104\r\n')
                self.assertFalse(physical.run_case(root/'bundle', case, 1, scratch)['passed'])

    def test_runtime_probe_uses_ascii_relative_kernel_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory)/'frozen λ'
            case = {'path': 'cuda/async-program-tests.exe', 'stdoutPrefix': 'PASS',
                    'arguments': ['cuda/runtime-map/kernel0.ptx'], 'bundleArguments': [0]}
            with patch.object(physical.subprocess, 'run') as child:
                child.return_value = subprocess.CompletedProcess([], 0, b'PASS\n', b'')
                self.assertTrue(physical.run_case(bundle, case, 1, Path(directory))['passed'])
                self.assertEqual(child.call_args.args[0][1], str(Path('runtime-map/kernel0.ptx')))
                self.assertEqual(child.call_args.kwargs['cwd'], bundle/'cuda')

    def test_fixture_inventory_rejects_replaced_kernel(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            kernel = root/'vulkan/case/kernel0.spv'
            kernel.parent.mkdir(parents=True)
            kernel.write_bytes(b'original kernel')
            report = root/'frozen.json'
            physical.write_report(report, {'files': physical.inventory(root)})
            manifest = json.loads(report.read_text(encoding='utf-8'))
            physical.verify(root, manifest)
            kernel.write_bytes(b'rebuilt kernel')
            with self.assertRaisesRegex(RuntimeError, 'frozen build'):
                physical.verify(root, manifest)


if __name__ == '__main__':
    unittest.main()
