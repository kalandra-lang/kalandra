"""Windows target identity and capabilities of the pinned native LLVM SDKs."""
from pathlib import Path
import struct

MACHINES = {"windows-x86_64": 0x8664, "windows-arm64": 0xaa64}


def pe_machine(path):
    with Path(path).open("rb") as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b"MZ":
            raise ValueError(f"Not a PE object: {path}")
        offset = struct.unpack_from("<I", header, 0x3c)[0]
        stream.seek(offset)
        coff = stream.read(26)
    if len(coff) != 26 or coff[:4] != b"PE\0\0":
        raise ValueError(f"Invalid PE signature: {path}")
    machine = struct.unpack_from("<H", coff, 4)[0]
    if machine not in MACHINES.values() or struct.unpack_from("<H", coff, 24)[0] != 0x20b:
        raise ValueError(f"Unsupported native Windows PE architecture: {path}")
    return machine


def has_sanitizers(path):
    # The official Windows ARM64 LLVM 23.1.3 archive contains compiler builtins
    # and profiling support, but no ASan/UBSan/TSan runtime libraries.
    return pe_machine(path) == MACHINES["windows-x86_64"]
