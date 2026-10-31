"""Run the installed VSIX against the downloaded ARM64 server in native VS Code."""
import hashlib
from http.client import IncompleteRead
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile

from provision_native_llvm import save_verified

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "native/packaging"))
from WindowsPlatform import MACHINES, pe_machine


def download_editor(lock, destination):
    for attempt in range(3):
        try:
            with urllib.request.urlopen(lock["url"], timeout=120) as response:
                save_verified(response, destination, lock["sha256"])
            return
        except (OSError, IncompleteRead, ValueError) as error:
            print(f"VS Code download attempt {attempt + 1}/3 failed: {error}", flush=True)
            if attempt == 2:
                raise
            time.sleep(2 ** attempt)


def remove_editor_workspace(path):
    # Electron/Windows can retain handles briefly after the extension host
    # exits. Avoid tempfile's recursive permission-error handler, and never
    # turn a permanent resource leak into a passing qualification.
    for attempt in range(8):
        try:
            shutil.rmtree(path)
            return
        except FileNotFoundError:
            return
        except OSError as error:
            if getattr(error, 'winerror', None) not in (5, 32) or attempt == 7:
                raise
            print(f"Editor cleanup sharing retry {attempt + 1}/7: {error.winerror}", flush=True)
            time.sleep(0.1 * 2 ** attempt)


def main():
    if (os.environ.get("GITHUB_ACTIONS") != "true" or platform.system() != "Windows" or
            platform.machine().lower() not in ("arm64", "aarch64")):
        raise RuntimeError("Requires a disposable native Windows ARM64 GitHub runner")
    editor = ROOT / "native/editor/vscode"
    lock = json.loads((editor / "windows-arm64-test.json").read_text())
    archive = ROOT / "downloaded/kalandra-native-cpu-standalone-windows-arm64.tar.gz"
    with archive.open("rb") as source:
        if hashlib.file_digest(source, "sha256").hexdigest() != archive.with_suffix(".gz.sha256").read_text().split()[0]:
            raise RuntimeError("Downloaded compiler checksum mismatch")
    folder = tempfile.mkdtemp(prefix="kalandra ARM64 editor ", dir=os.environ["RUNNER_TEMP"])
    try:
        work = Path(folder)
        package_root = work / "toolchain"
        package_root.mkdir()
        with tarfile.open(archive) as package:
            members = package.getmembers()
            if (not all(m.isdir() or m.isfile() for m in members) or
                    len({Path(m.name).parts[0] for m in members}) != 1):
                raise RuntimeError("Unexpected downloaded package layout")
            package.extractall(package_root, filter="data")
        prefix, = package_root.iterdir()
        server = prefix / "bin/kalandra-lsp.exe"
        code_archive = work / "vscode.zip"
        download_editor(lock, code_archive)
        code_root = work / "vscode"
        with zipfile.ZipFile(code_archive) as package:
            package.extractall(code_root)
        executable = code_root / "Code.exe"
        for binary in (server, executable):
            if pe_machine(binary) != MACHINES["windows-arm64"]:
                raise RuntimeError(f"Editor integration requires native ARM64: {binary}")
        environment = dict(os.environ, KALANDRA_LSP=str(server), KALANDRA_VSCODE_EXECUTABLE=str(executable))
        subprocess.run(["bun", "install", "--frozen-lockfile"], cwd=editor, env=environment, check=True)
        subprocess.run(["bun", "run", "test"], cwd=editor, env=environment, check=True)
        subprocess.run(["bun", "run", "test:editor"], cwd=editor, env=environment, check=True, timeout=600)
        result = json.loads((ROOT / "build/native/editor/qualification.json").read_text())
        if (result.get("platform") != "win32" or result.get("architecture") != "arm64" or
                result.get("vscode") != lock["version"] or result.get("installedVsix") is not True):
            raise RuntimeError("Editor qualification identity mismatch")
        print(f"Native Windows ARM64 VS Code {lock['version']}: {len(result['checks'])} installed-VSIX checks passed", flush=True)
    finally:
        remove_editor_workspace(Path(folder))


if __name__ == "__main__":
    main()
