"""Physical-GPU primitive checks with every build-tree built-in shader hidden.

Run only when other tests/builds are idle. Files are restored even on test failure.
This proves runtime resource independence, not installed compiler packaging.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True, help='Vulkan runtime binary directory')
parser.add_argument('--device', type=int, default=0)
args = parser.parse_args()
build = args.build.resolve()
resources = [build / 'add.spv', build / 'mul_mm.spv', *sorted((build / 'transforms').glob('*.spv'))]
assert len(resources) == 14 and all(path.is_file() and not path.is_symlink() for path in resources)
with tempfile.TemporaryDirectory(prefix='kalandra-vulkan-relocation-') as directory:
    root = Path(directory)
    binary = root / 'primitive-proof'
    shutil.copy2(build / 'kalandra-vulkan-tests', binary)
    # Keep originals on the same filesystem and preserve them if restoration
    # fails. A TemporaryDirectory cleanup must never erase the only backup.
    backups = Path(tempfile.mkdtemp(prefix='hidden-builtins-', dir=build))
    moved = []
    try:
        for index, path in enumerate(resources):
            backup = backups / f'resource-{index}'
            path.rename(backup)
            moved.append((path, backup))
        subprocess.run([binary, str(args.device)], cwd=root, check=True, timeout=180)
        assert all(not path.exists() for path in resources)
    finally:
        conflicts = []
        for path, backup in reversed(moved):
            if path.exists():
                conflicts.append((path, backup))
            else:
                backup.rename(path)
        if conflicts:
            raise RuntimeError(f'concurrent build recreated resources; originals retained: {conflicts}')
        backups.rmdir()
print('PASS: relocated executable uses embedded shaders with all 14 build resources absent')
