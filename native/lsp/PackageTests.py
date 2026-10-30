"""Scoped, read-only package graphs through real framed editor requests."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from ProtocolClient import Client, publications

server, compiler, destination = sys.argv[1:]
env = {**os.environ, "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": "/dev/null",
       "GIT_AUTHOR_NAME": "Fixture", "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
       "GIT_COMMITTER_NAME": "Fixture", "GIT_COMMITTER_EMAIL": "fixture@example.invalid"}

def run(*args):
    result = subprocess.run(args, env=env, capture_output=True, timeout=60)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result.stdout.decode().strip()

def commit(repo):
    run('/usr/bin/git', '-C', str(repo), 'add', '--all')
    run('/usr/bin/git', '-C', str(repo), 'commit', '-m', 'fixture')
    return run('/usr/bin/git', '-C', str(repo), 'rev-parse', 'HEAD')

def request(name, repo, rev):
    return f'{name}={{git={json.dumps(str(repo))},rev="{rev}"}}\n'

Path(destination).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=destination) as temporary:
    root = Path(temporary).resolve()
    geometry, library, app = [root / part for part in ('geometry', 'library', 'app space λ')]
    for repo in (geometry, library):
        repo.mkdir()
        run('/usr/bin/git', '-C', str(repo), 'init', '--template=', '--initial-branch=main')
    geometry_manifest = geometry / 'kalandra.toml'
    geometry_manifest.write_text("format=1\n[package]\nname='geometry'\nversion='0.1.0'\nentry='public.k'\n")
    old_text = 'export fn measure(value:int64)->int64:\n  value+1\n'
    (geometry / 'public.k').write_text(old_text)
    old = commit(geometry)
    geometry_manifest.write_text(geometry_manifest.read_text().replace('0.1.0', '0.2.0'))
    (geometry / 'public.k').write_text(old_text.replace('int64', 'float32'))
    new = commit(geometry)
    library_text = 'from "geometry" import measure\nexport fn compute(value:int64)->int64:\n  measure(value)\n'
    (library / 'public.k').write_text(library_text)
    (library / 'kalandra.toml').write_text("format=1\n[package]\nname='library'\nversion='1.0.0'\nentry='public.k'\n[dependencies]\n" + request('geometry', geometry, old))
    wrapper = commit(library)
    app.mkdir()
    manifest = app / 'kalandra.toml'
    configuration = "format=1\n[project]\nentry='main.k'\n[dependencies]\n" + request('geometry', geometry, new) + request('library', library, wrapper)
    manifest.write_text(configuration)
    main = app / 'main.k'
    good = 'from "geometry" import measure\nfrom "library" import compute\nfn main()->int64:\n  result=compute(10)\n  result\n'
    # An invalid saved buffer must be replaceable before the root is parsed.
    (app / 'other.k').write_text('from "library" import compute\nfn other()->int64:\n  compute(1)\n')
    main.write_text(good)
    run(compiler, 'install', '--project', str(manifest))
    main.write_text('broken disk source\n')
    relocated = root / 'relocated application λ'
    shutil.move(app, relocated)
    app = relocated
    manifest, main = app / 'kalandra.toml', app / 'main.k'
    snapshots = list((app / '.kalandra/snapshots').glob('*/source/public.k'))
    old_source = next(p for p in snapshots if p.read_text() == old_text)
    wrapper_source = next(p for p in snapshots if p.read_text() == library_text)
    new_source = next(p for p in snapshots if 'float32' in p.read_text())
    before = {p.relative_to(app): p.read_bytes() for p in app.rglob('*') if p.is_file()}
    # No repository or network is needed by editor analysis.
    shutil.rmtree(geometry)
    shutil.rmtree(library)
    client = Client(server, '--placement', '--project', str(manifest))
    client.init(rename=True, rootUri=app.as_uri())
    client.open(main, good)
    assert publications(client.barrier())[main.as_uri()]['diagnostics'] == []
    assert 'int64' in str(client.hover(main, 3, 10)['result'])
    target = client.definition(main, 3, 10)['result']
    assert target['uri'] == wrapper_source.as_uri(), target
    # Workspace discovery must skip .kalandra, and the private int64 version
    # must not be mistaken for the application's float32 geometry version.
    client.open(wrapper_source, library_text)
    client.barrier()
    assert client.definition(wrapper_source, 2, 4)['result']['uri'] == old_source.as_uri()
    assert 'int64' in str(client.hover(wrapper_source, 2, 4)['result'])
    assert client.definition(main, 0, 25)['result']['uri'] == new_source.as_uri()
    refs = client.references(main, 3, 10, True)['result']
    assert {v['uri'] for v in refs} == {main.as_uri(), (app / 'other.k').as_uri(), wrapper_source.as_uri()}, refs
    assert 'read-only' in client.rename(main, 3, 10, 'renamed')['error']['message']
    assert 'read-only' in client.prepare_rename(main, 3, 10)['error']['message']
    local = client.rename(main, 3, 3, 'answer')['result']['documentChanges']
    assert len(local) == 1 and local[0]['textDocument']['uri'] == main.as_uri(), local
    assert client.formatting(wrapper_source)['result'] == []
    client.change(main, good.replace('  result\n', '  comp\n'), 2)
    client.barrier()
    names = client.completion(main, 4, 6)['result']['items']
    assert [item['label'] for item in names] == ['compute'], names
    client.change(main, good.replace('  result\n', '  measure(10)\n'), 3)
    errors = publications(client.barrier())[main.as_uri()]['diagnostics']
    assert errors, 'application must use float32 signature, not private int64 version'
    client.change(main, good, 4)
    assert publications(client.barrier())[main.as_uri()]['diagnostics'] == []
    aliased = good.replace('"library" import compute', '"library" import compute as invoke').replace('result=compute', 'result=invoke')
    client.change(main, aliased, 5)
    client.barrier()
    alias_changes = client.rename(main, 3, 10, 'execute')['result']['documentChanges']
    assert len(alias_changes) == 1 and alias_changes[0]['textDocument']['uri'] == main.as_uri()
    client.change(main, good, 6)
    client.barrier()
    # Structural queries retain scoped package versions and never edit the cache.
    unfinished = good.replace('  result\n', '  compute(1+resSuffix\n')
    client.change(main, unfinished, 7)
    assert any(d['code']=='L005' for d in publications(client.barrier())[main.as_uri()]['diagnostics'])
    cursor=unfinished.splitlines()[4].index('resSuffix')+3
    names=client.completion(main,4,cursor)['result']['items']
    assert [(item['label'],item['kind']) for item in names]==[('result',6)],names
    hint=client.signature(main,4,cursor)['result']
    assert hint and hint['activeParameter']==0 and 'value: int64' in hint['signatures'][0]['label'],hint
    assert not client.barrier(), 'recovery query published diagnostics'
    client.change(main,good,8)
    assert publications(client.barrier())[main.as_uri()]['diagnostics']==[]
    client.change(wrapper_source, library_text.replace('measure(value)', 'measure(1)'), 2)
    events = client.barrier()
    assert any('read-only' in str(event) for event in events), events
    assert client.hover(main, 3, 10)['result'] is None
    client.close(wrapper_source)
    assert publications(client.barrier())[main.as_uri()]['diagnostics'] == []
    # Fresh integrity and lock checks: no stale typed graph after corruption.
    lock = app / 'kalandra.lock'
    locked = lock.read_bytes()
    lock.unlink()
    assert client.hover(main, 3, 10)['result'] is None
    client.send('textDocument/didSave', {'textDocument': {'uri': main.as_uri()}})
    assert any('kalandra.lock' in str(event) for event in client.barrier())
    assert not lock.exists(), 'editor must not install a missing lock'
    lock.write_text('invalid lock')
    assert client.hover(main, 3, 10)['result'] is None
    assert client.completion(main, 3, 12)['result']['items'] == []
    assert 'error' in client.rename(main, 3, 3, 'answer')
    lock.write_bytes(locked)
    old_source.write_text(old_text + '\n# tampered\n')
    assert client.hover(main, 3, 10)['result'] is None
    old_source.write_text(old_text)
    assert client.definition(main, 3, 10)['result']['uri'] == wrapper_source.as_uri()
    manifest.write_text(configuration + '\n# changed configuration\n')
    assert client.hover(main, 3, 10)['result'] is None
    manifest.write_text(configuration)
    assert client.hover(main, 3, 10)['result'] is not None
    client.finish()
    after = {p.relative_to(app): p.read_bytes() for p in app.rglob('*') if p.is_file()}
    assert before == after, 'editor must not install, repair or modify source/cache/lock files'
print('Package-aware editor diagnostics, completion, navigation, references and rename passed')
