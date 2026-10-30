"""Portable exact-commit lock graphs, strict parsing, and explicit installation modes."""
import argparse
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import tomllib

parser = argparse.ArgumentParser()
for option in ['api', 'compiler', 'checker', 'root']:
    parser.add_argument('--' + option, required=True)
args = parser.parse_args()
root = Path(args.root).resolve()
if root.exists():
    shutil.rmtree(root)
root.mkdir(parents=True)
env = {**os.environ, 'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': '/dev/null',
       'GIT_AUTHOR_NAME': 'Fixture', 'GIT_AUTHOR_EMAIL': 'fixture@example.invalid',
       'GIT_COMMITTER_NAME': 'Fixture', 'GIT_COMMITTER_EMAIL': 'fixture@example.invalid'}

def run(*command, ok=True):
    result = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=60)
    assert not any(marker in result.stderr for marker in [b'ERROR: AddressSanitizer', b'runtime error:', b'WARNING: ThreadSanitizer']), result.stderr
    assert (result.returncode == 0) == ok, (command, result.returncode, result.stdout, result.stderr)
    return result

def git(repository, *command):
    return run('/usr/bin/git', '-C', repository, *command).stdout.decode().strip()

def commit(repository):
    git(repository, 'add', '--all')
    git(repository, 'commit', '-m', 'fixture')
    return git(repository, 'rev-parse', 'HEAD')

def repository(name, source, dependencies=''):
    path = root / name
    path.mkdir()
    git(path, 'init', '--template=', '--initial-branch=main')
    (path / 'kalandra.toml').write_text("format=1\n[package]\nname='fixture'\nversion='0.1.0'\nentry='public.k'\n" + ('[dependencies]\n' + dependencies if dependencies else ''))
    (path / 'public.k').write_text(source)
    return path, commit(path)

def request(name, source, revision):
    return f'{name}={{git={json.dumps(str(source))},rev={json.dumps(revision)}}}\n'

geometry, first = repository('geometry', 'export fn next(n:int64)->int64:\n  n+1\n')
(geometry / 'public.k').write_text('export fn next(n:int64)->int64:\n  n+2\n')
second = commit(geometry)
parent, third = repository('parent', 'from "geometry" import next\nexport fn compute(n:int64)->int64:\n  next(n)\n', request('geometry', geometry, first))
configuration = "format=1\n[project]\nentry='main.k'\n[dependencies]\n" + request('geometry', geometry, second) + request('parent', parent, third)
source = 'from "geometry" import next\nfrom "parent" import compute\nfn main()->[int64] ! TaskError:\n  old=try parallel:\n    try await spawn compute(10)\n  [next(10),old]\n'

def application(name, with_lock=None):
    path = root / name
    path.mkdir()
    (path / 'kalandra.toml').write_text(configuration)
    (path / 'main.k').write_text(source)
    if with_lock is not None:
        (path / 'kalandra.lock').write_bytes(with_lock)
    return path

app = application('application')
manifest = app / 'kalandra.toml'
lock = app / 'kalandra.lock'
for option in ['--offline', '--frozen']:
    run(args.compiler, 'install', '--project', manifest, option, ok=False)
