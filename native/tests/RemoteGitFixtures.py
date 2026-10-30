"""Controlled smart HTTP/TLS Git servers; no external host or user credentials."""
import argparse
import http.server
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import threading
import time
import tomllib
from urllib.parse import urlsplit

parser = argparse.ArgumentParser()
parser.add_argument('--api', required=True)
parser.add_argument('--compiler', required=True)
parser.add_argument('--checker', required=True)
parser.add_argument('--root', required=True)
parser.add_argument('--metal')
parser.add_argument('--cmake')
parser.add_argument('--build')
parser.add_argument('--config')
args = parser.parse_args()
root = Path(args.root).absolute().resolve()
if root.exists():
    shutil.rmtree(root)
root.mkdir(parents=True)
repositories = root / 'repositories'
repositories.mkdir()
env = {**os.environ, 'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': '/dev/null',
       'GIT_AUTHOR_NAME': 'Fixture', 'GIT_AUTHOR_EMAIL': 'fixture@example.invalid',
       'GIT_COMMITTER_NAME': 'Fixture', 'GIT_COMMITTER_EMAIL': 'fixture@example.invalid'}

def run(*command, ok=True):
    result = subprocess.run(list(map(str, command)), env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=60)
    assert not any(s in result.stderr for s in [b'ERROR: AddressSanitizer', b'runtime error:',
                                               b'WARNING: ThreadSanitizer']), result.stderr
    assert (result.returncode == 0) == ok, (command, result.returncode, result.stdout, result.stderr)
    return result

def git(path, *command):
    return run('/usr/bin/git', '-C', path, *command).stdout.decode().strip()

requests = []
transport_failures = {}
request_lock = threading.Lock()
stopped = threading.Event()
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *unused):
        pass

    def do_GET(self):
        self.respond()

    def do_POST(self):
        self.respond()

    def respond(self):
        path = urlsplit(self.path)
        with request_lock:
            requests.append(path.path)
            failure = next((mode for prefix, mode in transport_failures.items()
                            if path.path.startswith(prefix + '/')), None)
        if failure == 'redirect' or path.path.startswith('/redirect/'):
            self.send_response(302)
            self.send_header('Location', f'http://127.0.0.1:{self.server.server_port}/forbidden/geometry.git/info/refs?service=git-upload-pack')
            self.end_headers()
            return
        if failure == 'auth' or path.path.startswith('/auth/'):
            self.send_response(401)
            self.send_header('WWW-Authenticate', 'Basic realm="fixture"')
            self.end_headers()
            return
        if path.path.startswith('/stall/'):
            stopped.wait(4)
            self.send_response(503)
            self.end_headers()
            return
        if failure == 'malformed' or path.path.startswith('/malformed/'):
            self.send_response(200)
            self.send_header('Content-Type', 'application/x-git-upload-pack-advertisement')
            self.end_headers()
            self.wfile.write(b'not a git packet')
            return
        body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
        backend = subprocess.run(['/usr/bin/git', 'http-backend'], input=body, capture_output=True,
            env={**env, 'GIT_PROJECT_ROOT': str(repositories), 'GIT_HTTP_EXPORT_ALL': '1',
                 'PATH_INFO': path.path, 'QUERY_STRING': path.query, 'REQUEST_METHOD': self.command,
                 'CONTENT_TYPE': self.headers.get('Content-Type', ''), 'CONTENT_LENGTH': str(len(body)),
                 'REMOTE_ADDR': '127.0.0.1'}, timeout=15)
        headers, separator, payload = backend.stdout.partition(b'\r\n\r\n')
        assert separator, backend.stderr
        status = 200
        pairs = []
        for line in headers.decode().split('\r\n'):
            name, value = line.split(':', 1)
            if name.lower() == 'status':
                status = int(value.strip().split()[0])
            else:
                pairs.append((name, value.strip()))
        self.send_response(status)
        for name, value in pairs:
            self.send_header(name, value)
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass

class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

http = Server(('127.0.0.1', 0), Handler)
https = Server(('127.0.0.1', 0), Handler)
ca = root / 'ca.pem'
key = root / 'key.pem'
configuration = root / 'openssl.cnf'
configuration.write_text('[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=extensions\n'
                         '[dn]\nCN=Kalandra local transport fixture\n[extensions]\n'
                         'basicConstraints=critical,CA:true\nsubjectAltName=IP:127.0.0.1\n'
                         'keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign\n')
