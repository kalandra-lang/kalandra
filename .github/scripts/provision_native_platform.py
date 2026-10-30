"""Verify official LLVM release archives before portable CI qualification."""
import json
import os
from pathlib import Path
import platform
import subprocess
import tarfile
import urllib.request
from compression import zstd

from provision_native_llvm import save_verified, sha256


def main():
    if os.environ.get("GITHUB_ACTIONS") != "true":
        raise RuntimeError("This provisioner is restricted to disposable GitHub runners")
    lock = json.loads(Path("native/toolchain-proof/toolchain.json").read_text())
    key = {"Linux": "linux-x64", "Windows": "windows-x64"}.get(platform.system())
    if key is None or platform.machine().lower() not in ("amd64", "x86_64"):
        raise RuntimeError("Unqualified CI platform")
    asset = lock["officialArchives"][key]
    work = Path(os.environ["RUNNER_TEMP"]) / "kalandra-llvm"
    work.mkdir(exist_ok=True)
    installed = work / "installation"
    configs = list(installed.rglob("MLIRConfig.cmake")) if installed.exists() else []
    if len(configs) == 1:
        root = configs[0].parents[3]
        print(f"Restored cached LLVM/MLIR root: {root}", flush=True)
        with open(os.environ["GITHUB_ENV"], "a") as environment:
            environment.write(f"KALANDRA_LLVM_ROOT={root.as_posix()}\n")
            if key == "windows-x64":
                environment.write(f"CMAKE_PREFIX_PATH={(root / 'dependencies/x64-windows-static').as_posix()}\n")
        return
    archive = work / asset["name"]
    url = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{lock['llvmVersion']}/{asset['name']}"
    with urllib.request.urlopen(url, timeout=120) as response:
        save_verified(response, archive, asset["sha256"])
    print(f"Verified official archive: {archive.name} SHA-256 {sha256(archive)}", flush=True)
    print(f"Archive magic: {archive.open('rb').read(8).hex()}", flush=True)
    # LLVM uses long-window zstd compression; Python's default decoder window
    # cap may reject an otherwise valid, hash-verified release archive.
    with zstd.ZstdFile(archive, options={zstd.DecompressionParameter.window_log_max: 31}) as decoded:
        with tarfile.open(fileobj=decoded, mode="r|") as source:
            source.extractall(installed, filter="data")
    archive.unlink()
    configs = list(installed.rglob("MLIRConfig.cmake"))
    if not configs and key == "windows-x64":
        llvm_configs = list(installed.rglob("LLVMConfig.cmake"))
        if len(llvm_configs) != 1:
            raise RuntimeError("Official Windows LLVM archive lacks its development configuration")
        root = llvm_configs[0].parents[3]
        build_windows_mlir(root, work, lock)
        configs = list(installed.rglob("MLIRConfig.cmake"))
    if len(configs) != 1:
        raise RuntimeError(f"Official LLVM archive contains {len(configs)} MLIR development configurations")
    root = configs[0].parents[3]
    compiler = root / "bin" / ("clang.exe" if key == "windows-x64" else "clang")
    subprocess.run([str(compiler), "--version"], check=True)
    with open(os.environ["GITHUB_ENV"], "a") as environment:
        environment.write(f"KALANDRA_LLVM_ROOT={root.as_posix()}\n")
        if key == "windows-x64":
            environment.write(f"CMAKE_PREFIX_PATH={(root / 'dependencies/x64-windows-static').as_posix()}\n")
    print(f"LLVM/MLIR development root: {root}", flush=True)


