"""Exercise the installed CLI with uninstalled manifests and equivalent TOML layouts."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib

compiler = Path(sys.argv[1]).resolve()
base = "format=1 # λ\nproject.entry='main.k'\npackages.local='local.k'\n"
fields = "git='https://example.com/library', rev='main'"
request = '{' + fields + '}'
survivor = 'keep = ' + request
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    path = root / 'kalandra.toml'

    def call(*args, status=0):
        p = subprocess.run([compiler, *args], cwd=root, capture_output=True, timeout=20)
        assert p.returncode == status, (args, p.returncode, p.stderr)

    layouts = [
        ('[dependencies]\n' + survivor + '\n"gone" = ' + request,
         '[dependencies]\n' + survivor + '\n'),
        ("[dependencies.'gone'] # header\nrev='main'\n# unrelated\ngit='https://example.com/library'\n"
         + '[dependencies.keep]\n' + fields.replace(', ', '\n'),
         '# header\n# unrelated\n[dependencies.keep]\n' + fields.replace(', ', '\n')),
        ("dependencies.gone.git = '''https://example.com/library'''\ndependencies." + survivor
         + "\ndependencies.gone.rev = '''main''' # trailing\n",
         'dependencies.' + survivor + '\n# trailing\n'),
        ("dependencies = {gone.git='https://example.com/library', " + survivor + ", gone.rev='main'}",
         'dependencies = { ' + survivor + '}'),
        ('dependencies = { ' + survivor + ', "gone"=' + request + ' }',
         'dependencies = { ' + survivor + '}'),
        ('dependencies = {gone=' + request + '}', 'dependencies = {}'),
        ('dependencies.gone=' + request, ''),
        ('dependencies.gone.rev="""\\\nmain"""\ndependencies.gone.git=\'https://example.com/library\'', ''),
    ]
    for index, (original, expected) in enumerate(layouts):
        for newline in ['\n', '\r\n']:
            prefix = ('\ufeff' if index % 2 else '') + base
            def encoded(s):
                return (prefix + s).replace('\n', newline).encode()
            path.write_bytes(encoded(original))
            call('remove' if index % 2 else '--remove', 'gone')
            assert path.read_bytes() == encoded(expected), (index, path.read_bytes(), encoded(expected))
            assert not (root / 'kalandra.lock').exists()

    path.write_text(base + '[dependencies]\ngone=' + request, encoding='utf-8')
    original = path.read_bytes()
    for alias in ['absent', 'local']:
        call('remove', alias, status=1)
    for args in [[], ['gone', 'another'], ['--project'], ['gone', '--offline'],
                 ['gone', '--project', path, '--project', path]]:
        call('remove', *args, status=2)
    assert path.read_bytes() == original
    call('remove', '--help')
    call('remove', '--project', path, 'gone')

    # Qualify lock publication in a shipped compiler without any Git or snapshots.
    path.write_bytes((base + '[dependencies]\ngone=' + request + '\n' + survivor).encode())
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    commit = '1' * 40
    repository = 'https://example.com/library'
    identity = f'repository="{repository}", commit="{commit}"'
    roots = ', '.join(f'{{name="{alias}", rev="main", {identity}}}' for alias in ['gone', 'keep'])
    checksum = '2' * 64
    package = f'{{{identity}, digest="{checksum}", entry="main.k", dependencies=[]}}'
    lock = root / 'kalandra.lock'
    lock.write_text(f'format=2\nmanifest_digest="{digest}"\nroots=[{roots}]\npackages=[{package}]')
    if os.name == 'nt':
        # An open manifest denies replacement on Windows. Restore the old lock
        # when the second rename fails, then succeed once the reader releases it.
        manifest_bytes, lock_bytes = path.read_bytes(), lock.read_bytes()
        with path.open('rb'):
            call('remove', 'gone', status=1)
        assert (path.read_bytes(), lock.read_bytes()) == (manifest_bytes, lock_bytes)
    call('remove', 'gone')
    selected = tomllib.loads(lock.read_text())
    assert selected['manifest_digest'] == hashlib.sha256(path.read_bytes()).hexdigest()
    assert selected['roots'] == [dict(name='keep', rev='main', repository=repository, commit=commit)]
    assert selected['packages'] == [dict(repository=repository, commit=commit, digest='2' * 64,
                                        entry='main.k', dependencies=[])]

    path.write_text("format=1\n[package]\nname='lib'\nversion='0.1.0'\nentry='main.k'\n"
                    "[dependencies]\ngone=" + request, encoding='utf-8')
    lock.unlink()
    call('remove', 'gone')
    assert tomllib.loads(path.read_text())['dependencies'] == {}
