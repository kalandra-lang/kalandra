"""Provision hash-pinned SPIR-V tools and verify Apple's pinned Metal compiler."""
import json
import os
from pathlib import Path
import subprocess

from provision_native_llvm import download_bottle, require_ci


def main():
    require_ci()
    lock = json.loads(Path("native/metal-proof/toolchain.json").read_text())
    environment = dict(os.environ, HOMEBREW_DEVELOPER="1")
    for kind, bottle in lock["homebrewBottles"].items():
        path = download_bottle(bottle, Path(os.environ["RUNNER_TEMP"]) / "pinned-spirv")
        existing = subprocess.run(["brew", "list", "--versions", bottle["formula"]], capture_output=True)
        if existing.returncode == 0:
            subprocess.run(["brew", "uninstall", "--force", "--ignore-dependencies", bottle["formula"]], check=True)
        subprocess.run(["brew", "install", "--ignore-dependencies", "--force-bottle", str(path)],
                       env=environment, check=True)
        cellar = subprocess.check_output(["brew", "--cellar", bottle["formula"]], text=True).strip()
        root = Path(cellar) / bottle["version"]
        tool = root / "bin" / ("spirv-cross" if kind == "cross" else "spirv-val")
        subprocess.run([str(tool), "--revision" if kind == "cross" else "--version"], check=True)
        with open(os.environ["GITHUB_ENV"], "a") as destination:
            destination.write(f"KALANDRA_SPIRV_{kind.upper()}_ROOT={root}\n")
    subprocess.run(["xcodebuild", "-downloadComponent", "MetalToolchain"], check=True)
    metal = subprocess.check_output(["xcrun", "--find", "metal"], text=True).strip()
    subprocess.run(["codesign", "--verify", "--strict", "-R=anchor apple", metal], check=True)
    version = subprocess.check_output([metal, "--version"], text=True)
    if f"Apple metal version {lock['metalCompilerVersion']} " not in version:
        raise RuntimeError("Hosted Apple Metal compiler does not match the qualified pin")
    print(version, flush=True)
    with open(os.environ["GITHUB_ENV"], "a") as destination:
        destination.write(f"KALANDRA_METAL_COMPILER={metal}\n")


if __name__ == "__main__":
    main()
