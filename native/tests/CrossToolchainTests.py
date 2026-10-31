"""Invalid cross-toolchain configurations preserve all existing outputs."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='kalandra toolchain validation ') as folder:
    root = Path(folder)
    source = root / 'source.k'
    source.write_text('fn main()->int:\n  42\n')
    executable, ir = root / 'output', root / 'output.ll'
    executable.write_bytes(b'executable sentinel')
    ir.write_bytes(b'IR sentinel')
    profile = root / 'profile.json'
    configs = [[], {}, {'format': 2}, {'format': 1, 'surprise': 1},
               *[{'format': 1, 'target': target} for target in (
                   'armv7-linux-gnueabihf', 'arm64ec-pc-windows-msvc', 'arm64e-apple-macosx26.0',
                   'aarch64-unknown-linux-musl', 'x86_64-w64-windows-gnu',
                   'arm64-apple-macosx15.0', 'arm64-apple-darwin26.0', 'x86_64-apple-macosx26.0',
                   'aarch64-unknown-linux-gnu\0bad', 'aarch64-unknown-linux-gnu')]]
    for config in configs:
        profile.write_text(json.dumps(config))
        result = subprocess.run(list(map(str, [a.compiler, source, '--toolchain', profile,
            '-o', executable, '--emit-llvm', ir])), capture_output=True, text=True, timeout=30)
        assert result.returncode == 2, (config, result.returncode, result.stderr)
        assert executable.read_bytes() == b'executable sentinel' and ir.read_bytes() == b'IR sentinel'
    for args in (['--toolchain'], ['--toolchain', ''], ['--toolchain', '--target', 'cpu'],
                 ['--toolchain', str(profile), '--toolchain', str(profile)],
                 ['--toolchain', str(profile), '--target', 'metal'],
                 ['--toolchain', str(profile), '--sanitize'],
                 ['--toolchain', str(profile), '--thread-sanitize']):
        result = subprocess.run(list(map(str, [a.compiler, source, '-o', executable, *args])),
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 2, (args, result.returncode, result.stderr)
        assert executable.read_bytes() == b'executable sentinel'
print(f'{len(configs)+7} invalid toolchain/ABI/mode cases rejected without changing outputs')