run('/usr/bin/openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
    '-config', configuration, '-keyout', key, '-out', ca)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(ca, key)
https.socket = context.wrap_socket(https.socket, server_side=True)
for server in [http, https]:
    threading.Thread(target=server.serve_forever, daemon=True).start()
base = f'http://127.0.0.1:{http.server_port}'
tls = f'https://127.0.0.1:{https.server_port}'

def repository(name, source, dependencies='', extra=None):
    work = root / (name + '-work')
    work.mkdir()
    git(work, 'init', '--template=', '--initial-branch=main')
    (work / 'kalandra.toml').write_text("format=1\n[package]\nname='fixture'\nversion='0.1.0'\nentry='public.k'\n" +
                                      ('[dependencies]\n' + dependencies if dependencies else ''))
    (work / 'public.k').write_text(source)
    (work / '.gitattributes').write_text('raw.txt export-subst\n*.k filter=attack\n')
    (work / 'raw.txt').write_text('$Format:%H$\n')
    if extra:
        extra(work)
    git(work, 'add', '--all')
    git(work, 'commit', '-m', 'fixture')
    revision = git(work, 'rev-parse', 'HEAD')
    run('/usr/bin/git', 'clone', '--bare', '--no-hardlinks', work, repositories / (name + '.git'))
    return revision

def request(name, url, revision):
    return f'{name}={{git={json.dumps(url)},rev={json.dumps(revision)}}}\n'

simple = 'export fn next(n:int64)->int64:\n  n+1\n'
first = repository('geometry', simple)
remote_geometry = repositories / 'geometry.git'
git(remote_geometry, 'tag', '-a', 'release', first, '-m', 'release')
git(remote_geometry, 'tag', '-a', 'nested', 'release', '-m', 'nested')
git(remote_geometry, 'tag', 'light', first)
git(remote_geometry, 'tag', 'collision', first)
git(remote_geometry, 'branch', 'collision', first)
blob = git(remote_geometry, 'rev-parse', first + ':public.k')
git(remote_geometry, 'tag', 'blob', blob)
geometry_work = root / 'geometry-work'
(geometry_work / 'public.k').write_text(simple.replace('n+1', 'n+2'))
git(geometry_work, 'add', '--all')
git(geometry_work, 'commit', '-m', 'next remote revision')
second = git(geometry_work, 'rev-parse', 'HEAD')
git(geometry_work, 'push', remote_geometry, 'HEAD:refs/heads/future')

parent = repository('parent', 'from "geometry" import next\nexport fn compute(n:int64)->int64:\n  next(n)\n',
                    request('geometry', base + '/geometry.git', first))
branch_parent = repository('branch-parent', 'from \"geometry\" import next\nexport fn compute(n:int64)->int64:\n  next(n)\n',
                           request('geometry', base + '/geometry.git', 'main'))
large = repository('large', simple, extra=lambda path: (path / 'large.bin').write_bytes(os.urandom(1024 * 1024)))
unsafe = repository('unsafe', simple, extra=lambda path: (path / 'link.k').symlink_to('public.k'))
local = repository('local-dependency', simple, request('private', str(root / 'private'), first))
numerical = repository('numerical', 'export fn shift(values:Tensor[float32,?])->Tensor[float32,?]:\n  values+1\n')
app = root / 'application'
app.mkdir()
(app / 'main.k').write_text('from "parent" import compute\nfn main()->int64 ! TaskError:\n  try parallel:\n    try await spawn compute(41)\n')
manifest = app / 'kalandra.toml'
manifest.write_text("format=1\n[project]\nentry='main.k'\n[dependencies]\n" + request('parent', base + '/parent.git', parent))

def api(mode, cache, url, revision=first, certificate='-', budget='normal', ok=True):
    return run(args.api, mode, cache, url, revision, certificate, budget, ok=ok)

