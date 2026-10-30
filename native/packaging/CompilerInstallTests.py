"""CPU bundle acceptance: relocate, compile outside checkout, and reject missing resources."""
from VersionCheck import verify_package_version
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

cmake, build, config, packager = sys.argv[1:]
build = Path(build)

def run(args, cwd, expected=0):
    result = subprocess.run(list(map(str, args)), cwd=cwd, capture_output=True, text=True, timeout=45)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

with tempfile.TemporaryDirectory(prefix="kalandra cpu install ") as temporary:
    cwd = Path(temporary)
    output = cwd / "artifacts"
    run([cmake, f"-DBUILD_DIR={build}", f"-DCONFIG={config}", f"-DOUTPUT_DIR={output}",
         "-DPACKAGE_CPU=ON", "-P", packager], cwd)
    archive = output / "kalandra-native-cpu-macos-arm64.tar.gz"
    assert archive.with_suffix(".gz.sha256").read_text().split()[0] == digest(archive)
    with tarfile.open(archive) as bundle:
        assert all(member.isdir() or member.isfile() for member in bundle.getmembers())
        bundle.extractall(cwd / "relocated prefix", filter="data")
    shutil.rmtree(output)
    prefix = cwd / "relocated prefix/kalandra-native-cpu"
    verify_package_version(prefix)
    for line in (prefix / "SHA256SUMS").read_text().splitlines():
        expected, name = line.split("  ", 1)
        assert (prefix / name).resolve().is_relative_to(prefix.resolve())
        assert digest(prefix / name) == expected
    assert len(list((prefix / "lib/kalandra/native").glob("*.a"))) == 30
    assert not (prefix / "bin/kalandrac").exists()
    compiler = cwd / "compiler symlink"
    compiler.symlink_to(prefix / "bin/kalandra")
    starter = cwd / "installed starter"
    run([compiler, "new", starter], cwd)
    run([compiler, "--project", starter / "kalandra.toml", "-o", cwd / "starter program"], cwd)
    assert run([cwd / "starter program"], cwd).stdout.strip() == "[2, 4, 6]"
    cases = [(kind, "42", "42") for kind in ("int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64")]
    cases += [("float32", "1.5", "1.5"), ("float64", "1.5", "1.5"),
              ("Bool", "true", "true"), ("String", '"hello"', "hello")]
    # Every result-entry family and all three runtime variants must link after relocation.
    for mode in ([], ["--sanitize"], ["--thread-sanitize"]):
        for kind, value, expected in cases:
            source = cwd / "main.k"
            source.write_text(f"fn main() -> {kind}:\n  {value}\n")
            executable = cwd / "program"
            run([compiler, source, "-o", executable, "-O2", *mode], cwd)
            assert run([executable], cwd).stdout.strip() == expected
    (cwd / "library.k").write_text("export fn double(value: int) -> int:\n  value * 2\n")
    (cwd / "main.k").write_text('from "math" import double\nfn main() -> int:\n  double(21)\n')
    (cwd / "kalandra.toml").write_text('format=1\n[project]\nentry="main.k"\n[packages]\nmath="library.k"\n')
    run([compiler, "--project", cwd / "kalandra.toml", "-o", cwd / "program"], cwd)
    assert run([cwd / "program"], cwd).stdout.strip() == "42"
    deps = run(["otool", "-L", cwd / "program"], cwd).stdout
    assert "libLLVM" not in deps and "libMLIR" not in deps
    # Collection entry and bounded worker dispatch use the installed runtime too.
    (cwd / "main.k").write_text("fn double(value: int) -> int:\n  value * 2\nfn main() -> int ! TaskError:\n  values = try parallelMap([21], double)\n  values[0]\n")
    for mode in ([], ["--sanitize"], ["--thread-sanitize"]):
        run([compiler, cwd / "main.k", "-o", cwd / "program", *mode], cwd)
        assert run([cwd / "program"], cwd).stdout.strip() == "42"
        (cwd / "array.k").write_text("fn main() -> [int]:\n  [1, 2, 3]\n")
        run([compiler, cwd / "array.k", "-o", cwd / "program", *mode], cwd)
        assert "1" in run([cwd / "program"], cwd).stdout
        (cwd / "strings.k").write_text('fn main() -> [String]:\n  ["hello", "world"]\n')
        run([compiler, cwd / "strings.k", "-o", cwd / "program", *mode], cwd)
        assert "hello" in run([cwd / "program"], cwd).stdout
    # The relocated public compiler installs exact local dependencies, then builds
    # solely from the activated cache with the original Git repository absent.
    repository = cwd / "installed Git library"
    repository.mkdir()
    repository = repository.resolve()
    git = ["/usr/bin/env", "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null",
           "GIT_CONFIG_SYSTEM=/dev/null", "GIT_CONFIG_COUNT=0", "/usr/bin/git",
           "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
           "-c", "commit.gpgsign=false", "-C", repository]
    run([*git, "init", "--template=", "--initial-branch=main"], cwd)
    (repository / "kalandra.toml").write_text("format=1\n[package]\nname='installed'\nversion='0.1.0'\nentry='public.k'\n")
    (repository / "public.k").write_text("export fn next(n:int)->int:\n  n+1\n")
    run([*git, "add", "--all"], cwd)
    run([*git, "commit", "-m", "fixture"], cwd)
    revision = run([*git, "rev-parse", "HEAD"], cwd).stdout.strip()
    application = cwd / "installed dependency application"
    application.mkdir()
    (application / "main.k").write_text('from "installed" import next\nfn main()->int ! TaskError:\n  try parallel:\n    try await spawn next(41)\n')
    (application / "kalandra.toml").write_text("format=1\n[project]\nentry='main.k'\n[dependencies]\ninstalled={git=" + json.dumps(str(repository)) + ",rev=" + json.dumps(revision) + "}\n")
    run([compiler, "install", "--project", application / "kalandra.toml"], cwd)
    repository.rename(repository.with_name("unavailable Git library"))
    for mode in ([], ["--sanitize"], ["--thread-sanitize"]):
        run([compiler, "--project", application / "kalandra.toml", "-o", cwd / "program", *mode], cwd)
        assert run([cwd / "program"], cwd).stdout.strip() == "42"
    # Exercise targeted updates through the relocated public CLI, including
    # owner-local pins and failed activation preserving the prior lock.
    run([sys.executable, Path(__file__).resolve().parents[1] / "tests/PackageUpdateFixtures.py",
         "--compiler", compiler, "--checker", prefix / "bin/kalandra-check",
         "--root", cwd / "named dependency updates"], cwd)
    saved = (cwd / "program").read_bytes()
    runtime = prefix / "lib/kalandra/native/libkalandra-native-runtime.a"
    runtime.unlink()
    result = run([compiler, "--project", cwd / "kalandra.toml", "-o", cwd / "program"], cwd, 1)
    assert "native runtime archive is missing" in result.stderr
    assert (cwd / "program").read_bytes() == saved
    result = run([compiler, cwd / "main.k", "--target", "metal", "-o", cwd / "program"], cwd, 2)
    assert "supports CPU only" in result.stderr
    assert (cwd / "program").read_bytes() == saved
print("CPU bundle: relocation, symlink, 45 scalar/collection/worker sanitizer programs, project imports, exact dependency installation/offline workers, named updates, resource failures passed")
