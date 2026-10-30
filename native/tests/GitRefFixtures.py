"""Local refs and package-owner pin preservation; no original repository config executes."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tomllib

parser = argparse.ArgumentParser()
for name in ['api', 'compiler', 'checker', 'root']:
    parser.add_argument('--' + name, required=True)
args = parser.parse_args()
root = Path(args.root).resolve()
if root.exists():
    shutil.rmtree(root)
root.mkdir(parents=True)
env = {**os.environ, 'GIT_CONFIG_NOSYSTEM':'1', 'GIT_CONFIG_GLOBAL':'/dev/null',
       'GIT_AUTHOR_NAME':'Fixture', 'GIT_AUTHOR_EMAIL':'fixture@example.invalid',
       'GIT_COMMITTER_NAME':'Fixture', 'GIT_COMMITTER_EMAIL':'fixture@example.invalid'}

def run(*command, ok=True):
    result = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=60)
    assert not any(marker in result.stderr for marker in [b'ERROR: AddressSanitizer', b'runtime error:', b'WARNING: ThreadSanitizer']), result.stderr
    assert (result.returncode == 0) == ok, (command, result.returncode, result.stdout, result.stderr)
    return result

def git(repo, *command):
    return run('/usr/bin/git', '-C', repo, *command).stdout.decode().strip()

def commit(repo):
    git(repo, 'add', '--all')
    git(repo, 'commit', '-m', 'fixture')
    return git(repo, 'rev-parse', 'HEAD')

def request(name, repo, revision):
    return f'{name}={{git={json.dumps(str(repo))},rev={json.dumps(revision)}}}\n'

def repository(name, text, dependencies=''):
    path = root / name
    path.mkdir()
    git(path, 'init', '--template=', '--initial-branch=main')
    (path / 'kalandra.toml').write_text("format=1\n[package]\nname='fixture'\nversion='0.1.0'\nentry='public.k'\n" + ('[dependencies]\n' + dependencies if dependencies else ''))
    (path / 'public.k').write_text(text)
    return path, commit(path)

geometry, first = repository('geometry source λ', 'export fn next(n:int64)->int64:\n  n+1\n')
git(geometry, 'tag', '-a', 'release', first, '-m', 'release')
git(geometry, 'tag', '-a', 'nested', 'release', '-m', 'nested')
git(geometry, 'tag', 'light', first)
git(geometry, 'branch', 'feature/next', first)
git(geometry, 'branch', 'collision', first)
git(geometry, 'tag', 'collision', first)
(geometry / 'public.k').write_text('export fn next(n:int64)->int64:\n  n+2\n')
second = commit(geometry)
blob = git(geometry, 'rev-parse', first + ':public.k')
git(geometry, 'tag', 'blob', blob)
git(geometry, 'tag', '-a', 'tree', first + '^{tree}', '-m', 'not a commit')
number = 0

def resolve(revision, expected=None, repo=geometry, budget='normal'):
    global number
    number += 1
    cache = root / f'probe-{number}'
    result = run(args.api, 'revision', cache, repo, revision, '-', budget, ok=expected is not None)
    assert not list(cache.glob('.kalandra-snapshot-*'))
    if expected is not None:
        assert result.stdout.splitlines()[0].decode() == expected
    else:
        assert not list(cache.iterdir())
    return result

configuration = (geometry / '.git/config').read_text()
marker = root / 'executed'
with (geometry / '.git/config').open('a') as out:
    out.write(f'\n[core]\n fsmonitor = !touch "{marker}"\n[uploadpack]\n packObjectsHook = !touch "{marker}"\n')
for rev, expected in [('main', second), ('release', first), ('nested', first), ('light', first),
                      ('feature/next', first), ('refs/heads/collision', first), ('refs/tags/collision', first)]:
    resolve(rev, expected)
assert b'ambiguous' in resolve('collision').stderr
assert not marker.exists()
(geometry / '.git/config').write_text(configuration)
for rev in ['HEAD', 'HEAD~1', 'main^{}', 'main:public.k', 'main*', '../main', 'main.lock',
            '.hidden', 'refs/remotes/origin/main', 'feature//next', first[:12], first.upper(),
            '$(touch marker)', 'branch\nother', 'missing', 'blob', 'tree']:
    resolve(rev)
# Packed annotated tags peel in the private database; loose refs override packs.
git(geometry, 'pack-refs', '--all', '--prune')
resolve('release', first)
resolve('nested', first)
resolve('main', second)
git(geometry, 'update-ref', 'refs/tags/release', second)
resolve('release', second)
git(geometry, 'update-ref', 'refs/tags/release', first)
resolve('main', budget='listing')
git(geometry, 'symbolic-ref', 'refs/heads/symbolic', 'refs/heads/main')
resolve('symbolic')
unsafe = geometry / '.git/refs/heads/unsafe'
unsafe.symlink_to(geometry / '.git/HEAD')
resolve('unsafe')
unsafe.unlink()
copy_repo = root / 'symlink-packed-refs'
shutil.copytree(geometry, copy_repo, symlinks=True)
(copy_repo / '.git/packed-refs').unlink()
(copy_repo / '.git/packed-refs').symlink_to(geometry / '.git/packed-refs')
resolve('main', repo=copy_repo)
parent, third = repository('parent', 'from "geometry" import next\nexport fn compute(n:int64)->int64:\n  next(n)\n', request('geometry', geometry, 'release'))
app = root / 'application'
app.mkdir()
manifest = app / 'kalandra.toml'
base = "format=1\n[project]\nentry='main.k'\n[dependencies]\n"
requests = request('geometry', geometry, 'main') + request('again', geometry, 'main') + request('parent', parent, third)
manifest.write_text(base + requests)
(app / 'main.k').write_text('from "geometry" import next\nfrom "parent" import compute\nfn main()->[int64] ! TaskError:\n  value=try parallel:\n    try await spawn compute(10)\n  [next(10),value]\n')
run(args.api, 'project', manifest, '-', '-', '-', 'normal')
lock = app / 'kalandra.lock'
active = lock.read_bytes()
data = tomllib.loads(active.decode())
assert data['format'] == 2 and len(data['packages']) == 3
roots = {item['name']: item for item in data['roots']}
assert roots['geometry']['rev'] == 'main' and roots['geometry']['commit'] == second
assert roots['again']['commit'] == second
parent_lock = next(item for item in data['packages'] if item['commit'] == third)
assert parent_lock['dependencies'][0]['rev'] == 'release' and parent_lock['dependencies'][0]['commit'] == first
# Both sources move, but all existing source requests stay pinned.
(geometry / 'public.k').write_text('export fn next(n:int64)->int64:\n  n+3\n')
fourth = commit(geometry)
git(geometry, 'update-ref', 'refs/tags/release', fourth)
for options in [[], ['--frozen'], ['--offline']]:
    run(args.compiler, 'install', '--project', manifest, *options)
    assert lock.read_bytes() == active
# Adding an unrelated root preserves both root and private transitive pins.
other, other_commit = repository('other', 'export fn value()->int64:\n  9\n')
manifest.write_text(base + requests + request('other', other, other_commit))
for options in [['--frozen'], ['--offline']]:
    run(args.compiler, 'install', '--project', manifest, *options, ok=False)
    assert lock.read_bytes() == active
run(args.api, 'project', manifest, '-', '-', '-', 'normal')
data = tomllib.loads(lock.read_text())
assert next(item for item in data['roots'] if item['name'] == 'geometry')['commit'] == second
assert next(item for item in data['packages'] if item['commit'] == third)['dependencies'][0]['commit'] == first
# A new alias is a new request: it can select the current branch without changing
# an older alias's pin, even though the repository and requested ref are the same.
requests = request('geometry', geometry, 'main') + request('newAlias', geometry, 'main') + request('parent', parent, third)
manifest.write_text(base + requests)
run(args.api, 'project', manifest, '-', '-', '-', 'normal')
data = tomllib.loads(lock.read_text())
roots = {item['name']:item for item in data['roots']}
assert roots['geometry']['commit'] == second and roots['newAlias']['commit'] == fourth
# Changing the request explicitly selects the new branch; existing parent pins
# stay put. A new parent commit gets its own current private dependency selection.
manifest.write_text(base + request('geometry', geometry, 'refs/heads/main') + request('parent', parent, third))
run(args.api, 'project', manifest, '-', '-', '-', 'normal')
(parent / 'kalandra.toml').write_text((parent / 'kalandra.toml').read_text().replace("version='0.1.0'", "version='0.2.0'"))
new_parent = commit(parent)
manifest.write_text(manifest.read_text() + request('newParent', parent, new_parent))
run(args.api, 'project', manifest, '-', '-', '-', 'normal')
data = tomllib.loads(lock.read_text())
assert next(item for item in data['packages'] if item['commit'] == third)['dependencies'][0]['commit'] == first
assert next(item for item in data['packages'] if item['commit'] == new_parent)['dependencies'][0]['commit'] == fourth
# Ambiguous new requests fail atomically without losing the prior lock.
before = lock.read_bytes()
original = manifest.read_text()
manifest.write_text(original + request('ambiguous', geometry, 'collision'))
assert b'ambiguous' in run(args.api, 'project', manifest, '-', '-', '-', 'normal', ok=False).stderr
assert lock.read_bytes() == before
manifest.write_text(original)
cycle_a, _ = repository('cycle-a', 'export fn unused()->int64:\n  1\n', request('b', root / 'cycle-b', 'main'))
cycle_b, _ = repository('cycle-b', 'export fn unused()->int64:\n  1\n', request('a', cycle_a, 'main'))
manifest.write_text(original + request('cycle', cycle_a, 'main'))
assert b'cycle' in run(args.api, 'project', manifest, '-', '-', '-', 'normal', ok=False).stderr
assert lock.read_bytes() == before
manifest.write_text(original)
# A branch/tag disappearing does not matter after the graph is locked.
geometry.rename(root / 'geometry unavailable')
parent.rename(root / 'parent unavailable')
for options in [[], ['--frozen'], ['--offline']]:
    run(args.compiler, 'install', '--project', manifest, *options)
    assert lock.read_bytes() == before
run(args.checker, '--project', manifest)
for flags in [[], ['-O2'], ['--sanitize'], ['--thread-sanitize']]:
    program = app / 'program'
    run(args.compiler, '--project', manifest, '-o', program, *flags)
    for workers in [1,4]:
        env['KALANDRA_WORKERS'] = str(workers)
        assert run(program).stdout == b'[13, 11]\n'
print('Git refs: loose/packed refs, annotated tags, ambiguity, input bounds, stable per-owner pins, moved refs, rollback and offline workers passed')
