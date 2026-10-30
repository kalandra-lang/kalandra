"""Install/package acceptance using only staged tools from an unrelated cwd."""
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

cmake, build, config, packager, protocol, llvm, compiler = sys.argv[1:]
build, llvm = Path(build), Path(llvm).resolve()
tools = ["kalandra-check", "kalandra-fmt", "kalandra-parse", "kalandra-lsp"]

def run(args, cwd, expected=0, env=None):
    result = subprocess.run(list(map(str, args)), cwd=cwd, env=env, capture_output=True, text=True, timeout=45)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

root = build / "native-editor-tools-acceptance"
root.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=root, prefix="install ") as temporary, tempfile.TemporaryDirectory(prefix="kalandra tool cwd ") as runtime:
    work = Path(temporary)
    cwd = Path(runtime)
    original = {tool: digest(build / "native" / tool) for tool in tools}
    # Explicit prefixes and DESTDIR must stay inside their respective staging roots.
    direct = work / "direct prefix"
    run([cmake, "--install", build, "--config", config, "--prefix", direct, "--component", "NativeEditorTools"], cwd)
    env = dict(os.environ, DESTDIR=str(work / "destdir"))
    run([cmake, "--install", build, "--config", config, "--prefix", "/kalandra-prefix", "--component", "NativeEditorTools"], cwd, env=env)
    assert all((work / "destdir/kalandra-prefix/bin" / tool).is_file() for tool in tools)
    assert {path.name for path in (direct / "bin").iterdir()} == set(tools)
    assert not (direct / "bin/kalandra").exists()
    project = cwd / "project"
    project.mkdir()
    source = "from \"math\" import double\nfn main() -> int:\n  double(21)\n"
    (project / "main.k").write_text(source)
    (project / "library.k").write_text("export fn double(value: int) -> int:\n  value * 2\n")
    manifest = project / "kalandra.toml"
    manifest.write_text('format=1\n[project]\nentry="main.k"\n[packages]\nmath="library.k"\n')
    result = run([direct / "bin/kalandra-check", "--diagnostics=json", "--project", manifest], cwd)
    assert json.loads(result.stdout)["ok"] is True
    run([direct / "bin/kalandra-fmt", "--check", project / "main.k"], cwd)
    assert "Parsed 1 functions" in run([direct / "bin/kalandra-parse", project / "main.k"], cwd).stdout
    # Package into its own stage, then remove that prefix and run the relocated archive.
    output = work / "artifacts"
    run([cmake, f"-DBUILD_DIR={build}", f"-DCONFIG={config}", f"-DOUTPUT_DIR={output}", "-P", packager], cwd,
        env=dict(os.environ, DESTDIR=str(work / "unexpected destdir")))
    assert not (work / "unexpected destdir").exists(), "packaging inherited DESTDIR"
    archive = output / "kalandra-native-editor-tools-macos-arm64.tar.gz"
    assert archive.with_suffix(archive.suffix + ".sha256").read_text().split()[0] == digest(archive)
    archive_hash = digest(archive)
    sidecar = archive.with_suffix(archive.suffix + ".sha256").read_bytes()
    run([cmake, f"-DBUILD_DIR={build}", "-DCONFIG=NotConfigured", f"-DOUTPUT_DIR={output}", "-P", packager], cwd, expected=1)
    assert digest(archive) == archive_hash and archive.with_suffix(archive.suffix + ".sha256").read_bytes() == sidecar, "failed package replaced a valid archive"
    extracted = work / "relocated package"
    with tarfile.open(archive, "r:gz") as bundle:
        assert all(member.isdir() or member.isfile() for member in bundle.getmembers())
        bundle.extractall(extracted, filter="data")
    shutil.rmtree(output / "package-stage")
    prefix = extracted / "kalandra-native-editor-tools"
    metadata = json.loads((prefix / "share/kalandra-native-tools/toolchain.json").read_text())
    verify_package_version(prefix)
    assert metadata["format"] == 1 and metadata["llvmVersion"] == json.loads((Path(__file__).parents[1] / "toolchain-proof/toolchain.json").read_text())["llvmVersion"]
    assert metadata["platform"] == "macos-arm64" and metadata["configuration"] == config
    assert set(metadata["tools"]) == set(tools) and metadata["llvmRuntime"] == "external"
    checksums = (prefix / "SHA256SUMS").read_text().splitlines()
    names = set()
    for line in checksums:
        expected, name = line.split("  ", 1)
        assert name not in names and (prefix / name).resolve().is_relative_to(prefix.resolve())
        names.add(name)
        assert digest(prefix / name) == expected, name
    files = {str(path.relative_to(prefix)) for path in prefix.rglob("*") if path.is_file()}
    assert names == files - {"SHA256SUMS"}
    assert len(names) == 10 and "share/licenses/kalandra-native-tools/LLVM-LICENSE.txt" in names
    for tool in tools:
        executable = prefix / "bin" / tool
        assert os.access(executable, os.X_OK)
        assert run(["lipo", "-archs", executable], cwd).stdout.strip() == "arm64"
        dependencies = run(["otool", "-L", executable], cwd).stdout.splitlines()[1:]
        for dependency in dependencies:
            name = dependency.strip().split(" (", 1)[0]
            assert name.startswith(("/usr/lib/", "/System/Library/")) or Path(name).resolve().is_relative_to(llvm / "lib"), name
    environment = dict(os.environ, PATH=str(prefix / "bin") + os.pathsep + os.environ.get("PATH", ""))
    assert shutil.which("kalandra-check", path=environment["PATH"]) == str(prefix / "bin/kalandra-check")
    run(["kalandra-check", "--project", manifest], cwd, env=environment)
    assert run(["kalandra-fmt", project / "main.k"], cwd, env=environment).stdout == source
    run(["kalandra-parse", project / "main.k"], cwd, env=environment)
    # The complete independent stdio suite checks the relocated real server.
    result = run([sys.executable, protocol, prefix / "bin/kalandra-lsp", work / "protocol"], cwd, env=environment)
    assert "Native LSP:" in result.stdout
    run([sys.executable, Path(protocol).with_name("PackageTests.py"),
         prefix / "bin/kalandra-lsp", compiler, work / "package-protocol"], cwd, env=environment)
    run([sys.executable, Path(protocol).with_name("RecoveryProtocolTests.py"),
         prefix / "bin/kalandra-lsp", work / "recovery-protocol"], cwd, env=environment)
    assert (project / "main.k").read_text() == source
    assert original == {tool: digest(build / "native" / tool) for tool in tools}, "install/strip changed build binaries"
print("Native editor tools: prefix/DESTDIR install, package checksums, relocation, runtime dependency audit, CLI and full stdio acceptance passed")
