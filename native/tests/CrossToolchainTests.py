"""Invalid cross-toolchain configurations preserve all existing outputs."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
a = p.parse_args()
a.compiler = a.compiler.resolve()
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
    invalid_options = (['--toolchain'], ['--toolchain', ''], ['--toolchain', '--target', 'cpu'],
                 ['--toolchain', str(profile), '--toolchain', str(profile)],
                 ['--toolchain', str(profile), '--target', 'metal'],
                 ['--toolchain', str(profile), '--sanitize'],
                 ['--toolchain', str(profile), '--thread-sanitize'],
                 ['--platform'], ['--platform', ''], ['--platform', 'linux-armv7'],
                 ['--platform', 'macos-arm64', '--platform', 'macos-arm64'],
                 ['--platform', 'macos-arm64', '--toolchain', str(profile)],
                 ['--platform', 'macos-arm64', '--target', 'metal'],
                 ['--platform', 'macos-arm64', '--sanitize'],
                 ['--platform', 'macos-arm64', '--thread-sanitize'],
                 ['--sysroot', '/'], ['--gcc-directory', '/'], ['--library-directory', '/'],
                 ['--platform', 'linux-arm64', '--sysroot', '/', '--sysroot', '/'])
    for args in invalid_options:
        result = subprocess.run(list(map(str, [a.compiler, source, '-o', executable, *args])),
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 2, (args, result.returncode, result.stderr)
        assert executable.read_bytes() == b'executable sentinel'
    # LLVM's temporary-directory helper relocates relative prefixes into the
    # system temp folder. Exercise both ordinary output forms on every host.
    for name in ('program', 'nested output/program'):
        relative = name + ('.exe' if sys.platform == 'win32' else '')
        output = root / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run([str(a.compiler), str(source), '-O2', '-o', relative],
                                cwd=root, capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, (relative, result.stderr)
        result = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
        assert result.returncode == 0 and result.stdout == '42\n', (relative, result.stdout, result.stderr)
    assert not list(root.rglob('.kalandra-link*')), 'Staging directory leaked after publication'
print(f'{len(configs)+len(invalid_options)} invalid toolchain/ABI/mode cases rejected without changing outputs')
