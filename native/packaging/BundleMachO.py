"""Vendor the licensed Mach-O dependency closure into a staged macOS package.

Only staging copies are changed. Every non-system load is resolved before it is
rewritten to @loader_path; unresolved/unknown dependencies stop publication.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess


def run(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f"{args}: {result.stderr or result.stdout}")
    return result.stdout


def system(path):
    return os.path.normpath(path).startswith(("/usr/lib/", "/System/Library/"))


def macho(path):
    if path.suffix == ".a":  # Universal static archives share the fat Mach-O magic.
        return False
    with path.open("rb") as stream:
        return stream.read(4) in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
                                  b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def inspect(path):
    loads, rpaths, identity, minimum = [], [], None, (0,)
    for command in run("/usr/bin/otool", "-arch", "arm64", "-l", path).split("Load command")[1:]:
        kind = re.search(r"\bcmd (\S+)", command)[1]
        if kind in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
                    "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB", "LC_ID_DYLIB"):
            name = re.search(r"\n\s+name (.*?) \(offset", command)[1]
            if kind == "LC_ID_DYLIB":
                identity = name
            else:
                loads.append(name)
        elif kind == "LC_RPATH":
            rpaths.append(re.search(r"\n\s+path (.*?) \(offset", command)[1])
        elif kind in ("LC_BUILD_VERSION", "LC_VERSION_MIN_MACOSX"):
            if kind == "LC_BUILD_VERSION" and re.search(r"\n\s+platform (\d+)", command)[1] != "1":
                raise RuntimeError(f"Not a macOS Mach-O: {path}")
            version = re.search(r"\n\s+(?:minos|version) ([\d.]+)", command)[1]
            minimum = tuple(map(int, version.split(".")))
    if minimum == (0,):
        raise RuntimeError(f"Missing macOS deployment target: {path}")
    return loads, rpaths, identity, minimum


def expand(value, loader, executable):
    return value.replace("@loader_path", str(loader.parent)).replace(
        "@executable_path", str(executable.parent))


def resolve(name, source, executable, rpaths):
    if system(name):
        return None
    if name.startswith("@rpath/"):
        candidates = [Path(base) / name[len("@rpath/"):] for base in rpaths]
    else:
        candidates = [Path(expand(name, source, executable))]
    for candidate in candidates:
        if system(str(candidate)):
            # System libraries may reside solely in the dyld shared cache.
            return str(candidate)
        if candidate.is_absolute() and candidate.is_file():
            return candidate.resolve()
    raise RuntimeError(f"Unresolved load {name!r} in {source}")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def bundle(prefix, llvm, cpu=False, cross=None, spirv=None):
    prefix, llvm = prefix.resolve(), llvm.resolve()
    metal = cross is not None or spirv is not None
    if metal and (not cpu or cross is None or spirv is None):
        raise ValueError("Metal bundling requires CPU, SPIRV-Cross and SPIRV-Tools")
    extra_roots = {}
    library = prefix / "lib/kalandra/toolchain/lib"
    library.mkdir(parents=True)
    licenses = prefix / "share/licenses/kalandra-toolchain"
    licenses.mkdir(parents=True)
    shutil.copy2(llvm / "LICENSE.TXT", licenses / "LLVM-LICENSE.txt")
    seeds = [(path, path) for path in sorted((prefix / "bin").iterdir()) if macho(path)]
    if cpu:
        clang = prefix / "libexec/kalandra/clang++"
        clang.parent.mkdir(parents=True)
        original = (llvm / "bin/clang++").resolve()
        shutil.copy2(original, clang)
        resources = Path(run(llvm / "bin/clang++", "-print-resource-dir").strip()).resolve()
        if not resources.is_relative_to(llvm):
            raise RuntimeError("Clang resources are outside the pinned LLVM installation")
        destination = library.parent / "clang"
        shutil.copytree(resources, destination)
        seeds.append((original, clang))
        seeds.extend((p, destination / p.relative_to(resources))
                     for p in sorted(resources.rglob("*")) if p.is_file() and macho(p))
    if metal:
        cross, spirv = cross.resolve(), spirv.resolve()
        extra_roots = {cross: "SPIRV-Cross", spirv: "SPIRV-Tools"}
        for root, upstream in extra_roots.items():
            shutil.copy2(root / "LICENSE", licenses / f"{upstream}-LICENSE.txt")
        for root, name in ((llvm, "mlir-opt"), (llvm, "mlir-translate"),
                           (cross, "spirv-cross"), (spirv, "spirv-val")):
            original = (root / "bin" / name).resolve()
            destination = prefix / "libexec/kalandra" / name
            shutil.copy2(original, destination)
            seeds.append((original, destination))
        seeds.extend((p, p) for p in sorted((prefix / "lib/kalandra/native").iterdir())
                     if p.is_file() and macho(p))

    copied, origins, edits, metadata = {}, {}, {}, []
    minimum = (0,)

    def copy_dependency(source):
        if source in copied:
            return copied[source]
        if source.is_relative_to(llvm):
            upstream = "LLVM"
        elif any(source.is_relative_to(root) for root in extra_roots):
            upstream = next(upstream for root, upstream in extra_roots.items()
                            if source.is_relative_to(root))
        else:
            home = source.parent.parent
            if source.name.startswith("libz3.") and (home / "LICENSE.txt").is_file():
                upstream, files = "Z3", [home / "LICENSE.txt"]
            elif source.name.startswith("libzstd.") and (home / "LICENSE").is_file():
                upstream, files = "Zstandard", [home / "LICENSE", home / "COPYING"]
            else:
                raise RuntimeError(f"Dependency has no approved license packaging rule: {source}")
            for file in files:
                shutil.copy2(file, licenses / f"{upstream}-{file.name}")
        destination = library / source.name
        if destination in origins and origins[destination] != source:
            raise RuntimeError(f"Dependency basename collision: {source} and {origins[destination]}")
        origins[destination] = source
        shutil.copy2(source, destination)
        copied[source] = destination
        metadata.append({"name": source.name, "upstream": upstream,
                         "sourceSHA256": digest(source),
                         "path": str(destination.relative_to(prefix))})
        return destination

    def visit(source, destination, executable, inherited=()):
        nonlocal minimum
        loads, paths, identity, version = inspect(source)
        minimum = max(minimum, version)
        paths = [expand(p, source, executable) for p in paths] + list(inherited)
        changes, dependencies = {}, []
        for name in loads:
            resolved = resolve(name, source, executable, paths)
            if isinstance(resolved, Path):
                target = copy_dependency(resolved)
                changes[name] = "@loader_path/" + os.path.relpath(target, destination.parent)
                dependencies.append((resolved, target))
            elif resolved:
                changes[name] = resolved
        plan = (changes, identity, tuple(inspect(source)[1]))
        if destination in edits:
            if edits[destination] != plan:
                raise RuntimeError(f"Context-dependent dylib resolution for {source}")
            return
        edits[destination] = plan
        for original, target in dependencies:
            visit(original, target, executable, paths)

    for source, destination in seeds:
        visit(source, destination, source)
    for destination, (changes, identity, paths) in edits.items():
        destination.chmod(destination.stat().st_mode | stat.S_IWUSR)
        architectures = run("/usr/bin/lipo", "-archs", destination).split()
        if "arm64" not in architectures:
            raise RuntimeError(f"Expected an arm64 Mach-O: {destination}")
        if architectures != ["arm64"]:
            temporary = destination.with_name(destination.name + ".arm64")
            run("/usr/bin/lipo", destination, "-thin", "arm64", "-output", temporary)
            temporary.replace(destination)
        args = ["/usr/bin/install_name_tool"]
        for old, new in changes.items():
            args += ["-change", old, new]
        for path in dict.fromkeys(paths):
            args += ["-delete_rpath", path]
        if identity:
            args += ["-id", "@rpath/" + destination.name]
        if len(args) > 1:
            run(*args, destination)
        run("/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", destination)
        run("/usr/bin/codesign", "--verify", "--strict", destination)
        for load in inspect(destination)[0]:
            if not system(load):
                if not load.startswith("@loader_path/"):
                    raise RuntimeError(f"Non-relative bundled load: {destination}: {load}")
                target = (destination.parent / load.removeprefix("@loader_path/")).resolve()
                if not target.is_file() or not target.is_relative_to(prefix):
                    raise RuntimeError(f"Bundled load escapes package: {destination}: {load}")
    for item in metadata:
        item["bundledSHA256"] = digest(prefix / item["path"])
    record = {"format": 1, "architecture": "arm64", "minimumMacOS": ".".join(map(str, minimum)),
              "llvmRuntime": "bundled", "clang": "bundled" if cpu else "not included",
              "codeSignature": "ad-hoc", "notarized": False,
              "dependencies": sorted(metadata, key=lambda item: item["name"]),
              "externalRequirements": ["macOS system libraries"] +
              (["Apple command-line tools / SDK and system linker"] if cpu else [])}
    if metal:
        record["metal"] = "bundled shader translation tools; external Apple Metal compiler/SDK"
        record["externalRequirements"].append("Pinned Apple Metal compiler from Xcode's Metal Toolchain component")
    (library.parent / "standalone.json").write_text(json.dumps(record, indent=2) + "\n")
    installed = prefix / "share/kalandra-native-tools/toolchain.json"
    info = json.loads(installed.read_text())
    info["llvmRuntime"] = "bundled"
    info["minimumMacOS"] = record["minimumMacOS"]
    installed.write_text(json.dumps(info, indent=2) + "\n")
    shutil.copy2(Path(__file__).with_name("STANDALONE-README.md"),
                 prefix / "share/doc/kalandra-native-tools/README.md")
    if cpu:
        shutil.copy2(Path(__file__).with_name("STANDALONE-README.md"),
                     prefix / "share/doc/kalandra-native-cpu/README.md")
    if metal:
        installed = prefix / "share/kalandra-native-metal/toolchain.json"
        shutil.copy2(Path(__file__).with_name("STANDALONE-README.md"),
                     prefix / "share/doc/kalandra-native-metal/README.md")
        info = json.loads(installed.read_text())
        for key in ("spirvCross", "spirvTools", "metalCompiler", "macosSdk"):
            info.pop(key, None)
        info.update(spirvTools="bundled", spirvCross="bundled", clang="bundled",
                    metalCompiler="resolved and version-checked on the installed machine",
                    macosSdk="resolved by xcrun on the installed machine",
                    minimumMacOS=record["minimumMacOS"], externalDependencies=["Apple Metal compiler", "Apple SDK/linker"])
        installed.write_text(json.dumps(info, indent=2) + "\n")
    print(f"Bundled {len(metadata)} libraries and {len(edits)} Mach-O files; macOS >= {record['minimumMacOS']}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--llvm", type=Path, required=True)
    parser.add_argument("--cpu", action="store_true")
    parser.add_argument("--spirv-cross", type=Path)
    parser.add_argument("--spirv-tools", type=Path)
    args = parser.parse_args()
    bundle(args.prefix, args.llvm, args.cpu, args.spirv_cross, args.spirv_tools)
