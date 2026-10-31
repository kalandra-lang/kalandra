"""Bundle Windows PE imports from the pinned LLVM SDK, never the ambient PATH."""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct

from BundleMachO import digest, run
from WindowsPlatform import MACHINES, pe_machine

SYSTEM = {name + ".dll" for name in (
    "kernel32", "kernelbase", "ntdll", "advapi32", "user32", "gdi32", "shell32", "shlwapi",
    "ole32", "oleaut32", "combase", "ws2_32", "bcrypt", "crypt32", "secur32", "psapi",
    "dbghelp", "imagehlp", "version", "userenv", "iphlpapi", "rpcrt4", "msvcrt", "ucrtbase",
    "vcruntime140", "vcruntime140_1", "msvcp140", "concrt140", "normaliz", "winmm", "winhttp")}


def system(name):
    return name.lower() in SYSTEM or bool(re.fullmatch(r"(?:api|ext)-ms-[a-z0-9-]+\.dll", name.lower()))


def imports(path, expected_machine=None):
    actual = pe_machine(path)
    if expected_machine is not None and actual != expected_machine:
        raise ValueError(f"Wrong PE architecture: {path}")
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"Not a PE object: {path}")
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE signature: {path}")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if machine != actual or struct.unpack_from("<H", data, optional)[0] != 0x20b:
        raise ValueError(f"Invalid PE target: {path}")
    sections = []
    for index in range(count):
        start = optional + optional_size + index * 40
        virtual_size, virtual, size, raw = struct.unpack_from("<IIII", data, start + 8)
        sections.append((virtual, size, raw))

    def offset(rva):
        for virtual, size, raw in sections:
            if virtual <= rva < virtual + size and raw + rva - virtual < len(data):
                return raw + rva - virtual
        raise ValueError(f"PE import RVA is outside initialized sections: {path}")

    result = set()
    for directory, stride, name_offset in ((1, 20, 12), (13, 32, 4)):
        rva, size = struct.unpack_from("<II", data, optional + 112 + directory * 8)
        if not rva:
            continue
        for index in range(min(size // stride, 4096)):
            entry = offset(rva + index * stride)
            fields = data[entry:entry + stride]
            if not any(fields):
                break
            if directory == 13 and not struct.unpack_from("<I", fields)[0] & 1:
                raise ValueError("Legacy VA delay imports are unsupported")
            name_rva = struct.unpack_from("<I", fields, name_offset)[0]
            start = offset(name_rva)
            end = data.find(b"\0", start, start + 4096)
            if end < 0:
                raise ValueError("Unterminated PE import name")
            name = data[start:end].decode("ascii")
            if not re.fullmatch(r"[A-Za-z0-9_.+-]+\.dll", name, re.IGNORECASE):
                raise ValueError(f"Unsafe PE import name: {name}")
            result.add(name)
        else:
            raise ValueError("Unbounded PE import table")
    return sorted(result)


def package_machine(prefix):
    info = json.loads((prefix / "share/kalandra-native-tools/toolchain.json").read_text())
    try:
        return MACHINES[info["platform"]]
    except KeyError as error:
        raise ValueError("Unsupported Windows package platform") from error


def audit(prefix):
    machine = package_machine(prefix)
    for path in prefix.rglob("*"):
        if path.suffix.lower() not in (".dll", ".exe"):
            continue
        for name in imports(path, machine):
            if not system(name) and not (path.parent / name).is_file():
                raise RuntimeError(f"PE import is not adjacent to its loader: {path}: {name}")


def bundle(prefix, llvm):
    prefix, llvm = prefix.resolve(), llvm.resolve()
    machine = package_machine(prefix)
    triplet = "arm64-windows-static" if machine == MACHINES["windows-arm64"] else "x64-windows-static"
    private = prefix / "libexec/kalandra"
    private.mkdir(parents=True)
    resources = Path(run(llvm / "bin/clang++.exe", "-print-resource-dir").strip()).resolve()
    if not resources.is_relative_to(llvm):
        raise RuntimeError("Clang resources escape verified SDK")
    resource_copy = prefix / "lib/kalandra/toolchain/clang"
    shutil.copytree(resources, resource_copy)
    shutil.copy2(llvm / "bin/clang++.exe", private / "clang++.exe")
    licenses = prefix / "share/licenses/kalandra-toolchain"
    licenses.mkdir(parents=True)
    shutil.copy2(Path(__file__).resolve().parents[1] / "third-party/LLVM-23.1.3-LICENSE.txt", licenses / "LLVM-LICENSE.txt")
    for package in ("zlib", "zstd", "libxml2"):
        copyright = llvm / "dependencies" / triplet / "share" / package / "copyright"
        if not copyright.is_file():
            raise RuntimeError(f"Missing static LLVM dependency license: {package}")
        shutil.copy2(copyright, licenses / f"{package}-copyright")
    queue = [p for p in prefix.rglob("*") if p.suffix.lower() in (".exe", ".dll")]
    visited, metadata = set(), []
    while queue:
        target = queue.pop()
        if target in visited:
            continue
        visited.add(target)
        for name in imports(target, machine):
            if system(name):
                continue
            copied = target.parent / name
            candidates = [p.resolve() for p in (llvm / "bin").iterdir() if p.name.lower() == name.lower()]
            if len(candidates) != 1:
                raise RuntimeError(f"PE import lacks a unique licensed SDK dependency: {target}: {name}")
            source, = candidates
            if not copied.exists():
                shutil.copy2(source, copied)
                metadata.append({"name": name, "upstream": "LLVM", "sourceSHA256": digest(source),
                                 "path": str(copied.relative_to(prefix))})
                queue.append(copied)
    audit(prefix)
    path = prefix / "share/kalandra-native-tools/toolchain.json"
    info = json.loads(path.read_text())
    info.update({"standalone": True, "llvmRuntime": "bundled", "libraries": metadata,
                 "platformRequirements": ["Windows 11 ARM64" if machine == MACHINES["windows-arm64"] else "Windows 11 x86-64", "Visual Studio 2026 ARM64 C++ Build Tools and Windows SDK" if machine == MACHINES["windows-arm64"] else "Visual Studio C++ Build Tools and Windows SDK", "Git for Windows for package management"],
                 "unsupportedModes": ["ThreadSanitizer"] + (["ASan/UBSan"] if machine == MACHINES["windows-arm64"] else [])})
    path.write_text(json.dumps(info, indent=2) + "\n")
    print(f"PE closure audited: {len(visited)} objects, {len(metadata)} private DLL copies", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--llvm", type=Path, required=True)
    args = parser.parse_args()
    bundle(args.prefix, args.llvm)
