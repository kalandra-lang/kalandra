"""Black-box tests of the authenticated CI transport executable."""
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='SDK transport test ') as work:
    root = Path(work)
    def run(*args, ok=True):
        result = subprocess.run([binary, *map(str, args)], capture_output=True, text=True)
        assert (result.returncode == 0) == ok, (args, result.stderr)
    private, public = root/'private', root/'public'
    run('key', private, public)
    source, sealed, result = root/'source', root/'sealed', root/'result'
    source.write_bytes(b'CI link inputs\0' * 100)
    run('seal', public, source, sealed)
    assert source.read_bytes() not in sealed.read_bytes()
    run('open', private, sealed, result)
    assert result.read_bytes() == source.read_bytes()
    result.write_bytes(b'keep existing output')
    changed = bytearray(sealed.read_bytes()); changed[-1] ^= 1
    (root/'changed').write_bytes(changed)
    run('open', private, root/'changed', result, ok=False)
    assert result.read_bytes() == b'keep existing output'
    run('key', root/'other-private', root/'other-public')
    run('open', root/'other-private', sealed, result, ok=False)
    for data in (b'', b'KALSDK1\n', b'KALSDK1\n'+b'\xff'*4):
        (root/'bad').write_bytes(data)
        run('open', private, root/'bad', result, ok=False)
        assert result.read_bytes() == b'keep existing output'
print('SDK transport: round-trip, recipient isolation, corruption and output preservation passed')
