#!/usr/bin/env python3
"""Print a compute-only Vulkan manifest derived from the installed Linux NVIDIA ICD."""
import argparse
import json
from pathlib import Path


def compute_manifest(source):
    manifest = json.loads(source.read_text())
    library = Path(manifest['ICD']['library_path'])
    if library.name not in ('libGLX_nvidia.so.0', 'libEGL_nvidia.so.0'):
        raise ValueError(f'{source}: expected the NVIDIA GLX or EGL ICD')
    # Bare sonames use the system library search path. Paths with a directory
    # are relative to the original manifest, which the loader will no longer see.
    egl = library.with_name('libEGL_nvidia.so.0')
    if not egl.is_absolute() and '/' in manifest['ICD']['library_path']:
        egl = (source.parent / egl).resolve()
    manifest['ICD']['library_path'] = str(egl)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('icd', type=Path, nargs='?',
                        default=Path('/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json'))
    args = parser.parse_args()
    try:
        manifest = compute_manifest(args.icd)
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(1, f'{parser.prog}: {error}\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
