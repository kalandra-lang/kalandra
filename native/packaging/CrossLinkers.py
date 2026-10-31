"""Stage pinned host-native LLD drivers, never destination executables."""
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def stage(prefix, llvm):
    metadata = prefix / 'share/kalandra-native-tools/toolchain.json'
    info = json.loads(metadata.read_text())
    suffix = '.exe' if info['platform'].startswith('windows-') else ''
    directory = Path(os.environ.get('KALANDRA_CROSS_LLD_ROOT', str(llvm / 'bin')))
    names = ('ld.lld', 'ld64.lld', 'lld-link')
    source = next((directory / (name + suffix) for name in names
                   if (directory / (name + suffix)).is_file()), None)
    if source is None:
        raise RuntimeError('Standalone CPU packaging requires pinned host LLD; set KALANDRA_CROSS_LLD_ROOT')
    version = json.loads((ROOT / 'native/toolchain-proof/toolchain.json').read_text())['llvmVersion']
    reported = subprocess.check_output([str(source), '--version'], text=True)
    if version not in reported: raise RuntimeError('Host LLD version does not match the pinned toolchain')
    private = prefix / 'libexec/kalandra'
    private.mkdir(parents=True, exist_ok=True)
    seeds = []
    for name in names:
        target = private / (name + suffix)
        shutil.copy2(source, target)
        seeds.append((source.resolve(), target))
    info['crossLinkers'] = {kind: str(path.relative_to(prefix)).replace('\\', '/')
                            for kind, (_, path) in zip(('elf', 'macho', 'coff'), seeds)}
    metadata.write_text(json.dumps(info, indent=2) + '\n')
    return seeds
