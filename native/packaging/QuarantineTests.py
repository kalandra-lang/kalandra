"""Check quarantined, hosted-notarized disk images without removing quarantine or re-signing."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
import uuid
from StandaloneTests import qualify_prefix

def run(args):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=180)
    if p.returncode:
        raise RuntimeError((args, p.returncode, p.stdout, p.stderr))
    return p.stdout + p.stderr

def qualify(root, destination, metal_execute=False):
    results = []
    for kind in ('editor-tools', 'cpu', 'metal'):
        image = root / kind / f'kalandra-native-{kind}-standalone.dmg'
        record = json.loads(image.with_suffix('.notarization.json').read_text())
        assert hashlib.sha256(image.read_bytes()).hexdigest() == record['artifactSHA256']
        quarantine = f'0083;{int(time.time()):x};Codex;{str(uuid.uuid4()).upper()}'
        run(['/usr/bin/xattr', '-w', 'com.apple.quarantine', quarantine, image])
        staple = run(['/usr/bin/xcrun', 'stapler', 'validate', image])
        assessment = run(['/usr/sbin/spctl', '--assess', '--type', 'open', '--context', 'context:primary-signature', '--verbose=4', image])
        with tempfile.TemporaryDirectory(prefix='kalandra quarantine ') as directory:
            directory = Path(directory)
            mount = directory / 'volume'
            mount.mkdir()
            run(['/usr/bin/hdiutil', 'attach', '-readonly', '-nobrowse', '-noautoopen', '-mountpoint', mount, image])
            try:
                original, = mount.glob('kalandra-native-*')
                prefix = directory / original.name
                run(['/usr/bin/ditto', original, prefix])
            finally:
                run(['/usr/bin/hdiutil', 'detach', mount])
            listed = {}
            for line in (prefix / 'SHA256SUMS').read_text().splitlines():
                checksum, name = line.split('  ', 1)
                path = (prefix / name).resolve()
                assert path.is_relative_to(prefix.resolve()) and name not in listed
                with path.open('rb') as stream:
                    assert hashlib.file_digest(stream, 'sha256').hexdigest() == checksum, name
                listed[name] = checksum
            assert {p.relative_to(prefix).as_posix() for p in prefix.rglob('*') if p.is_file()} == listed.keys() | {'SHA256SUMS'}
            binary = prefix / 'bin' / ('kalandra-check' if kind == 'editor-tools' else 'kalandra')
            run(['/usr/bin/xattr', '-w', 'com.apple.quarantine', quarantine, binary])
            assessed = subprocess.run(['/usr/sbin/spctl', '--assess', '--type', 'execute', '--verbose=4', str(binary)], capture_output=True, text=True, timeout=180)
            binary_assessment = assessed.stdout + assessed.stderr
            assert assessed.returncode == 0 or (assessed.returncode == 3 and 'the code is valid but does not seem to be an app' in binary_assessment), binary_assessment
            run(['/usr/bin/codesign', '--verify', '--deep', '--strict', binary])
            source = directory / 'source λ.k'
            source.write_text('fn main()->int:\n  42\n')
            run([prefix / 'bin/kalandra-check', source])
            if kind == 'metal' and metal_execute:
                acceptance = directory / 'physical GPU acceptance'
                acceptance.mkdir()
                qualify_prefix(prefix.resolve(), 'metal', acceptance.resolve(), metal_execute=True)
            if kind != 'editor-tools':
                output = directory / 'program'
                run([binary, source, '-o', output])
                assert run([output]).strip() == '42'
                prefix.rename(directory / 'unavailable compiler')
                assert run([output]).strip() == '42'
            results.append({'kind': kind, 'artifactSHA256': record['artifactSHA256'], 'staple': staple.strip(), 'imageGatekeeper': assessment.strip(), 'binaryGatekeeper': binary_assessment.strip(), 'quarantineExecution': 'passed', 'gpuExecutionQualified': kind == 'metal' and metal_execute, 'notarization': record['notarization']})
        print(f'Quarantined {kind}: staple, image Gatekeeper, quarantined CLI execution and compiler independence passed', flush=True)
    destination.write_text(json.dumps(results, indent=2) + '\n')
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('signed', type=Path)
    parser.add_argument('--record', type=Path, required=True)
    parser.add_argument('--metal-execute', action='store_true')
    args = parser.parse_args()
    qualify(args.signed.resolve(), args.record.resolve(), args.metal_execute)
