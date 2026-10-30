"""Explicit root updates preserve unrelated and unchanged-owner pins; all Git is local."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import tomllib

parser = argparse.ArgumentParser()
for name in ['compiler', 'checker', 'root']:
    parser.add_argument('--' + name, required=True)
parser.add_argument('--api')
args = parser.parse_args()
root = Path(args.root).resolve()
if root.exists():
    shutil.rmtree(root)
root.mkdir(parents=True)
env = {**os.environ, 'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': '/dev/null',
       'GIT_CONFIG_SYSTEM': '/dev/null', 'GIT_CONFIG_COUNT': '0',
       'GIT_AUTHOR_NAME': 'Fixture', 'GIT_AUTHOR_EMAIL': 'fixture@example.invalid',
       'GIT_COMMITTER_NAME': 'Fixture', 'GIT_COMMITTER_EMAIL': 'fixture@example.invalid'}


def run(*command, ok=True):
    result = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=60)
    assert not any(marker in result.stderr for marker in
                   [b'ERROR: AddressSanitizer', b'runtime error:', b'WARNING: ThreadSanitizer']), result.stderr
    assert (result.returncode == 0) == ok, (command, result.returncode, result.stdout, result.stderr)
    return result


def git(repo, *command):
    return run('/usr/bin/git', '-C', repo, *command).stdout.decode().strip()


def commit(repo):
    git(repo, 'add', '--all')
    git(repo, 'commit', '-m', 'fixture')
    return git(repo, 'rev-parse', 'HEAD')


def request(name, repo, rev):
    return f'{name}={{git={json.dumps(str(repo))},rev={json.dumps(rev)}}}\n'


def repository(name, source, dependencies=''):
    repo = root / name
    repo.mkdir()
    git(repo, 'init', '--template=', '--initial-branch=main')
    (repo / 'kalandra.toml').write_text(
        "format=1\n[package]\nname='fixture'\nversion='0.1.0'\nentry='public.k'\n" +
        ('[dependencies]\n' + dependencies if dependencies else ''))
    (repo / 'public.k').write_text(source)
    return repo, commit(repo)


math, first = repository('math source λ', 'export fn next(n:int)->int:\n  n+1\n')
git(math, 'tag', 'release', first)
parent, parent_first = repository('parent',
    'from "math" import next\nexport fn compute(n:int)->int:\n  next(n)\n', request('math', math, 'main'))
app = root / 'application'
app.mkdir()
manifest = app / 'kalandra.toml'
configuration = "format=1\n[project]\nentry='main.k'\n[dependencies]\n" + (
    request('math', math, 'main') + request('again', math, 'main') +
    request('tagged', math, 'release') + request('fixed', math, first) + request('parent', parent, 'main'))
manifest.write_text(configuration)
source = ('from "math" import next\nfrom "parent" import compute\n'
          'fn main()->[int] ! TaskError:\n  old=try parallel:\n    try await spawn compute(10)\n  [next(10),old]\n')
(app / 'main.k').write_text(source)
lock = app / 'kalandra.lock'
# Updates are not an implicit first install, and invalid selection creates no cache.
run(args.compiler, 'update', 'math', '--project', manifest, ok=False)
assert not lock.exists() and not (app / '.kalandra').exists()
run(args.compiler, 'install', '--project', manifest)
original = lock.read_bytes()


def state():
    data = tomllib.loads(lock.read_text())
    return data, {item['name']: item['commit'] for item in data['roots']}


def package(data, repo, revision):
    return next(item for item in data['packages'] if item['repository'] == str(repo) and item['commit'] == revision)


def update(*names, ok=True, api=False):
    if api and args.api:
        return run(args.api, manifest, *names, ok=ok)
    return run(args.compiler, 'update', *names, '--project', manifest, ok=ok)


for names in [('missing',), ('math', 'math'), ('math', '--offline'), ('math', '--frozen')]:
    update(*names, ok=False)
    update(*names, ok=False, api=True)
    assert lock.read_bytes() == original
update(*(['math'] * 129), ok=False, api=True)
for flags in [[], ['--all'], ['math', '--unknown']]:
    result = run(args.compiler, 'update', *flags, '--project', manifest, ok=False)
    assert result.returncode == 2
manifest.write_text(configuration + '# changed\n')
update('math', ok=False)
assert lock.read_bytes() == original
manifest.write_text(configuration)
lock.write_text('format=999\n')
update('math', ok=False)
assert lock.read_text() == 'format=999\n'
lock.write_bytes(original)

# A selected branch can add a dependency without the old graph-size gate rejecting it.
leaf, leaf_first = repository('leaf', 'export fn boost(n:int)->int:\n  n+2\n')
(math / 'kalandra.toml').write_text((math / 'kalandra.toml').read_text() + '[dependencies]\n' + request('leaf', leaf, 'main'))
(math / 'public.k').write_text('from "leaf" import boost\nexport fn next(n:int)->int:\n  boost(n)\n')
second = commit(math)
git(math, 'tag', '-f', 'release', second)
run(args.compiler, 'install', '--project', manifest)
assert lock.read_bytes() == original
update('math')
data, roots = state()
assert roots['math'] == second and all(roots[name] == first for name in ['again', 'tagged', 'fixed'])
assert roots['parent'] == parent_first and len(data['packages']) == 4
assert package(data, parent, parent_first)['dependencies'][0]['commit'] == first
assert package(data, math, second)['dependencies'][0]['commit'] == leaf_first
assert manifest.read_text() == configuration
run(args.checker, '--project', manifest)
program = app / 'program'
run(args.compiler, '--project', manifest, '-o', program)
assert run(program).stdout == b'[12, 11]\n'
# Moving a private ref alone does not update an unchanged owner's private lock.
(leaf / 'public.k').write_text('export fn boost(n:int)->int:\n  n+3\n')
leaf_second = commit(leaf)
update('math', api=True)
data, roots = state()
assert package(data, math, second)['dependencies'][0]['commit'] == leaf_first
# Multiple explicitly selected aliases converge; the tag follows its current target too.
update('again', 'tagged', api=True)
data, roots = state()
assert roots['again'] == second and roots['tagged'] == second and roots['fixed'] == first
assert package(data, parent, parent_first)['dependencies'][0]['commit'] == first
# An unchanged parent keeps its private math selection; a new parent gets its own current graph.
update('parent')
data, roots = state()
assert package(data, parent, parent_first)['dependencies'][0]['commit'] == first
(parent / 'kalandra.toml').write_text((parent / 'kalandra.toml').read_text().replace("version='0.1.0'", "version='0.2.0'"))
parent_second = commit(parent)
update('parent', api=True)
data, roots = state()
assert roots['parent'] == parent_second
assert package(data, parent, parent_second)['dependencies'][0]['commit'] == second
assert package(data, math, second)['dependencies'][0]['commit'] == leaf_first
# A new math owner may select the moved private leaf without moving other aliases.
(math / 'kalandra.toml').write_text((math / 'kalandra.toml').read_text().replace("version='0.1.0'", "version='0.2.0'"))
third = commit(math)
update('math')
data, roots = state()
assert roots['math'] == third and roots['again'] == second and roots['fixed'] == first
assert package(data, math, third)['dependencies'][0]['commit'] == leaf_second
assert package(data, math, second)['dependencies'][0]['commit'] == leaf_first
before = lock.read_bytes()
# Failed import graphs and held guards leave the active generation byte-for-byte unchanged.
(math / 'public.k').write_text('from "absent" import missing\nexport fn next(n:int)->int:\n  missing(n)\n')
commit(math)
update('math', ok=False)
update('math', ok=False, api=True)
assert lock.read_bytes() == before
with (app / '.kalandra/install.guard').open('r+') as guard:
    fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
    update('again', ok=False)
    assert lock.read_bytes() == before
# Frozen/offline still consume the selected generation; compilation does not install/update.
for flags in [['--frozen'], ['--offline']]:
    run(args.compiler, 'install', '--project', manifest, *flags)
    assert lock.read_bytes() == before
run(args.checker, '--project', manifest)
run(args.compiler, '--project', manifest, '-o', program)
assert run(program).stdout == b'[13, 12]\n'
math.rename(root / 'math unavailable')
parent.rename(root / 'parent unavailable')
leaf.rename(root / 'leaf unavailable')
update('again', ok=False)
assert lock.read_bytes() == before
# Exact commits still mean exact commits and can reuse verified cached snapshots.
update('fixed', api=True)
assert lock.read_bytes() == before
assert not list((app / '.kalandra').glob('.activate-*'))
print('Named package updates: selected roots, private pins, graph growth, aliases/tags, exact pins, rollback, modes and workers passed')
