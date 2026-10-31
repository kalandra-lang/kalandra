"""Bundle and audit the Linux ELF closure; keep glibc and system C++ runtimes external."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from BundleMachO import digest, run

PLATFORM = re.compile(r"^(?:linux-vdso\.so\.1|ld-linux-x86-64\.so\.2|ld-linux-aarch64\.so\.1|lib(?:c|m|pthread|dl|rt|resolv|util|gcc_s|stdc\+\+)\.so(?:\..*)?)$")
LICENSED = {"libz.so": "zlib1g", "libzstd.so": "libzstd1", "libxml2.so": "libxml2",
            "libffi.so": "libffi8", "libtinfo.so": "libtinfo6", "libncursesw.so": "libncursesw6",
            "libedit.so": "libedit2", "libz3.so": "libz3-4", "liblzma.so": "liblzma5",
            "libbsd.so": "libbsd0", "libmd.so": "libmd0"}


def elf(path):
    if not path.is_file():
        return False
    with path.open("rb") as stream:
        header = stream.read(18)
        return len(header) == 18 and header[:4] == b"\x7fELF" and header[5] == 1 and int.from_bytes(header[16:18], "little") in (2, 3)


def expected_machine(prefix):
    info = json.loads((prefix / "share/kalandra-native-tools/toolchain.json").read_text())
    try:
        return {"linux-x86_64": 62, "linux-arm64": 183}[info["platform"]]
    except KeyError as error:
        raise RuntimeError("Unsupported ELF package platform") from error


def check_machine(path, machine):
    with path.open("rb") as stream:
        header = stream.read(64)
        if (len(header) != 64 or header[:6] != b"\x7fELF\x02\x01" or
                int.from_bytes(header[18:20], "little") != machine):
            raise RuntimeError(f"Wrong ELF architecture: {path}")
        if machine != 183:
            return
        # Pi 5 kernels can use 16 KiB pages. Every LOAD segment, including the
        # bundled SDK and sanitizer DSOs, must permit that mapping granularity.
        offset = int.from_bytes(header[32:40], "little")
        size = int.from_bytes(header[54:56], "little")
        count = int.from_bytes(header[56:58], "little")
        loads = 0
        if size < 56 or not count:
            raise RuntimeError(f"Invalid ELF program headers: {path}")
        for index in range(count):
            stream.seek(offset + index * size)
            segment = stream.read(56)
            if len(segment) != 56:
                raise RuntimeError(f"Truncated ELF program headers: {path}")
            if int.from_bytes(segment[:4], "little") != 1:
                continue
            loads += 1
            file_offset = int.from_bytes(segment[8:16], "little")
            address = int.from_bytes(segment[16:24], "little")
            alignment = int.from_bytes(segment[48:56], "little")
            if alignment < 16384 or (address - file_offset) % 16384:
                raise RuntimeError(f"ELF LOAD segment is incompatible with 16 KiB Pi pages: {path}")
        if not loads:
            raise RuntimeError(f"ELF has no LOAD segments: {path}")


def dependencies(path, sdk=None):
    environment = dict(os.environ)
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT"):
        environment.pop(key, None)
    if sdk:
        environment["LD_LIBRARY_PATH"] = str(sdk / "lib")
    checked = subprocess.run(["ldd", str(path)], capture_output=True, text=True,
                             timeout=120, env=environment)
    if checked.returncode:
        raise RuntimeError(f"Cannot inspect ELF dependencies in {path}: {checked.stderr}")
    output = checked.stdout
    if "not found" in output:
        raise RuntimeError(f"Unresolved ELF dependency in {path}: {output}")
    result = {}
    for line in output.splitlines():
        match = re.match(r"\s*(\S+) => (/.+) \(0x[0-9a-fA-F]+\)\s*$", line)
        if match:
            result[match[1]] = Path(match[2]).resolve()
        elif line.strip() and "=>" in line:
            raise RuntimeError(f"Unknown ELF load in {path}: {line}")
    return result


def audit(prefix):
    machine = expected_machine(prefix)
    for path in prefix.rglob("*"):
        if not elf(path):
            continue
        check_machine(path, machine)
        for name, dependency in dependencies(path).items():
            if not PLATFORM.fullmatch(name) and not dependency.is_relative_to(prefix):
                raise RuntimeError(f"ELF dependency escapes installed prefix: {path}: {dependency}")
        rpath = run("patchelf", "--print-rpath", path).strip()
        if any(not item.startswith("$ORIGIN") or not (path.parent / item.replace("$ORIGIN", ".")).resolve().is_relative_to(prefix)
               for item in rpath.split(":") if item):
            raise RuntimeError(f"Unsafe ELF search path: {path}: {rpath}")


def bundle(prefix, llvm):
    prefix, llvm = prefix.resolve(), llvm.resolve()
    machine = expected_machine(prefix)
    library = prefix / "lib/kalandra/toolchain/lib"
    library.mkdir(parents=True)
    licenses = prefix / "share/licenses/kalandra-toolchain"
    licenses.mkdir(parents=True)
    shutil.copy2(Path(__file__).resolve().parents[1] / "third-party/LLVM-23.1.3-LICENSE.txt", licenses / "LLVM-LICENSE.txt")
    clang = (llvm / "bin/clang++").resolve()
    destination = prefix / "libexec/kalandra/clang++"
    destination.parent.mkdir(parents=True)
    shutil.copy2(clang, destination)
    resources = Path(run(clang, "-print-resource-dir").strip()).resolve()
    if not resources.is_relative_to(llvm):
        raise RuntimeError("Clang resources escape verified LLVM root")
    resource_copy = library.parent / "clang"
    shutil.copytree(resources, resource_copy)
    queue = [(p, p) for p in (prefix / "bin").iterdir() if elf(p)] + [(clang, destination)]
    queue += [(p, resource_copy / p.relative_to(resources)) for p in resources.rglob("*") if elf(p)]
    visited, origins, metadata = set(), {}, []
    while queue:
        source, target = queue.pop()
        if target in visited:
            continue
        visited.add(target)
        check_machine(source, machine)
        for name, dependency in dependencies(source, llvm).items():
            if PLATFORM.fullmatch(name):
                continue
            copied = library / name
            if copied in origins:
                if origins[copied] != dependency:
                    raise RuntimeError(f"ELF basename collision: {name}")
                continue
            upstream = "LLVM"
            if not dependency.is_relative_to(llvm):
                package = next((package for stem, package in LICENSED.items() if name.startswith(stem + ".") or name == stem), None)
                if package is None:
                    raise RuntimeError(f"Unapproved external ELF dependency: {dependency}")
                license_file = Path("/usr/share/doc") / package / "copyright"
                if not license_file.is_file():
                    raise RuntimeError(f"Missing dependency copyright: {package}")
                shutil.copy2(license_file, licenses / f"{package}-copyright")
                upstream = package
            origins[copied] = dependency
            shutil.copy2(dependency, copied)
            metadata.append({"name": name, "upstream": upstream, "sourceSHA256": digest(dependency),
                             "path": str(copied.relative_to(prefix))})
            queue.append((dependency, copied))
        relative = os.path.relpath(library, target.parent)
        run("patchelf", "--set-rpath", "$ORIGIN" if relative == "." else "$ORIGIN/" + relative, target)
    audit(prefix)
    metadata_path = prefix / "share/kalandra-native-tools/toolchain.json"
    info = json.loads(metadata_path.read_text())
    info.update({"llvmRuntime": "bundled", "standalone": True, "minimumLinux": "Ubuntu 24.04 / glibc 2.39",
                 "platformRuntime": ["glibc", "libstdc++", "libgcc_s"], "libraries": sorted(metadata, key=lambda x: x["name"])})
    metadata_path.write_text(json.dumps(info, indent=2) + "\n")
    print(f"ELF closure audited: {len(visited)} objects, {len(metadata)} bundled libraries", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--llvm", type=Path, required=True)
    args = parser.parse_args()
    bundle(args.prefix, args.llvm)
