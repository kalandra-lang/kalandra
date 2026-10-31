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


def qualified_variants(variants, compiler):
    if sys.platform != 'win32':
        return variants
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'packaging'))
    from WindowsPlatform import has_sanitizers
    if has_sanitizers(compiler):
        return variants
    omitted = [variant[0] for variant in variants if 'sanitiz' in variant[0]]
    if omitted:
        print('Windows ARM64 pinned SDK has no sanitizer runtimes; excluded modes: ' + ', '.join(omitted), flush=True)
    return [variant for variant in variants if 'sanitiz' not in variant[0]]
