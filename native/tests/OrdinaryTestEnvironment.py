"""Resolve the Windows sanitizer DLLs used by ordinary ABI host adapters."""
import os
from pathlib import Path
import sys


def configure(clang):
    if sys.platform != 'win32':
        return
    resources = sorted(Path(clang).resolve().parent.parent.glob('lib/clang/*/lib/windows'))
    if not resources:
        raise RuntimeError(f'Windows sanitizer runtime directory missing beside {clang}')
    os.environ['PATH'] = os.pathsep.join(map(str, resources)) + os.pathsep + os.environ.get('PATH', '')
