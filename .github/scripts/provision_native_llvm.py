"""Verify the exact Homebrew LLVM bottle; installation is restricted to CI runners."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import urllib.parse
import urllib.request


def sha256(path):
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(chunk)
    return checksum.hexdigest()


def save_verified(response, destination, expected):
    """Never replace a cached bottle until its complete digest has been checked."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as target:
            temporary = Path(target.name)
            for chunk in iter(lambda: response.read(1024 * 1024), b""):
                target.write(chunk)
        if sha256(temporary) != expected:
            raise ValueError("Pinned LLVM bottle checksum mismatch")
        temporary.replace(destination)
    finally:
        if temporary and temporary.exists():
            temporary.unlink()


def download(lock, directory):
    bottle = lock["homebrewBottle"]
    if (bottle["formula"] != "llvm" or bottle["tag"] != "arm64_sequoia" or
            not re.fullmatch(re.escape(lock["llvmVersion"]) + r"(?:_\d+)?", bottle["version"]) or
            not re.fullmatch(r"[0-9a-f]{64}", bottle["sha256"])):
        raise ValueError("Invalid pinned macOS ARM64 LLVM bottle metadata")
    return download_bottle(bottle, directory)


def download_bottle(bottle, directory):
    formula = bottle["formula"]
    if (formula not in ("llvm", "spirv-cross", "spirv-tools") or
            bottle["tag"] != "arm64_sequoia" or
            not re.fullmatch(r"\d+(?:\.\d+){2,3}(?:_\d+)?", bottle["version"]) or
            not re.fullmatch(r"[0-9a-f]{64}", bottle["sha256"])):
        raise ValueError("Invalid pinned toolchain bottle metadata")
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{formula}--{bottle['version']}.{bottle['tag']}.bottle.tar.gz"
    if path.is_file() and sha256(path) == bottle["sha256"]:
        return path
    # This public-registry token is anonymous, scoped to this repository, and never logged.
    query = urllib.parse.urlencode({"service": "ghcr.io", "scope": f"repository:homebrew/core/{formula}:pull"})
    with urllib.request.urlopen("https://ghcr.io/token?" + query, timeout=120) as reply:
        token = json.load(reply)["token"]
    url = f"https://ghcr.io/v2/homebrew/core/{formula}/blobs/sha256:" + bottle["sha256"]
    request = urllib.request.Request(url, headers={"Authorization": "Bearer " + token})
    with urllib.request.urlopen(request, timeout=120) as reply:
        save_verified(reply, path, bottle["sha256"])
    return path


def require_ci():
    if (os.environ.get("GITHUB_ACTIONS") != "true" or platform.system() != "Darwin" or
            platform.machine() != "arm64"):
        raise RuntimeError("Managed installation is allowed only on GitHub macOS ARM64 runners")


def install(lock, bottle):
    require_ci()
    # The runner may already carry another LLVM version. Keep exactly one version
    # so Homebrew's opt loader paths cannot resolve to an unqualified LLVM library.
    existing = subprocess.run(["brew", "list", "--versions", "llvm"], capture_output=True)
    if existing.returncode == 0:
        subprocess.run(["brew", "uninstall", "--force", "--ignore-dependencies", "llvm"], check=True)
    # Homebrew disables local package paths by default. Its documented developer
    # mode permits this SHA-256-verified official bottle; scope it to this call.
    environment = dict(os.environ, HOMEBREW_DEVELOPER="1")
    subprocess.run(["brew", "install", "--ignore-dependencies", "--force-bottle", str(bottle)],
                   env=environment, check=True)
    cellar = subprocess.check_output(["brew", "--cellar", "llvm"], text=True).strip()
    root = Path(cellar) / lock["homebrewBottle"]["version"]
    version = subprocess.check_output([str(root / "bin/clang"), "--version"], text=True)
    reported = re.search(r"\bclang version ([0-9.]+)", version)
    if not reported or reported[1] != lock["llvmVersion"]:
        raise RuntimeError("Installed Clang does not match the qualified version")
    if not (root / "lib/cmake/mlir/MLIRConfig.cmake").is_file():
        raise RuntimeError("Pinned LLVM bottle does not contain MLIR")
    with open(os.environ["GITHUB_ENV"], "a") as environment:
        environment.write(f"KALANDRA_LLVM_ROOT={root}\n")
    print(f"Verified pinned LLVM/MLIR installation: {root}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=Path("native/toolchain-proof/toolchain.json"))
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--install", action="store_true")
    args = parser.parse_args()
    if args.install:
        require_ci()  # Refuse before network/filesystem work on a developer machine.
    lock = json.loads(args.lock.read_text())
    path = download(lock, args.directory)
    print(f"Verified bottle: {path.name}")
    if args.install:
        install(lock, path)
