"""Relocated artifacts with checkout/Homebrew access denied to native processes.

--archive also supports a separate CI runner which never installed LLVM.
This is a dependency-isolation gate, not a substitute for clean-machine testing.
"""
from VersionCheck import verify_package_version
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

from BundleMachO import inspect, macho, system


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def qualify(archive, kind, runtime, build=None, metal_execute=False):
    archive = archive.resolve()
    assert archive.with_suffix(".gz.sha256").read_text().split()[0] == digest(archive)
    name = "kalandra-native-" + ("editor-tools" if kind == "editor" else kind) + "-standalone"
    prefix = runtime / "relocated prefix λ" / name
    with tarfile.open(archive) as bundle:
        assert all(p.isdir() or p.isfile() for p in bundle.getmembers())
        assert all(Path(p.name).parts[0] == name for p in bundle.getmembers())
        bundle.extractall(prefix.parent, filter="data")
    return qualify_prefix(prefix, kind, runtime, build, metal_execute)


def qualify_prefix(prefix, kind, runtime, build=None, metal_execute=False):
    verify_package_version(prefix)
    files = {str(p.relative_to(prefix)) for p in prefix.rglob("*") if p.is_file()}
    listed = {}
    for line in (prefix / "SHA256SUMS").read_text().splitlines():
        checksum, path = line.split("  ", 1)
        assert path not in listed and (prefix / path).resolve().is_relative_to(prefix)
        assert digest(prefix / path) == checksum, path
        listed[path] = checksum
    assert files == set(listed) | {"SHA256SUMS"}
    record = json.loads((prefix / "lib/kalandra/toolchain/standalone.json").read_text())
    assert record["llvmRuntime"] == "bundled" and record["architecture"] == "arm64"
    assert {item["upstream"] for item in record["dependencies"]} >= {"LLVM", "Z3", "Zstandard"}
    assert all(digest(prefix / item["path"]) == item["bundledSHA256"] for item in record["dependencies"])
    minimum = (0,)
    for path in sorted(prefix.rglob("*")):
        if not path.is_file() or not macho(path):
            continue
        dependencies, rpaths, _, required = inspect(path)
        minimum = max(minimum, required)
        assert not rpaths, (path, rpaths)
        for dependency in dependencies:
            if system(dependency):
                continue
            assert dependency.startswith("@loader_path/"), (path, dependency)
            target = (path.parent / dependency.removeprefix("@loader_path/")).resolve()
            assert target.is_relative_to(prefix) and target.is_file(), (path, dependency)
        checked = subprocess.run(["/usr/bin/codesign", "--verify", "--strict", str(path)], capture_output=True)
        assert checked.returncode == 0, (path, checked.stderr)
    assert record["minimumMacOS"] == ".".join(map(str, minimum))
    assert tuple(map(int, platform.mac_ver()[0].split("."))) >= minimum

    # Parents run the Python tests, but native children cannot read the checkout,
    # the build tree, Homebrew, or any original private toolchain installation.
    repository = Path(__file__).resolve().parents[2]
    denied = {repository, Path("/opt/homebrew"), Path("/usr/local")}
    if build:
        denied.add(build.resolve())
    profile = runtime / "native.sb"
    profile.write_text('(version 1)\n(allow default)\n(deny network*)\n(deny file-read*\n' +
                       ''.join('  (subpath ' + json.dumps(str(p)) + ')\n' for p in sorted(denied)) + ')\n')
    environment = {k: v for k, v in os.environ.items() if not k.startswith("DYLD_")}
    environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    environment.pop("SDKROOT", None)
    environment.pop("CPATH", None)
    environment.pop("LIBRARY_PATH", None)

    def execute(args, expected=0, overrides=None):
        result = subprocess.run(["/usr/bin/sandbox-exec", "-f", str(profile), *map(str, args)],
                                env=environment | (overrides or {}), cwd=runtime, capture_output=True, text=True, timeout=120)
        assert (result.returncode == expected if expected is not None else result.returncode != 0), (
            args, result.returncode, result.stdout, result.stderr)
        return result

    forbidden = execute(["/bin/cat", repository / "LICENSE"], expected=1)
    assert "Operation not permitted" in forbidden.stderr, forbidden.stderr
    source = runtime / "main.k"
    source.write_text("fn main() -> int:\n  42\n")
    assert "Parsed" in execute([prefix / "bin/kalandra-parse", source]).stdout
    assert json.loads(execute([prefix / "bin/kalandra-check", "--diagnostics=json", source]).stdout)["ok"]
    execute([prefix / "bin/kalandra-fmt", "--check", source])
    wrapper = runtime / "server"
    wrapper.write_text("#!/bin/sh\nexec /usr/bin/sandbox-exec -f " + shlex.quote(str(profile)) +
                       " " + shlex.quote(str(prefix / "bin/kalandra-lsp")) + ' "$@"\n')
    wrapper.chmod(0o755)
    for script in ("ProtocolTests.py", "RecoveryProtocolTests.py"):
        result = subprocess.run([sys.executable, str(repository / "native/lsp" / script),
                                 str(wrapper), str(runtime / script)], capture_output=True,
                                text=True, timeout=90, env=environment)
        assert result.returncode == 0, (script, result.stdout, result.stderr)
    if kind in ("cpu", "metal"):
        compiler = prefix / "bin/kalandra"
        executable = runtime / "program"
        link = runtime / "compiler symlink"
        link.symlink_to(compiler)
        cases = [(kind, "42", "42") for kind in
                 ("int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64")]
        cases += [("float32", "1.5", "1.5"), ("float64", "1.5", "1.5"),
                  ("Bool", "true", "true"), ("String", '"bundled"', "bundled"),
                  ("[int]", "[1, 2]", "[1, 2]"), ("[String]", '["bundled"]', '["bundled"]')]
        for mode in ([], ["--sanitize"], ["--thread-sanitize"]):
            for returned, body, expected in cases:
                source.write_text(f"fn main() -> {returned}:\n  {body}\n")
                execute([link, source, "-o", executable, "-O2", *mode])
                assert execute([executable]).stdout.strip() == expected
            source.write_text("fn double(value:int)->int:\n  value*2\nfn main()->int ! TaskError:\n"
                              "  values=try parallelMap([21],double)\n  values[0]\n")
            execute([compiler, source, "-o", executable, *mode])
            assert execute([executable]).stdout.strip() == "42"
        # Missing private tools must not silently use the development Clang.
        clang = prefix / "libexec/kalandra/clang++"
        clang.rename(clang.with_name("hidden-clang"))
        prior = digest(executable)
        assert "bundled Clang" in execute([compiler, source, "-o", executable], expected=1).stderr
        assert digest(executable) == prior
        clang.with_name("hidden-clang").rename(clang)
        execute([compiler, source, "-o", executable])
        loads = inspect(executable)[0]
        assert all(system(dependency) for dependency in loads), loads
        # Ordinary generated programs run with the entire compiler bundle absent.
        hidden = prefix.with_name("unavailable compiler bundle")
        prefix.rename(hidden)
        assert execute([executable]).stdout.strip() == "42"
        hidden.rename(prefix)
    if kind == "metal":
        assert {"SPIRV-Cross", "SPIRV-Tools"} <= {
            p.name.removesuffix("-LICENSE.txt") for p in (prefix / "share/licenses/kalandra-toolchain").iterdir()}
        info = json.loads((prefix / "share/kalandra-native-metal/toolchain.json").read_text())
        assert info["spirvCross"] == info["spirvTools"] == info["clang"] == "bundled"
        applications = []
        for numeric in ("int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64", "float32", "float64"):
            source.write_text(f"""fn increment(value:int32)->int32:
  value+1
fn operation(values:Tensor[{numeric},?,?,?,?,?,?,?,?],scale:{numeric})->float64:
  amount=reduceSum(values)
  if scale==1:
    float64(reduceSum(values+amount))
  else:
    float64(reduceSum(values)/scale)
fn main()->float64 ! TaskError:
  try parallel:
    worker=spawn increment(0)
    gpu=devices.gpu()
    values:Tensor[{numeric},1,1,1,1,1,1,1,3]=tensor([[[[[[[[1,2,3]]]]]]]])
    resident=try await transfer(values,gpu)
    scale=try await worker
    try await spawn operation(resident,{numeric}(scale)) on gpu
""")
            for mode, flags in (("normal", []), ("optimized", ["-O2"]),
                                ("sanitized", ["--sanitize"]), ("thread-sanitized", ["--thread-sanitize"])):
                app = runtime / f"{numeric} {mode} application"
                # Every numeric case intentionally returns float64, including
                # integer/float32 inputs. Metal requires an explicit fallback
                # for that conversion/result; float32 arithmetic stays hardware.
                execute([compiler, source, "--target", "metal", "--arithmetic",
                         "hardware-with-software-fallback", "-o", app, *flags])
                assert list(app.glob("*.metallib"))
                if not flags or flags == ["-O2"]:
                    assert all(system(dep) for dep in inspect(app / "program")[0])
                    applications.append((app, "4038000000000000"))
                if metal_execute:
                    assert execute([app / "program"]).stdout.strip() == "4038000000000000"
        source.write_bytes((repository / "native/metal-source/matrix-graph.k").read_bytes())
        app = runtime / "resident matrix application"
        execute([compiler, source, "--target", "metal", "-o", app, "-O2"])
        applications.append((app, "[42d40000,431c0000,43710000,43b10000]"))
        if metal_execute:
            assert execute([app / "program"]).stdout.strip() == applications[-1][1]
        prior = digest(app / "program")
        for missing in (prefix / "libexec/kalandra/mlir-opt",
                        prefix / "lib/kalandra/native/kalandra-metal-strict64"):
            hidden = missing.with_name(missing.name + ".hidden")
            missing.rename(hidden)
            execute([compiler, source, "--target", "metal", "-o", app], expected=1)
            assert digest(app / "program") == prior
            hidden.rename(missing)
        invalid = execute([compiler, source, "--target", "metal", "-o", app], expected=1,
                          overrides={"KALANDRA_METAL_COMPILER": "/usr/bin/true"})
        assert "version mismatch" in invalid.stderr
        assert digest(app / "program") == prior
        hidden = prefix.with_name("unavailable mixed compiler bundle")
        prefix.rename(hidden)
        if metal_execute:
            for app, expected in applications:
                assert execute([app / "program"]).stdout.strip() == expected
        hidden.rename(prefix)
    # A missing vendored dylib cannot fall back to a Homebrew copy.
    dependency = prefix / next(item["path"] for item in record["dependencies"]
                               if item["name"].startswith("libLLVM."))
    dependency.rename(dependency.with_name(dependency.name + ".hidden"))
    assert "Library not loaded" in execute([prefix / "bin/kalandra-check", source], expected=None).stderr
    dependency.with_name(dependency.name + ".hidden").rename(dependency)
    assert all(digest(prefix / path) == checksum for path, checksum in listed.items())
    checks = ["checksums", "code signatures", "loader confinement", "relocation",
              "denied checkout/Homebrew/network", "strict LSP protocols", "missing dylib rejection"]
    if kind in ("cpu", "metal"):
        checks += ["CPU/sanitizer/worker execution", "missing private Clang rejection",
                   "generated ordinary programs run without compiler bundle"]
    if kind == "metal":
        checks += ["all ten GPU types/rank-eight/worker bridges in four host modes",
                   "resident matrix graph", "missing shader tools and wrong Apple compiler rejection"]
        if metal_execute:
            checks.append("GPU execution and moved applications without compiler bundle")
    print(json.dumps({"kind": kind, "minimumMacOS": record["minimumMacOS"],
                      "dependencyLibraries": len(record["dependencies"]),
                      "gpuExecution": kind == "metal" and metal_execute,
                      "checks": checks}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--kind", choices=("editor", "cpu", "metal"), required=True)
    parser.add_argument("--metal-execute", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="kalandra standalone ") as temporary:
        work = Path(temporary).resolve()
        archive = args.archive
        if archive is None:
            assert args.build
            output = work / "artifacts"
            command = [args.cmake, f"-DBUILD_DIR={args.build.resolve()}", f"-DCONFIG={args.config}",
                       f"-DOUTPUT_DIR={output}", "-DSTANDALONE=ON"]
            if args.kind in ("cpu", "metal"):
                command.append("-DPACKAGE_CPU=ON")
            if args.kind == "metal":
                command.append("-DPACKAGE_METAL=ON")
            command += ["-P", str(Path(__file__).resolve().parents[1] / "cmake/PackageEditorTools.cmake")]
            subprocess.run(command, check=True, timeout=240)
            archive, = output.glob("*.tar.gz")
            other = work / "independent staging"
            repack = [f"-DOUTPUT_DIR={other}" if item.startswith("-DOUTPUT_DIR=") else item
                      for item in command]
            subprocess.run(repack, check=True, timeout=240)
            rebuilt, = other.glob("*.tar.gz")
            assert digest(archive) == digest(rebuilt), "repacking the same binaries changed archive bytes"
            shutil.rmtree(other)
            # Drop all staging resources before any relocated runtime tests.
            shutil.rmtree(output / "package-stage")
        qualify(archive, args.kind, work, args.build, args.metal_execute)
