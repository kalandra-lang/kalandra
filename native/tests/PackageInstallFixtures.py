"""Exact local Git dependency activation through public tools, without remote transport."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import tomllib

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--checker", required=True)
parser.add_argument("--root", required=True)
parser.add_argument("--variant", default="debug")
parser.add_argument("--api")
parser.add_argument("--metal")
args = parser.parse_args()
root = Path(args.root).absolute()
if root.exists():
    shutil.rmtree(root)
root.mkdir(parents=True)
env = {**os.environ, "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": "/dev/null",
       "GIT_AUTHOR_NAME": "Fixture", "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
       "GIT_COMMITTER_NAME": "Fixture", "GIT_COMMITTER_EMAIL": "fixture@example.invalid"}

def run(*command, ok=True, data=None):
    result = subprocess.run([str(c) for c in command], input=data, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env, timeout=120)
    assert not any(marker in result.stderr for marker in [b"ERROR: AddressSanitizer", b"runtime error:", b"WARNING: ThreadSanitizer"]), result.stderr
    if ok:
        assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    else:
        assert result.returncode != 0, (command, result.stdout, result.stderr)
    return result

def git(repo, *command):
    return run("/usr/bin/git", "-C", repo, *command).stdout.decode().strip()

def repo(name):
    path = root / name
    path.mkdir()
    git(path, "init", "--template=", "--initial-branch=main")
    return path

def commit(path):
    git(path, "add", "--all")
    git(path, "commit", "-m", "fixture")
    return git(path, "rev-parse", "HEAD")

def quote(value):
    return json.dumps(str(value), ensure_ascii=False)

def request(name, path, revision):
    return f"{name} = {{git={quote(path)}, rev={quote(revision)}}}\n"

def library(path, name, version, text, dependencies=""):
    (path / "kalandra.toml").write_text(f"format=1\n[package]\nname={quote(name)}\nversion={quote(version)}\nentry='public.k'\n" + ("[dependencies]\n" + dependencies if dependencies else ""))
    (path / "public.k").write_text(text)
    return commit(path)

geometry = repo("geometry source λ")
body = "export type Item:\n  value:int64\nexport fn next(item:Item)->Item:\n  item with {value:item.value+1}\n"
first = library(geometry, "geometry", "0.1.0", body)
second = library(geometry, "geometry", "0.2.0", body.replace("value+1", "value+2"))
telemetry = repo("telemetry")
third = library(telemetry, "telemetry", "1.0.0",
                'from "geometry" import Item, next\nexport fn compute(n:int64)->int64:\n  next(Item {value:n}).value\nexport fn take(item:Item)->int64:\n  item.value\n',
                request("geometry", geometry, first))
app = root / "application space λ"
app.mkdir()
manifest = app / "kalandra.toml"
source = app / "main.k"
text = ('from "geometry" import Item, next\nfrom "again" import Item as Same\n'
        'from "telemetry" import compute\nfn identity(item:Item)->Same:\n  item\n'
        'fn main()->[int64] ! TaskError:\n  old=try parallel:\n    try await spawn compute(10)\n'
        '  [next(identity(Item {value:10})).value,old]\n')
source.write_text(text)
configuration = ("format=1\n[project]\nentry='main.k'\n[dependencies]\n" +
                 request("geometry", geometry, second) + request("again", geometry, second) +
                 request("telemetry", telemetry, third))
manifest.write_text(configuration)
output = app / "program"
output.write_text("preserved")
for tool, extra in [(args.checker, []), (args.compiler, ["-o", output])]:
    run(tool, "--project", manifest, *extra, ok=False)
assert output.read_text() == "preserved" and not (app / ".kalandra").exists()
for option in ["--offline", "--frozen"]:
    run(args.compiler, "install", "--project", manifest, option, ok=False)
assert not (app / ".kalandra").exists()
if args.api:
    run(args.api, "install", manifest, "normal")
else:
    run(args.compiler, "install", "--project", manifest)
index = app / "kalandra.lock"
active = index.read_bytes()
assert len(tomllib.loads(active.decode())["packages"]) == 3
run(args.compiler, "install", "--project", manifest)
assert index.read_bytes() == active
run(args.checker, "--project", manifest)
response = json.loads(run(args.checker, "--diagnostics=json", "--project", manifest).stdout)
assert not response["diagnostics"]
# Unsaved root checking must not parse a broken disk buffer before applying stdin.
source.write_text("fn main(:\n")
run(args.checker, "--stdin", "--project", manifest, data=text.encode())
source.write_text('from "geometry" import Item\nfrom "telemetry" import take\nfn main():\n  take(Item {value:1})\n')
run(args.checker, "--project", manifest, ok=False)
source.write_text(text)
extra = app / "extra.k"
extra.write_text("export fn unused()->int64:\n  1\n")
run(args.checker, "--project", manifest, "--package", f"geometry={extra}", ok=False)
# Manifest spelling changes require explicit activation, without rewriting source.
manifest.write_text(configuration + "\n# changed\n")
assert b"differs" in run(args.checker, "--project", manifest, ok=False).stderr
assert index.read_bytes() == active
run(args.compiler, "install", "--project", manifest)
configuration = manifest.read_text()
active = index.read_bytes()
# Failure after a parent snapshot is published leaves the prior activation intact.
broken = repo("unresolved parent")
bad = library(broken, "unresolved", "1.0.0", "export fn value()->int64:\n  1\n",
              request("missing", geometry, "f" * 40))
manifest.write_text(configuration + request("unresolved", broken, bad))
run(args.compiler, "install", "--project", manifest, ok=False)
assert index.read_bytes() == active
manifest.write_text(configuration)
run(args.checker, "--project", manifest)
with (app / ".kalandra/install.guard").open("r+b") as guard:
    fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
    assert b"another package installation" in run(args.compiler, "install", "--project", manifest, ok=False).stderr
assert index.read_bytes() == active
# Snapshot or activation corruption never triggers implicit repair/fetch.
snapshots = list((app / ".kalandra/snapshots").iterdir())
used = next(p for p in snapshots if tomllib.loads((p / "snapshot.toml").read_text())["commit"] == second)
unused = next(p for p in snapshots if tomllib.loads((p / "snapshot.toml").read_text())["commit"] == bad)
public = used / "source/public.k"
original = public.read_bytes()
public.write_bytes(original + b"\n# edit\n")
run(args.checker, "--project", manifest, ok=False)
run(args.compiler, "install", "--project", manifest, ok=False)
assert public.read_bytes() != original and index.read_bytes() == active
public.write_bytes(original)
index.write_bytes(active + b"\nunknown=1\n")
run(args.checker, "--project", manifest, ok=False)
index.write_bytes(active)
# Protect every cache generation/file, including an unused valid snapshot.
for protected in [index, app / ".kalandra/install.guard", public, used / "snapshot.toml", unused / "source/public.k"]:
    before = hashlib.sha256(protected.read_bytes()).digest()
    for kind in ["direct", "symlink", "hardlink"]:
        destination = protected
        if kind != "direct":
            destination = app / f"alias-{kind}"
            destination.unlink(missing_ok=True)
            if kind == "symlink": destination.symlink_to(protected)
            else: os.link(protected, destination)
        for flag in ["-o", "--emit-llvm", "--emit-mlir"]:
            options = [flag, destination] if flag == "-o" else ["-o", output, flag, destination]
            result = run(args.compiler, "--project", manifest, *options, ok=False)
            assert result.returncode == 2 and b"and output" in result.stderr
            assert hashlib.sha256(protected.read_bytes()).digest() == before
for destination in [app / ".kalandra/new", app / ".kalandra"]:
    run(args.compiler, "--project", manifest, "-o", destination, ok=False)
redirect = app / "cache-link"
redirect.symlink_to(app / ".kalandra", target_is_directory=True)
run(args.compiler, "--project", manifest, "-o", redirect / "new", ok=False)
assert output.read_text() == "preserved"
if args.metal:
    gpu_repo = repo("gpu library")
    revision = library(gpu_repo, "numerical", "0.1.0", "export fn shift(values:Tensor[float32,?])->Tensor[float32,?]:\n  values+1\n")
    gpu_app = root / "gpu-app"
    gpu_app.mkdir()
    gpu_manifest = gpu_app / "kalandra.toml"
    gpu_configuration = "format=1\n[project]\nentry='main.k'\n[dependencies]\n" + request("numerical", gpu_repo, revision)
    gpu_manifest.write_text(gpu_configuration)
    (gpu_app / "main.k").write_text('from "numerical" import shift\nfn main()->float32 ! TaskError:\n  try parallel:\n    gpu=devices.gpu()\n    values:Tensor[float32,3]=tensor([1,2,3])\n    resident=try await transfer(values,gpu)\n    changed=try await spawn shift(resident) on gpu\n    host=try await transfer(changed,devices.cpu())\n    reduceSum(host)\n')
    run(args.compiler, "install", "--project", gpu_manifest)
    run(args.checker, "--placement", "--project", gpu_manifest)
    gpu_repo.rename(root / "gpu library unavailable")
    run(args.metal, "--project", gpu_manifest, "-o", gpu_app / "emitted")
    assert (gpu_app / "emitted/kernel0.mlir").exists()
    gpu_snapshot = next((gpu_app / ".kalandra/snapshots").iterdir())
    for protected in [gpu_app / "kalandra.lock", gpu_snapshot / "source/kalandra.toml", gpu_snapshot / "snapshot.toml"]:
        before = protected.read_bytes()
        for kind in ["hardlink", "symlink"]:
            destination = gpu_app / f"protected-{kind}"
            destination.mkdir(exist_ok=True)
            target = destination / "host.cpp"
            target.unlink(missing_ok=True)
            if kind == "hardlink": os.link(protected, target)
            else: target.symlink_to(protected)
            error = run(args.metal, "--project", gpu_manifest, "-o", destination, ok=False).stderr
            assert b"and output" in error and protected.read_bytes() == before
            assert not (destination / "placement.ir").exists()
    run(args.metal, "--project", gpu_manifest, "-o", gpu_app / ".kalandra/emitted", ok=False)
    assert not (gpu_app / ".kalandra/emitted").exists()
    # Source emission must not create a directory or follow a dangling output
    # alias at the reserved public lock, even with no managed graph/cache.
    (gpu_app / "numerical.k").write_text((gpu_snapshot / "source/public.k").read_text())
    gpu_manifest.write_text("format=1\n[project]\nentry='main.k'\n[packages]\nnumerical='numerical.k'\n")
    (gpu_app / "kalandra.lock").unlink()
    shutil.rmtree(gpu_app / ".kalandra")
    run(args.metal, "--project", gpu_manifest, "-o", gpu_app / "local-emitted")
    assert (gpu_app / "local-emitted/kernel0.mlir").exists()
    assert b"lock and output" in run(args.metal, "--project", gpu_manifest, "-o", gpu_app / "kalandra.lock", ok=False).stderr
    assert not (gpu_app / "kalandra.lock").exists()
    dangling = gpu_app / "dangling-emission"
    dangling.mkdir()
    (dangling / "host.cpp").symlink_to(gpu_app / "kalandra.lock")
    assert b"lock and output" in run(args.metal, "--project", gpu_manifest, "-o", dangling, ok=False).stderr
    assert not (gpu_app / "kalandra.lock").exists()
    assert not (dangling / "placement.ir").exists()
# Observe private snapshot staging, then change the manifest before activation.
changing = root / "changing-manifest"
changing.mkdir()
(changing / "main.k").write_text(text)
changed_manifest = changing / "kalandra.toml"
changed_manifest.write_text(configuration)
process = subprocess.Popen([args.compiler, "install", "--project", str(changed_manifest)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
deadline = time.monotonic() + 30
while process.poll() is None and not list((changing / ".kalandra/snapshots").glob(".kalandra-snapshot-*")):
    assert time.monotonic() < deadline, "installer did not begin staging"
    time.sleep(0.001)
assert process.poll() is None, "installer finished before staging could be observed"
changed_manifest.write_text(configuration + "\n# concurrent edit\n")
stdout, stderr = process.communicate(timeout=120)
assert process.returncode != 0 and b"manifest changed during installation" in stderr, (stdout, stderr)
assert not (changing / "kalandra.lock").exists()
assert not list((changing / ".kalandra").glob(".activate-*"))
if args.api:
    run(args.api, "load", manifest, "normal")
    wrapper = repo("wrapper")
    wrapper_revision = library(wrapper, "wrapper", "0.1.0", "export fn value()->int64:\n  1\n", request("telemetry", telemetry, third))
    shared = root / "shared-depth"
    shared.mkdir()
    (shared / "main.k").write_text(text)
    shared_manifest = shared / "kalandra.toml"
    shared_manifest.write_text(configuration + request("wrapper", wrapper, wrapper_revision))
    run(args.api, "install", shared_manifest, "normal")
    assert b"depth" in run(args.api, "load", shared_manifest, "shared-depth", ok=False).stderr
    for budget in ["packages", "depth", "entries", "bytes", "lock"]:
        limited = root / f"limited-{budget}"
        limited.mkdir()
        (limited / "main.k").write_text(text)
        (limited / "kalandra.toml").write_text(configuration)
        run(args.api, "install", limited / "kalandra.toml", budget, ok=False)
        assert not (limited / "kalandra.lock").exists()
        assert not list((limited / ".kalandra").glob(".activate-*"))
for bad in [
    "bad='value'", "'a.b'={git=" + quote(geometry) + ",rev=" + quote(first) + "}",
    "bad={git='./relative',rev=" + quote(first) + "}",
    "bad={git=" + quote(geometry) + ",rev='HEAD'}",
    "bad={git=" + quote(geometry) + ",rev=" + quote(first) + ",extra=true}",
]:
    malformed = app / "bad.toml"
    malformed.write_text("format=1\n[project]\nentry='main.k'\n[dependencies]\n" + bad + "\n")
    error = run(args.checker, "--project", malformed, ok=False).stderr
    assert b"dependency" in error or b"dependencies" in error, error
for option in ["--offline", "--frozen"]:
    run(args.compiler, "install", "--project", manifest, option)
    assert index.read_bytes() == active
for options in [["--unknown"], ["--offline", "--offline"], ["--frozen", "--frozen"], ["--project", manifest]]:
    assert run(args.compiler, "install", "--project", manifest, *options, ok=False).returncode == 2
# No original repository is available to the following build, check or reuse.
for path in [geometry, telemetry, broken]: path.rename(path.with_name(path.name + " unavailable"))
run(args.checker, "--project", manifest)
run(args.compiler, "install", "--project", manifest)
flags = {"debug": ["-O0"], "optimized": ["-O2"], "sanitized": ["--sanitize"],
         "thread-sanitized": ["--thread-sanitize"]}[args.variant]
run(args.compiler, "--project", manifest, "-o", output, *flags)
for workers in [1, 4]:
    env["KALANDRA_WORKERS"] = str(workers)
    assert run(output).stdout == b"[12, 11]\n"
# Metadata uses portable cache-relative layout even when repository identities are local paths.
moved = root / "relocated application"
app.rename(moved)
run(args.checker, "--project", moved / "kalandra.toml")
run(args.compiler, "--project", moved / "kalandra.toml", "-o", moved / "again", *flags)
assert run(moved / "again").stdout == b"[12, 11]\n"
print("Managed local packages: exact transitive scopes, complete activation, offline workers, relocation, rollback, corruption and output protection passed")