try:
    # Inherited Git config, proxy/TLS and askpass settings must not reach fetch.
    marker = root / 'executed'
    env.update({'GIT_CONFIG_COUNT': '3', 'GIT_CONFIG_KEY_0': 'core.fsmonitor',
                'GIT_CONFIG_VALUE_0': f'!touch {marker}', 'GIT_CONFIG_KEY_1': 'url.file:///bad.insteadOf',
                'GIT_CONFIG_VALUE_1': base, 'GIT_CONFIG_KEY_2': 'http.proxy',
                'GIT_CONFIG_VALUE_2': 'http://127.0.0.1:1', 'GIT_SSL_NO_VERIFY': '1',
                'HTTPS_PROXY': 'http://127.0.0.1:1', 'ALL_PROXY': 'http://127.0.0.1:1',
                'GIT_ASKPASS': str(marker)})
    for url in ['https://github.com/team/repo.git', base + '/geometry.git']:
        api('validate', root, url)
    for url in ['http://example.com/a.git', 'http://localhost:123/a.git', 'git://example.com/a',
                'ssh://example.com/a', 'git@example.com:a', 'file:///tmp/a', 'ext::touch-marker',
                'https://user:pass@example.com/a', 'https://example.com/a?token=x',
                'https://example.com/a#x', 'https://example.com/a%2fb', 'https://EXAMPLE.com/a',
                'https://example.com/../a', 'https://example.com//a', 'https://example.com/a/',
                'https://example.com:443/a', 'https://example.com:0001/a', 'https://example.com:99999/a',
                'https://example.com./a', 'https://example.com/a\n']:
        api('validate', root, url, ok=False)
    # Strict TLS is the default despite hostile ambient verification settings.
    untrusted = root / 'untrusted'
    api('install', untrusted, tls + '/geometry.git', ok=False)
    assert not list(untrusted.iterdir())
    trusted = root / 'trusted'
    trusted_result = api('install', trusted, tls + '/geometry.git', certificate=ca).stdout
    assert trusted_result.startswith(b'git-remote-v1:')
    assert next(trusted.glob('*/source/raw.txt')).read_text() == '$Format:%H$\n'
    wrong_host = root / 'wrong-host'
    api('install', wrong_host, f'https://localhost:{https.server_port}/geometry.git', certificate=ca, ok=False)
    assert not list(wrong_host.iterdir())
    tls_app = root / 'tls-application'
    tls_app.mkdir()
    tls_manifest = tls_app / 'kalandra.toml'
    tls_manifest.write_text("format=1\n[project]\nentry='main.k'\n[dependencies]\n" + request('geometry', tls + '/geometry.git', 'main'))
    (tls_app / 'main.k').write_text('from "geometry" import next\nfn main()->int64 ! TaskError:\n  try parallel:\n    try await spawn next(41)\n')
    run(args.compiler, 'install', '--project', tls_manifest, ok=False)
    assert not (tls_app / 'kalandra.lock').exists()
    api('project', tls_manifest, tls + '/geometry.git', certificate=ca)
    # Installation from remote source must reject source links and local dependencies.
    for name, revision in [('unsafe', unsafe), ('local-dependency', local)]:
        cache = root / (name + '-cache')
        api('install', cache, base + '/' + name + '.git', revision, ok=False)
        assert not list(cache.iterdir())
    for name, url, revision, budget in [
        ('redirect', base + '/redirect/repo.git', first, 'normal'),
        ('auth', base + '/auth/repo.git', first, 'normal'),
        ('malformed', base + '/malformed/repo.git', first, 'normal'),
        ('timeout', base + '/stall/repo.git', first, 'timeout'),
        ('missing', base + '/geometry.git', 'f' * 40, 'normal'),
        ('bytes', base + '/large.git', large, 'bytes'),
        ('entries', base + '/geometry.git', first, 'entries'),
        ('source', base + '/geometry.git', first, 'source'),
        ('listing', base + '/geometry.git', first, 'listing')]:
        cache = root / (name + '-cache')
        start = time.monotonic()
        api('install', cache, url, revision, budget=budget, ok=False)
        assert time.monotonic() - start < 15
        assert not list(cache.iterdir()), (name, list(cache.iterdir()))
    assert not any(path.startswith('/forbidden/') for path in requests)
    # Public CLI installs a complete transitive graph. Failed extension keeps it active.
    run(args.compiler, 'install', '--project', manifest)
    run(args.checker, '--project', manifest)
    active = app / 'kalandra.lock'
    original = active.read_bytes()
    assert len(tomllib.loads(original.decode())['packages']) == 2
    fresh_app = root / 'fresh locked checkout'
    fresh_app.mkdir()
    fresh_manifest = fresh_app / 'kalandra.toml'
    fresh_manifest.write_bytes(manifest.read_bytes())
    (fresh_app / 'main.k').write_bytes((app / 'main.k').read_bytes())
    (fresh_app / 'kalandra.lock').write_bytes(original)
    with request_lock:
        before_offline = len(requests)
    run(args.compiler, 'install', '--project', fresh_manifest, '--offline', ok=False)
    with request_lock:
        assert len(requests) == before_offline
    assert not (fresh_app / '.kalandra').exists()
    run(args.compiler, 'install', '--project', fresh_manifest, '--frozen')
    assert (fresh_app / 'kalandra.lock').read_bytes() == original
    text = manifest.read_text()
    manifest.write_text(text + request('missing', base + '/missing.git', first))
    run(args.compiler, 'install', '--project', manifest, ok=False)
    assert active.read_bytes() == original
    manifest.write_text(text)
    if args.metal:
        gpu_app = root / 'gpu-application'
        gpu_app.mkdir()
        gpu_manifest = gpu_app / 'kalandra.toml'
        gpu_manifest.write_text("format=1\n[project]\nentry='main.k'\n[dependencies]\n" + request('numerical', base + '/numerical.git', numerical))
        (gpu_app / 'main.k').write_text('from "numerical" import shift\nfn main()->float32 ! TaskError:\n  try parallel:\n    gpu=devices.gpu()\n    values:Tensor[float32,3]=tensor([1,2,3])\n    resident=try await transfer(values,gpu)\n    changed=try await spawn shift(resident) on gpu\n    host=try await transfer(changed,devices.cpu())\n    reduceSum(host)\n')
        run(args.compiler, 'install', '--project', gpu_manifest)
    if args.build:
        prefix = root / 'compiler installation'
        run(args.cmake, '--install', args.build, '--config', args.config,
            '--component', 'NativeCPUCompiler', '--prefix', prefix)
        installed_prefix = root / 'relocated compiler'
        prefix.rename(installed_prefix)
        installed = installed_prefix / 'bin/kalandra'
        installed_app = root / 'installed application'
        installed_app.mkdir()
        installed_manifest = installed_app / 'kalandra.toml'
        installed_manifest.write_text(text)
        (installed_app / 'main.k').write_bytes((app / 'main.k').read_bytes())
        run(installed, 'install', '--project', installed_manifest)
    # Ref lookup uses the same actual HTTP(S) transport as pinned fetches.
    for number, (revision, expected) in enumerate([('main', first), ('release', first),
            ('nested', first), ('light', first), ('refs/heads/future', second),
            ('refs/tags/collision', first), ('refs/heads/collision', first)]):
        result = api('revision', root / f'remote-ref-{number}', base + '/geometry.git', revision)
        assert result.stdout.splitlines()[0].decode() == expected
    for number, revision in enumerate(['collision', 'missing', 'blob']):
        failed_cache = root / f'remote-ref-failed-{number}'
        result = api('revision', failed_cache, base + '/geometry.git', revision, ok=False)
        if revision == 'collision':
            assert b'ambiguous' in result.stderr
        assert not list(failed_cache.iterdir())
    api('revision', root / 'remote-ref-budget', base + '/geometry.git', 'main', budget='listing', ok=False)
    stalled_ref = root / 'remote-ref-timeout'
    assert b'timeout' in api('revision', stalled_ref, base + '/stall/repo.git', 'main', budget='timeout', ok=False).stderr
    assert not list(stalled_ref.iterdir())
    api('revision', root / 'remote-ref-untrusted', tls + '/geometry.git', 'release', ok=False)
    assert api('revision', root / 'remote-ref-tls', tls + '/geometry.git', 'release', certificate=ca).stdout.splitlines()[0].decode() == first
    ref_app = root / 'ref application'
    ref_app.mkdir()
    ref_manifest = ref_app / 'kalandra.toml'
    ref_manifest.write_text("format=1\n[project]\nentry='main.k'\n[dependencies]\n" +
        request('geometry', base + '/geometry.git', 'main') +
        request('other', base + '/geometry.git', 'main') +
        request('tagged', base + '/geometry.git', 'release') +
        request('parent', base + '/branch-parent.git', branch_parent))
    (ref_app / 'main.k').write_text('from "geometry" import next\nfrom "parent" import compute\n'
        'fn main()->[int64] ! TaskError:\n  try parallel:\n'
        '    direct=try await spawn next(41)\n    private=try await spawn compute(41)\n    [direct,private]\n')
    run(args.compiler, 'install', '--project', ref_manifest)
    ref_lock = (ref_app / 'kalandra.lock').read_bytes()
    assert tomllib.loads(ref_lock.decode())['roots'][0]['commit'] == first
    # Mutate only the fixture server's trusted bare repository, clearing injected
    # Git configuration so the fixture does not run its own test attack marker.
    for reference in ['refs/heads/main', 'refs/tags/release']:
        changed = subprocess.run(['/usr/bin/git', '-C', str(remote_geometry), 'update-ref', reference, second],
            env={**env, 'GIT_CONFIG_COUNT':'0'}, capture_output=True, timeout=30)
        assert changed.returncode == 0, changed.stderr
    before_reuse = len(requests)
    for options in [[], ['--frozen'], ['--offline']]:
        run(args.compiler, 'install', '--project', ref_manifest, *options)
        assert (ref_app / 'kalandra.lock').read_bytes() == ref_lock
    assert len(requests) == before_reuse
    ref_fresh = root / 'fresh ref checkout'
    ref_fresh.mkdir()
    for filename in ['main.k', 'kalandra.toml', 'kalandra.lock']:
        (ref_fresh / filename).write_bytes((ref_app / filename).read_bytes())
    run(args.compiler, 'install', '--project', ref_fresh / 'kalandra.toml', '--frozen')
    assert (ref_fresh / 'kalandra.lock').read_bytes() == ref_lock
    # Update the same HTTP branch deliberately; unrelated aliases and private
    # branch pins must stay fixed even though they use the identical repository.
    manifest_bytes = ref_manifest.read_bytes()
    source_bytes = (ref_app / 'main.k').read_bytes()
    run(args.compiler, 'update', 'geometry', '--project', ref_manifest)
    updated = (ref_app / 'kalandra.lock').read_bytes()
    selected = tomllib.loads(updated.decode())
    assert {edge['name']:edge['commit'] for edge in selected['roots']} == {
        'geometry':second,'other':first,'tagged':first,'parent':branch_parent}
    owner = next(package for package in selected['packages'] if package['commit'] == branch_parent)
    assert owner['dependencies'][0]['commit'] == first
    assert len(selected['packages']) == 3
    # Explicitly update a moved tag too, retaining the still-unselected branch alias.
    run(args.compiler, 'update', 'geometry', 'tagged', '--project', ref_manifest)
    updated = (ref_app / 'kalandra.lock').read_bytes()
    assert {edge['name']:edge['commit'] for edge in tomllib.loads(updated.decode())['roots']} == {
        'geometry':second,'other':first,'tagged':second,'parent':branch_parent}
    assert ref_manifest.read_bytes() == manifest_bytes and (ref_app / 'main.k').read_bytes() == source_bytes
    # Re-resolution still verifies TLS despite a cached old snapshot and ambient
    # bypass settings. The private test API can supply its explicit trusted CA.
    tls_before = (tls_app / 'kalandra.lock').read_bytes()
    run(args.compiler, 'update', 'geometry', '--project', tls_manifest, ok=False)
    assert (tls_app / 'kalandra.lock').read_bytes() == tls_before
    api('update', tls_manifest, tls + '/geometry.git', 'geometry', certificate=ca)
    assert tomllib.loads((tls_app / 'kalandra.lock').read_text())['roots'][0]['commit'] == second
    # Ref lookup failures must not fall back to the cached selected commit.
    for failure in ['auth', 'redirect', 'malformed']:
        with request_lock:
            transport_failures['/geometry.git'] = failure
            before = len(requests)
        run(args.compiler, 'update', 'geometry', '--project', ref_manifest, ok=False)
        with request_lock:
            assert len(requests) > before
            del transport_failures['/geometry.git']
        assert (ref_app / 'kalandra.lock').read_bytes() == updated
        assert ref_manifest.read_bytes() == manifest_bytes
        assert not any(path.startswith('/forbidden/') for path in requests)
    if args.build:
        installed_update_app = root / 'installed update application'
        shutil.copytree(ref_fresh, installed_update_app)
        installed_update_manifest = installed_update_app / 'kalandra.toml'
        run(installed, 'update', 'geometry', '--project', installed_update_manifest)
        installed_selected = tomllib.loads((installed_update_app / 'kalandra.lock').read_text())
        assert {edge['name']:edge['commit'] for edge in installed_selected['roots']} == {
            'geometry':second,'other':first,'tagged':first,'parent':branch_parent}
    # A newer commit downloads successfully but has an invalid import graph.
    # Mutate only fixture-owned Git data with injected configuration disabled.
    (geometry_work / 'public.k').write_text('from "missing" import absent\nexport fn next(n:int64)->int64:\n  absent(n)\n')
    for command in [('add','--all'),('commit','-m','broken update'),('push',str(remote_geometry),'HEAD:refs/heads/main')]:
        changed = subprocess.run(['/usr/bin/git','-C',str(geometry_work),*command],
            env={**env,'GIT_CONFIG_COUNT':'0'},capture_output=True,timeout=30)
        assert changed.returncode == 0, changed.stderr
    run(args.compiler, 'update', 'geometry', '--project', ref_manifest, ok=False)
    assert (ref_app / 'kalandra.lock').read_bytes() == updated
    assert not list((ref_app / '.kalandra').rglob('.activate-*'))
    assert not list((ref_app / '.kalandra').rglob('.kalandra-snapshot-*'))
    assert not marker.exists()
