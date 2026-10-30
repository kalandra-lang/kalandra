"""New-project acceptance and nonreplacement under competing creators."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, checker, formatter = map(lambda p: str(Path(p).resolve()), sys.argv[1:])

def run(args, cwd, expected=0):
    result = subprocess.run(list(map(str, args)), cwd=cwd, capture_output=True, text=True, timeout=40)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result

with tempfile.TemporaryDirectory(prefix="kalandra starter ") as temporary:
    cwd = Path(temporary)
    project = cwd / "new project é"
    run([compiler, "new", project], cwd)
    files = sorted(p.relative_to(project).as_posix() for p in project.rglob("*") if p.is_file())
    assert files == ["kalandra.toml", "src/main.k", "src/math.k"]
    original = {p: (project / p).read_bytes() for p in files}
    run([checker, "--project", project / "kalandra.toml"], cwd)
    for source in project.glob("src/*.k"):
        run([formatter, "--check", source], cwd)
    for i, mode in enumerate(([], ["-O2"], ["--sanitize"], ["--thread-sanitize"])):
        app = cwd / f"program {i}"
        run([compiler, "--project", project / "kalandra.toml", "-o", app, *mode], cwd)
        assert run([app], cwd).stdout.strip() == "[2, 4, 6]"
    run([compiler, "new", project], cwd, 1)
    assert {p: (project / p).read_bytes() for p in files} == original
    for name, kind in (("empty", "directory"), ("file", "file"), ("link", "symlink"), ("dangling", "dangling")):
        target = cwd / name
        if kind == "directory": target.mkdir()
        elif kind == "file": target.write_text("keep")
        else: target.symlink_to(project if kind == "symlink" else cwd / "missing")
        run([compiler, "new", target], cwd, 1)
        assert target.is_symlink() if kind in ("symlink", "dangling") else target.exists()
    assert (cwd / "file").read_text() == "keep"
    assert not list((cwd / "empty").iterdir())
    assert {p: (project / p).read_bytes() for p in files} == original
    run([compiler, "new", str(cwd / "trailing") + "/"], cwd)
    assert (cwd / "trailing/src/main.k").is_file()
    run([compiler, "new", cwd / "missing parent/project"], cwd, 1)
    run([compiler, "new", cwd / "file/child"], cwd, 1)
    run([compiler, "new"], cwd, 2)
    run([compiler, "new", "--help"], cwd)
    run([compiler, "new", "extra", "argument"], cwd, 2)
    # Exactly one competing creator wins; the loser cannot merge/replace its output.
    destination = cwd / "race"
    commands = [[compiler, "new", str(destination)]] * 2
    processes = [subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for cmd in commands]
    outcomes = []
    for process in processes:
        process.communicate(timeout=30)
        outcomes.append(process.returncode)
    assert sorted(outcomes) == [0, 1]
    assert {p: (destination / p).read_bytes() for p in files} == original
    assert not list(cwd.glob(".kalandra-new-*")), "staging directory leaked"
print("Starter: typed parallel program, formatting, four build modes, no replacement, concurrent creation and staging cleanup passed")
