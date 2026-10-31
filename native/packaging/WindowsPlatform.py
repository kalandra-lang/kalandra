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


def rename_after_process_exit(source, destination):
    """Keep the removal check strict while allowing bounded Windows file locks."""
    import time
    for attempt in range(6):
        try:
            return source.rename(destination)
        except PermissionError as error:
            if getattr(error, 'winerror', None) not in (5, 32) or attempt == 5:
                raise
            print(f'Windows rename file-lock retry {attempt + 1}/5: {error.winerror}', flush=True)
            time.sleep(0.1 * 2 ** attempt)


def coff_archive_machines(path):
    """Inspect regular COFF/bigobj members without requiring a host LLVM SDK."""
    data = Path(path).read_bytes()
    if not data.startswith(b'!<arch>\n'):
        raise ValueError('Expected a self-contained COFF archive')
    offset, machines = 8, set()
    while offset < len(data):
        header = data[offset:offset+60]
        if len(header) != 60 or header[58:60] != b'`\n':
            raise ValueError('Invalid COFF archive member header')
        size = int(header[48:58].decode('ascii').strip())
        name = header[:16].decode('ascii').strip()
        contents = data[offset+60:offset+60+size]
        if len(contents) != size: raise ValueError('Truncated COFF archive member')
        if name not in ('/', '//', '/SYM64/'):
            if name.startswith('#1/'):
                contents = contents[int(name[3:]):]
            if len(contents) < 20: raise ValueError('Truncated COFF object')
            machine = int.from_bytes(contents[6:8] if contents[:4] == b'\0\0\xff\xff' else contents[:2], 'little')
            if machine not in MACHINES.values(): raise ValueError('Unsupported COFF object machine')
            machines.add(machine)
        offset += 60 + size + (size & 1)
    if offset != len(data) or not machines: raise ValueError('Invalid or empty COFF archive')
    return machines
