"""Keep private SDK transfers independent and stop waiting on failed producers."""
from pathlib import Path
import subprocess
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

import windows_sdk_exchange as exchange


class WindowsSDKExchangeTests(unittest.TestCase):
    def environment(self):
        return patch.dict(exchange.os.environ, GITHUB_REPOSITORY='example/kalandra', GITHUB_RUN_ID='123')

    @staticmethod
    def write_download(command, **kwargs):
        path = Path(command[-1])
        path.mkdir()
        (path / 'downloaded').write_text('complete')

    def test_failed_producers_stop_waiting_without_downloading(self):
        cases = (
            ('sdk-receiver-macos-arm64', 'CPU cross-build from macos-arm64'),
            ('sdk-sealed-arm64-linux-x86_64', 'Encrypted Windows SDK arm64'),
            ('sdk-receiver-linux-arm64', 'macos-package'),
        )
        for name, producer in cases:
            with self.subTest(name=name), self.environment(), \
                    patch.object(exchange, 'api', side_effect=[{'artifacts': []}, {'jobs': [
                        dict(name=producer, status='completed', conclusion='failure')]}]), \
                    patch.object(exchange.subprocess, 'run') as download, \
                    patch.object(exchange.time, 'sleep') as sleep:
                with self.assertRaisesRegex(RuntimeError, 'SDK producer failed'):
                    exchange.wait_download(name, Path('unused'), seconds=10)
                download.assert_not_called()
                sleep.assert_not_called()

    def test_published_cipher_is_available_even_if_other_receivers_failed(self):
        name = 'sdk-sealed-x86_64-linux-arm64'
        with tempfile.TemporaryDirectory() as folder, self.environment(), patch.object(exchange, 'api', return_value={
                'artifacts': [dict(name=name, expired=False)]}) as api, \
                patch.object(exchange.subprocess, 'run', side_effect=self.write_download) as download:
            exchange.wait_download(name, Path(folder) / 'destination', seconds=10)
            self.assertEqual(api.call_count, 1)  # No dependency on the producer's final job status.
            self.assertIn(name, download.call_args.args[0])
            self.assertTrue((Path(folder) / 'destination/downloaded').is_file())

    def test_successful_producer_allows_eventual_artifact_visibility(self):
        name = 'sdk-receiver-linux-x86_64'
        with tempfile.TemporaryDirectory() as folder, self.environment(), patch.object(exchange, 'api', side_effect=[
                {'artifacts': []}, {'jobs': [dict(name='CPU cross-build from linux-x86_64',
                                                 status='completed', conclusion='success')]},
                {'artifacts': [dict(name=name, expired=False)]}]), \
                patch.object(exchange.subprocess, 'run', side_effect=self.write_download) as download, \
                patch.object(exchange.time, 'sleep') as sleep:
            exchange.wait_download(name, Path(folder) / 'destination', seconds=10)
            sleep.assert_called_once_with(45)
            download.assert_called_once()

    def test_download_visibility_and_failure_cleanup(self):
        name = 'sdk-sealed-x86_64-linux-arm64'
        for statuses in ([404, 0], [404] * 5, [403]):
            with self.subTest(statuses=statuses), tempfile.TemporaryDirectory() as folder:
                calls = []
                def download(command, **kwargs):
                    self.write_download(command)
                    status = statuses[len(calls)]
                    calls.append(status)
                    if status:
                        (Path(command[-1]) / 'partial').touch()
                        raise subprocess.CalledProcessError(1, command, stderr=f'HTTP {status}')

                destination = Path(folder) / 'sdk'
                with self.environment(), patch.object(exchange, 'api', return_value={
                        'artifacts': [dict(name=name, expired=False)]}), \
                        patch.object(exchange.subprocess, 'run', side_effect=download), \
                        patch.object(exchange.time, 'sleep') as sleep:
                    if statuses[-1]:
                        with self.assertRaises(subprocess.CalledProcessError):
                            exchange.wait_download(name, destination)
                        self.assertEqual(list(Path(folder).iterdir()), [])
                    else:
                        exchange.wait_download(name, destination)
                        self.assertEqual(list(destination.iterdir()), [destination / 'downloaded'])
                    self.assertEqual(calls, statuses)
                    self.assertEqual(sleep.call_count, len(statuses) - 1)

    def test_timeout_is_bounded(self):
        with self.environment(), patch.object(exchange, 'api') as api:
            with self.assertRaisesRegex(RuntimeError, 'Timed out'):
                exchange.wait_download('sdk-receiver-linux-arm64', Path('unused'), seconds=0)
            api.assert_not_called()

    def test_single_recipient_never_publishes_plaintext_and_cleans_up_on_failure(self):
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / 'sealed'
                existing = output / 'linux-x86_64'
                existing.mkdir(parents=True)
                (existing / 'sdk.sealed').write_bytes(b'previous recipient ciphertext')
                private_paths = []
                def prepare(path):
                    path.mkdir()
                    (path / 'sdk.lib').write_bytes(b'private test SDK bytes')
                    private_paths.append(path)
                def public_key(name, destination):
                    self.assertEqual(name, 'sdk-receiver-linux-arm64')
                    destination.mkdir()
                    (destination / 'sdk-public.pem').write_bytes(b'public key fixture')
                def seal(command, check):
                    self.assertTrue(check)
                    archive = Path(command[3])
                    self.assertTrue(archive.is_file())
                    private_paths.append(archive)
                    if fail: raise RuntimeError('encryption failed')
                    Path(command[4]).write_bytes(b'authenticated ciphertext fixture')
                arguments = SimpleNamespace(output=output, receiver='linux-arm64', tool=Path('/test/sdk-transfer'))
                with patch.object(exchange.platform, 'system', return_value='Windows'), \
                        patch.dict(exchange.os.environ, GITHUB_ACTIONS='true'), \
                        patch('windows_sdk_snapshot.prepare', side_effect=prepare), \
                        patch.object(exchange, 'wait_download', side_effect=public_key), \
                        patch.object(exchange.subprocess, 'run', side_effect=seal):
                    if fail:
                        with self.assertRaisesRegex(RuntimeError, 'encryption failed'): exchange.send(arguments)
                    else:
                        exchange.send(arguments)
                self.assertTrue(private_paths)
                self.assertTrue(all(not path.exists() for path in private_paths))
                self.assertEqual((existing / 'sdk.sealed').read_bytes(), b'previous recipient ciphertext')
                self.assertTrue(all(path.name == 'sdk.sealed' for path in output.rglob('*') if path.is_file()))


if __name__ == '__main__':
    unittest.main()
