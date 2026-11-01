"""Executable-relative resource lookup through PATH, symlinks and Unicode."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('program', type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='kalandra-path-') as directory:
    root = Path(directory).resolve()
    destination = root / 'relocated space café λ'
    destination.mkdir()
    executable = destination / args.program.name
    shutil.copy2(args.program.resolve(), executable)
    expected = str(destination)
    subprocess.run([executable, expected], cwd=root, check=True)
    subprocess.run([str(executable.relative_to(root)), expected], cwd=root, check=True)
    subprocess.run([executable.name, expected], cwd=root, check=True,
                   env=os.environ | {'PATH': str(destination) + os.pathsep + os.environ.get('PATH', '')})
    link = root / executable.name
    link.symlink_to(executable)
    subprocess.run([link, expected], cwd=root, check=True)
print('PASS: executable directory after Unicode/space relocation, relative/PATH/symlink launches')