finally:
    stopped.set()
    for server in [http, https]:
        server.shutdown()
        server.server_close()

# No network service remains. Neither loads, explicit reuse, checking nor builds fetch.
observed = len(requests)
assert api('load', trusted, tls + '/geometry.git').stdout == trusted_result
assert api('install', trusted, tls + '/geometry.git').stdout == trusted_result
run(args.compiler, 'install', '--project', manifest, '--offline')
run(args.compiler, 'install', '--project', manifest, '--frozen')
assert active.read_bytes() == original
run(args.checker, '--project', fresh_manifest)
run(args.compiler, '--project', fresh_manifest, '-o', fresh_app / 'program')
assert run(fresh_app / 'program').stdout == b'42\n'
run(args.checker, '--project', manifest)
run(args.checker, '--project', ref_fresh / 'kalandra.toml')
run(args.compiler, '--project', ref_fresh / 'kalandra.toml', '-o', ref_fresh / 'program')
assert run(ref_fresh / 'program').stdout == b'[42, 42]\n'
run(args.checker, '--project', tls_manifest)
run(args.compiler, '--project', tls_manifest, '-o', tls_app / 'program')
assert run(tls_app / 'program').stdout == b'43\n'
# A mutable update cannot succeed offline by silently reusing its selected pin.
run(args.compiler, 'update', 'geometry', '--project', ref_manifest, ok=False)
assert (ref_app / 'kalandra.lock').read_bytes() == updated
run(args.compiler, 'install', '--project', ref_manifest, '--frozen', '--offline')
run(args.checker, '--project', ref_manifest)
run(args.compiler, '--project', ref_manifest, '-o', ref_app / 'program')
assert run(ref_app / 'program').stdout == b'[43, 42]\n'
if args.build:
    run(installed, '--project', installed_update_manifest, '-o', installed_update_app / 'program')
    assert run(installed_update_app / 'program').stdout == b'[43, 42]\n'
if args.metal:
    run(args.checker, '--placement', '--project', gpu_manifest)
    run(args.metal, '--project', gpu_manifest, '-o', gpu_app / 'emitted')
    assert (gpu_app / 'emitted/kernel0.mlir').exists()
if args.build:
    run(installed, '--project', installed_manifest, '-o', installed_app / 'program')
    assert run(installed_app / 'program').stdout == b'42\n'
for mode in [[], ['-O2'], ['--sanitize'], ['--thread-sanitize']]:
    output = app / 'program'
    run(args.compiler, '--project', manifest, '-o', output, *mode)
    for workers in [1, 4]:
        env['KALANDRA_WORKERS'] = str(workers)
        assert run(output).stdout == b'42\n'
assert len(requests) == observed
# A corrupted remote snapshot is never silently fetched or repaired.
source = next(trusted.glob('*/source/public.k'))
source.write_text('corrupt')
api('install', trusted, tls + '/geometry.git', ok=False)
print('Remote Git: strict TLS and URL policy, bounded HTTP(S) fetch, transitive activation, rollback, rejection, timeout, named updates/private pins/TLS rollback and offline workers passed')
