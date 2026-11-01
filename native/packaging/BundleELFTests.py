"""Exercise read-only relocation admission with real ELF dynamic entries."""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import tempfile
from BundleELF import audit, search_paths

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cxx', required=True)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='kalandra-elf-audit-') as directory:
    root = Path(directory)
    prefix = root/'prefix'
    (prefix/'bin').mkdir(parents=True)
    (prefix/'share/kalandra-native-tools').mkdir(parents=True)
    target = {'x86_64': 'linux-x86_64', 'aarch64': 'linux-arm64'}[platform.machine()]
    (prefix/'share/kalandra-native-tools/toolchain.json').write_text(json.dumps({'platform': target}))
    source = root/'probe.cpp'
    source.write_text('int main() { return 0; }\n')
    binary = prefix/'bin/probe'
    for tag in ('--enable-new-dtags', '--disable-new-dtags'):
        for rpath, allowed in [(None, True), ('$ORIGIN/../lib', True),
                               (str(root), False), ('$ORIGIN/../../outside', False),
                               ('$ORIGINevil', False), ('$ORIGIN:', False)]:
            flags = [] if rpath is None else ['-Wl,'+tag, '-Wl,-rpath,'+rpath]
            subprocess.run([a.cxx, source, *flags, '-o', binary], check=True)
            assert search_paths(binary) == ([] if rpath is None else [rpath])
            try:
                audit(prefix)
            except RuntimeError as error:
                assert not allowed and 'Unsafe ELF search path:' in str(error), (rpath, error)
            else:
                assert allowed, rpath
print('PASS: real ELF RPATH/RUNPATH admission; no mutation tool required', flush=True)