assert not (app / '.kalandra').exists()
run(args.compiler, 'install', '--project', manifest)
active = lock.read_bytes()
data = tomllib.loads(active.decode())
assert data['format'] == 2 and len(data['packages']) == 3 and len(data['roots']) == 2
assert b'.kalandra' not in active and str(app).encode() not in active
assert all(not Path(item['entry']).is_absolute() for item in data['packages'])
assert not (app / '.kalandra/installed.toml').exists()
canonical = run(args.api, lock, 64, 32, 1024 * 1024).stdout
assert canonical == active
run(args.compiler, 'install', '--project', manifest)
assert lock.read_bytes() == active
# A source lock is sufficient for reinstalling a fresh cache. Frozen mode does
# not touch formatting/comments, and offline cannot bootstrap missing snapshots.
formatted = active + b'\n# user comment\n'
fresh = application('fresh-checkout', formatted)
fresh_manifest = fresh / 'kalandra.toml'
run(args.checker, '--project', fresh_manifest, ok=False)
run(args.compiler, 'install', '--project', fresh_manifest, '--offline', ok=False)
assert not (fresh / '.kalandra').exists()
run(args.compiler, 'install', '--frozen', '--project', fresh_manifest)
assert (fresh / 'kalandra.lock').read_bytes() == formatted
run(args.checker, '--project', fresh_manifest)
run(args.compiler, '--project', fresh_manifest, '-o', fresh / 'program')
assert run(fresh / 'program').stdout == b'[12, 11]\n'
# Both non-mutating modes preserve the lock's inode, bytes and modification time.
lock.write_bytes(formatted)
identity = (lock.stat().st_ino, lock.stat().st_mtime_ns)
for options in [['--frozen'], ['--offline'], ['--frozen', '--offline']]:
    run(args.compiler, 'install', '--project', manifest, *options)
    assert lock.read_bytes() == formatted
    assert (lock.stat().st_ino, lock.stat().st_mtime_ns) == identity
lock.write_bytes(active)
# Locks are strict untrusted input, including their graph edges and entry paths.
def inline(value):
    if isinstance(value, dict):
        return '{' + ','.join(key + '=' + inline(item) for key, item in value.items()) + '}'
    if isinstance(value, list):
        return '[' + ','.join(map(inline, value)) + ']'
    return json.dumps(value, ensure_ascii=False)

def encode(value):
    return ('\n'.join(key + '=' + inline(item) for key, item in value.items()) + '\n').encode()

invalid = []
def case(change):
    value = copy.deepcopy(data)
    change(value)
    invalid.append(value)
case(lambda x: x.update(format=3))
case(lambda x: x.update(format=1))
case(lambda x: x.update(extra='unknown'))
case(lambda x: x.update(manifest_digest='x' * 64))
case(lambda x: x['roots'].append(x['roots'][0]))
case(lambda x: x['packages'].append(x['packages'][0]))
case(lambda x: x['packages'][0].update(extra=True))
case(lambda x: x['packages'][0].update(commit='main'))
case(lambda x: x['packages'][0].update(repository='https://user:secret@example.com/repo'))
case(lambda x: x['roots'][0].update(commit='f' * 40))
case(lambda x: x['roots'][0].update(rev='main', commit='f' * 40))
case(lambda x: x['roots'][0].update(rev='HEAD'))
case(lambda x: x['packages'].append({**x['packages'][0], 'repository':'https://example.com/unreachable.git'}))
for entry in ['/tmp/escape.k', 'C:/escape.k', '../escape.k', 'a/../public.k', './public.k', 'public.cpp', 'a\\public.k']:
    case(lambda x, entry=entry: x['packages'][0].update(entry=entry))
def cycle(value):
    item = value['packages'][0]
    item['dependencies'] = [{'name':'self', 'repository':item['repository'], 'rev':item['commit'], 'commit':item['commit']}]
case(cycle)
for number, value in enumerate(invalid):
    candidate = root / f'invalid-{number}.lock'
    candidate.write_bytes(encode(value))
    run(args.api, candidate, 64, 32, 1024 * 1024, ok=False)
    lock.write_bytes(candidate.read_bytes())
    before = lock.read_bytes()
    run(args.checker, '--project', manifest, ok=False)
    run(args.compiler, 'install', '--project', manifest, ok=False)
    assert lock.read_bytes() == before
lock.write_bytes(active)
for packages, depth, size in [(0,32,1024*1024), (64,1,1024*1024), (64,32,1)]:
    run(args.api, lock, packages, depth, size, ok=False)
