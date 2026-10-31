"""Prepare the pinned official Pi OS ARM64 userspace on a disposable ARM runner.

This executes native ARM64 programs, but does not qualify Raspberry Pi hardware,
firmware, or its 16 KiB kernel. The existing downloaded-package harness runs
inside the imported root filesystem, without the repository or system LLVM.
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import urllib.request

IMAGE = "2026-10-06-raspios-trixie-arm64-lite.img.xz"
URL = "https://downloads.raspberrypi.com/raspios_lite_arm64/images/raspios_lite_arm64-2026-10-06/" + IMAGE
SHA256 = "483db18a48da399b5b7022ffae9a07bc6d99daab30e593aedac09b69ff422843"


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def main():
    if os.environ.get("GITHUB_ACTIONS") != "true" or platform.machine().lower() not in ("aarch64", "arm64"):
        raise RuntimeError("Requires a disposable native ARM64 GitHub runner")
    repository = Path.cwd()
    with tempfile.TemporaryDirectory(dir=os.environ["RUNNER_TEMP"]) as temporary:
        work = Path(temporary)
        archive = work / IMAGE
        with urllib.request.urlopen(URL, timeout=120) as response, archive.open("wb") as output:
            shutil.copyfileobj(response, output)
        with archive.open("rb") as source:
            if hashlib.file_digest(source, "sha256").hexdigest() != SHA256:
                raise RuntimeError("Pi OS image checksum mismatch")
        run("xz", "-d", str(archive))
        image = archive.with_suffix("")
        loop = subprocess.check_output(["sudo", "losetup", "--find", "--show", "--partscan", "--read-only", str(image)], text=True).strip()
        root = work / "root"
        root.mkdir()
        try:
            table = json.loads(subprocess.check_output(["sudo", "sfdisk", "--json", loop], text=True))["partitiontable"]
            roots = [p["node"] for p in table["partitions"] if p.get("type") == "83"]
            if len(roots) != 1:
                raise RuntimeError("Unexpected Pi OS partition layout")
            run("sudo", "mount", "-o", "ro,noload", roots[0], str(root))
            try:
                with (work / "root.tar").open("wb") as output:
                    run("sudo", "tar", "-C", str(root), "-cf", "-", ".", stdout=output)
                run("docker", "import", str(work / "root.tar"), "kalandra-pi-os:qualification")
            finally:
                run("sudo", "umount", str(root))
        finally:
            run("sudo", "losetup", "--detach", loop)
        # Install only documented system prerequisites; no LLVM/Clang package.
        run("docker", "run", "--name", "kalandra-pi-prerequisites", "kalandra-pi-os:qualification", 
            "/bin/sh", "-ec", "apt-get update && apt-get install -y --no-install-recommends python3 g++ binutils git ca-certificates patchelf bubblewrap")
        run("docker", "commit", "kalandra-pi-prerequisites", "kalandra-pi-os:ready")
        tests = work / "qualification"
        (tests / "native").mkdir(parents=True)
        shutil.copytree(repository / "native/packaging", tests / "native/packaging", ignore=shutil.ignore_patterns("__pycache__"))
        shutil.copytree(repository / "native/lsp", tests / "native/lsp", ignore=shutil.ignore_patterns("__pycache__"))
        shutil.copy2(repository / "VERSION", tests)
        # Privileged only to permit nested bubblewrap namespaces. Network is off,
        # inputs are read-only, no credentials or project checkout are mounted.
        run("docker", "run", "--rm", "--privileged", "--network=none", "--tmpfs", "/tmp:exec",
            "-v", f"{tests}:/qualification:ro", "-v", f"{repository / 'downloaded'}:/packages:ro",
            "kalandra-pi-os:ready", "python3", "/qualification/native/packaging/PortablePackageTests.py",
            "/packages/kalandra-native-cpu-standalone-linux-arm64.tar.gz")


if __name__ == "__main__":
    main()