def build_windows_mlir(root, work, lock):
    # LLVM's Windows release omits MLIR. Build the matching official source
    # against its installed LLVM development libraries, never an unpinned fork.
    source = lock["mlirSource"]
    archive = work / source["name"]
    url = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{lock['llvmVersion']}/{source['name']}"
    with urllib.request.urlopen(url, timeout=120) as response:
        save_verified(response, archive, source["sha256"])
    with tarfile.open(archive) as package:
        package.extractall(work / "source", filter="data")
    archive.unlink()
    mlir_source, = (work / "source").glob("*/mlir")
    compiler = root / "bin/clang.exe"
    for mode in ("address", "undefined", "thread"):
        probe = work / "sanitizer-probe.cpp"
        probe.write_text("int main() { return 0; }\n")
        result = subprocess.run([str(compiler), "-x", "c++", str(probe), f"-fsanitize={mode}",
                                 "-o", str(work / "sanitizer-probe.exe")], capture_output=True, text=True)
        print(f"Windows {mode} sanitizer compilation: {result.returncode == 0}", flush=True)
        if result.returncode:
            print(result.stderr, flush=True)
    dependencies = build_windows_dependencies(root, work, lock)
    build = work / "mlir-build"
    subprocess.run(["cmake", "-S", str(mlir_source), "-B", str(build), "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={root.as_posix()}",
                    f"-DCMAKE_C_COMPILER={compiler.as_posix()}",
                    f"-DCMAKE_CXX_COMPILER={(root / 'bin/clang++.exe').as_posix()}",
                    f"-DLLVM_DIR={(root / 'lib/cmake/llvm').as_posix()}",
                    f"-DCMAKE_PREFIX_PATH={dependencies.as_posix()}",
                    f"-DCMAKE_PROJECT_INCLUDE={Path('native/cmake/WindowsLLVMDependencies.cmake').resolve().as_posix()}",
                    f"-DCMAKE_TOOLCHAIN_FILE={(work / 'vcpkg/scripts/buildsystems/vcpkg.cmake').as_posix()}",
                    f"-DVCPKG_INSTALLED_DIR={(root / 'dependencies').as_posix()}",
                    "-DVCPKG_TARGET_TRIPLET=x64-windows-static", "-DVCPKG_MANIFEST_MODE=OFF",
                    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", "-DLLVM_USE_CRT_RELEASE=MT",
                    "-DCMAKE_C_FLAGS=-DLIBXML_STATIC", "-DCMAKE_CXX_FLAGS=-DLIBXML_STATIC",
                    "-DMLIR_INCLUDE_TESTS=OFF", "-DMLIR_INCLUDE_INTEGRATION_TESTS=OFF",
                    "-DMLIR_ENABLE_BINDINGS_PYTHON=OFF", "-DMLIR_ENABLE_EXECUTION_ENGINE=OFF",
                    "-DMLIR_BUILD_MLIR_DYLIB=OFF", "-DMLIR_BUILD_MLIR_C_DYLIB=OFF"], check=True)
    subprocess.run(["cmake", "--build", str(build), "--target", "install", "-j2"], check=True)


def build_windows_dependencies(root, work, lock):
    baseline = lock["windowsDependencies"]["vcpkgBaseline"]
    repository = work / "vcpkg"
    subprocess.run(["git", "clone", "--filter=blob:none", "--no-checkout",
                    "https://github.com/microsoft/vcpkg.git", str(repository)], check=True)
    subprocess.run(["git", "-C", str(repository), "checkout", "--detach", baseline], check=True)
    subprocess.run(["cmd", "/c", str(repository / "bootstrap-vcpkg.bat"), "-disableMetrics"], check=True)
    manifest = work / "dependencies-manifest"
    manifest.mkdir()
    (manifest / "vcpkg.json").write_text(json.dumps({
        "name": "kalandra-native-llvm-dependencies", "version-string": "1",
        "builtin-baseline": baseline,
        "dependencies": ["zlib", "zstd", "tomlplusplus", {"name": "libxml2", "default-features": False}]}))
    destination = root / "dependencies"
    subprocess.run([str(repository / "vcpkg.exe"), "install", "--triplet=x64-windows-static",
                    f"--x-install-root={destination}", f"--x-manifest-root={manifest}"], check=True)
    return destination / "x64-windows-static"


if __name__ == "__main__":
    main()
