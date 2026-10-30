"""Local Git fixtures; no remote transport or project install command is exercised."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

binary, location = sys.argv[1:]
root = Path(location).absolute()
if root.exists():
    shutil.rmtree(root)
root.mkdir(parents=True)
env = {"PATH": "/usr/bin:/bin", "HOME": str(root / "home"), "LC_ALL": "C",
       "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": "/dev/null",
       "GIT_AUTHOR_NAME": "Fixture", "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
       "GIT_COMMITTER_NAME": "Fixture", "GIT_COMMITTER_EMAIL": "fixture@example.invalid"}

def git(repo, *args, data=None):
    return subprocess.check_output(["/usr/bin/git", "-C", str(repo), *args],
                                   input=data, env=env, stderr=subprocess.PIPE).decode().strip()

def initialize(name):
    repo = root / name
    repo.mkdir()
    git(repo, "init", "--template=", "--initial-branch=main")
    return repo

def commit(repo):
    git(repo, "add", "--all")
    git(repo, "commit", "-m", "fixture")
    return git(repo, "rev-parse", "HEAD")

repo = initialize("repository")
(repo / "kalandra.toml").write_text("format=1\n[package]\nname='geometry'\nversion='0.1.0'\nentry='public.k'\n")
(repo / "types.k").write_text("export type Item:\n  value:int64\n")
public = 'export from "./types.k" import Item\nexport fn next(item:Item)->Item:\n  item with {value:item.value+1}\n'
(repo / "public.k").write_text(public)
(repo / "literal.txt").write_text("$Format:%H$\n")
(repo / "retained.txt").write_text("retained\n")
(repo / ".gitattributes").write_text("literal.txt export-subst\nretained.txt export-ignore\n*.k filter=attack\n")
(repo / "tool.sh").write_text("#!/bin/sh\nexit 99\n")
(repo / "tool.sh").chmod(0o755)
first = commit(repo)
(repo / "public.k").write_text(public.replace("value+1", "value+2"))
(repo / "kalandra.toml").write_text((repo / "kalandra.toml").read_text().replace("0.1.0", "0.2.0"))
second = commit(repo)
# Packed bare repository is a separate resolved repository with identical source.
subprocess.run(["/usr/bin/git", "clone", "--bare", "--no-hardlinks", str(repo), str(root / "bare")],
               env=env, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
git(root / "bare", "repack", "-ad")
telemetry = initialize("telemetry")
(telemetry / "kalandra.toml").write_text("format=1\n[package]\nname='telemetry'\nversion='1.0.0'\nentry='public.k'\n")
(telemetry / "public.k").write_text('from "geometry" import Item, next\nexport fn compute(n:int64)->int64:\n  next(Item {value:n}).value\n')
third = commit(telemetry)
(repo / "link.k").symlink_to("public.k")
symlink = commit(repo)
(repo / "link.k").unlink()
git(repo, "add", "--all")
git(repo, "update-index", "--add", "--cacheinfo", f"160000,{first},submodule")
git(repo, "commit", "-m", "gitlink")
gitlink = git(repo, "rev-parse", "HEAD")
git(repo, "update-index", "--force-remove", "submodule")
(repo / "kalandra.toml").write_text("format=1\n[package]\nname='broken'\nversion='bad'\nentry='public.k'\n")
invalid = commit(repo)
blob = git(repo, "rev-parse", f"{first}:public.k")
# Tree with directory spellings that alias on the macOS volume.
leaf = git(repo, "mktree", data=f"100644 blob {blob}\tvalue.k\n".encode())
other_leaf = git(repo, "mktree", data=f"100644 blob {blob}\tother.k\n".encode())
manifest = git(repo, "rev-parse", f"{first}:kalandra.toml")
collision_tree = git(repo, "mktree", data=(f"040000 tree {leaf}\tCase\n040000 tree {other_leaf}\tcase\n"
                                                   f"100644 blob {manifest}\tkalandra.toml\n").encode())
collision = git(repo, "commit-tree", collision_tree, data=b"case collision\n")
(root / "commits").write_text("\n".join([first, second, third, symlink, gitlink, invalid, blob, collision]) + "\n")
shutil.copytree(repo, root / "alternate", symlinks=True)
(root / "alternate/.git/objects/info/alternates").write_text(str(repo / ".git/objects") + "\n")
shutil.copytree(repo, root / "broken", symlinks=True)
damaged = root / "broken/.git/objects" / first[:2] / first[2:]
damaged.chmod(0o600)
damaged.write_bytes(b"corrupt")
# Source config is deliberately unsafe. The importer must never read it.
attack = f"!touch '{root / 'executed'}'; false"
with (repo / ".git/config").open("a") as config:
    config.write(f"\n[filter \"attack\"]\n smudge = {attack}\n required = true\n"
                 f"[uploadpack]\n packObjectsHook = {attack}\n[include]\n path = /nonexistent/kalandra-config\n")
run_env = dict(os.environ)
run_env.update({"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "core.fsmonitor",
                "GIT_CONFIG_VALUE_0": attack, "GIT_CONFIG_GLOBAL": "/nonexistent/kalandra-config"})
subprocess.run([binary, str(root), "all", "cache"], env=run_env, check=True, timeout=120)
# Competing publications must converge without replacing an existing directory.
a = subprocess.Popen([binary, str(root), "race", "race-cache"], env=run_env, stdout=subprocess.PIPE)
b = subprocess.Popen([binary, str(root), "race", "race-cache"], env=run_env, stdout=subprocess.PIPE)
a_out, _ = a.communicate(timeout=60)
b_out, _ = b.communicate(timeout=60)
assert a.returncode == b.returncode == 0 and a_out == b_out
assert not list((root / "race-cache").glob(".kalandra-snapshot-*"))
repo.rename(root / "unavailable-repository")
subprocess.run([binary, str(root), "offline", "cache"], env=run_env, check=True, timeout=30)
assert not (root / "executed").exists()