# Valid-looking records cannot silently change source integrity or public entries.
for change in [lambda x: x['packages'][0].update(digest='0' * 64),
               lambda x: x['packages'][0].update(entry='different.k'),
               lambda x: x['roots'][0].update(name='different')]:
    value = copy.deepcopy(data)
    change(value)
    candidate = encode(value)
    lock.write_bytes(candidate)
    run(args.api, lock, 64, 32, 1024 * 1024)
    run(args.checker, '--project', manifest, ok=False)
    for options in [[], ['--frozen'], ['--offline']]:
        run(args.compiler, 'install', '--project', manifest, *options, ok=False)
        assert lock.read_bytes() == candidate
lock.write_bytes(active)
# A stale manifest is rejected by builds and both non-mutating modes.
manifest.write_text(configuration + '\n# edited\n')
for options in [['--frozen'], ['--offline']]:
    run(args.compiler, 'install', '--project', manifest, *options, ok=False)
    assert lock.read_bytes() == active
run(args.checker, '--project', manifest, ok=False)
run(args.compiler, 'install', '--project', manifest)
assert lock.read_bytes() != active
manifest.write_text(configuration)
run(args.compiler, 'install', '--project', manifest)
assert lock.read_bytes() == active
# Observe staging and edit/create a public lock during installation. The installer
# must preserve the external edit and never activate over it.
for existing in [False, True]:
    changing = application('changing-lock-' + str(existing), active if existing else None)
    changing_lock = changing / 'kalandra.lock'
    process = subprocess.Popen([args.compiler, 'install', '--project', str(changing / 'kalandra.toml')],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.monotonic() + 30
    while process.poll() is None and not list((changing / '.kalandra/snapshots').glob('.kalandra-snapshot-*')):
        assert time.monotonic() < deadline
        time.sleep(0.001)
    assert process.poll() is None, 'installation completed before staging was observed'
    external = active + b'\n# concurrent lock change\n'
    changing_lock.write_bytes(external)
    out, error = process.communicate(timeout=60)
    assert process.returncode != 0 and b'kalandra.lock' in error, (out, error)
    assert changing_lock.read_bytes() == external
    assert not list((changing / '.kalandra').glob('.activate-*'))
# The lock namespace is reserved even without a cache or managed dependencies.
plain = root / 'plain'
plain.mkdir()
(plain / 'main.k').write_text('fn main()->int64:\n  42\n')
(plain / 'kalandra.toml').write_text("format=1\n[project]\nentry='main.k'\n")
run(args.compiler, '--project', plain / 'kalandra.toml', '-o', plain / 'kalandra.lock', ok=False)
assert not (plain / 'kalandra.lock').exists()
alias = plain / 'dangling-output'
alias.symlink_to('kalandra.lock')
chain = plain / 'dangling-chain'
chain.symlink_to('dangling-output')
for target in [alias, chain]:
    run(args.compiler, '--project', plain / 'kalandra.toml', '-o', target, ok=False)
    assert target.is_symlink() and not (plain / 'kalandra.lock').exists()
(plain / 'kalandra.lock').symlink_to('other-output')
run(args.compiler, '--project', plain / 'kalandra.toml', '-o', plain / 'other-output', ok=False)
assert not (plain / 'other-output').exists()
(plain / 'kalandra.lock').unlink()
run(args.compiler, 'install', '--project', plain / 'kalandra.toml')
assert tomllib.loads((plain / 'kalandra.lock').read_text())['packages'] == []
run(args.compiler, 'install', '--project', plain / 'kalandra.toml', '--offline')
# Original repositories disappear; cache-only modes and builds remain usable.
geometry.rename(root / 'geometry-unavailable')
parent.rename(root / 'parent-unavailable')
for options in [['--frozen'], ['--offline']]:
    run(args.compiler, 'install', '--project', manifest, *options)
    assert lock.read_bytes() == active
run(args.compiler, '--project', manifest, '-o', app / 'program')
assert run(app / 'program').stdout == b'[12, 11]\n'
print('Package locks: portable graph reinstall, frozen/offline contracts, strict parser/graph budgets, immutable pins, concurrent activation and output reservation passed')
