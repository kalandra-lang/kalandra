"""Write a deterministic GNU tar/gzip archive without modifying staged files."""
import argparse
import gzip
import os
from pathlib import Path
import tarfile
import tempfile


def create_archive(prefix, destination, epoch=0):
    if not 0 <= epoch <= 0xffffffff:
        raise ValueError("SOURCE_DATE_EPOCH must be an unsigned 32-bit timestamp")
    prefix = prefix.resolve()
    if not prefix.is_dir():
        raise ValueError("Archive prefix must be an installed directory")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as output:
            temporary = Path(output.name)
            with gzip.GzipFile(filename="", fileobj=output, mode="wb", mtime=epoch) as compressed:
                with tarfile.open(fileobj=compressed, mode="w|", format=tarfile.GNU_FORMAT) as archive:
                    for path in [prefix, *sorted(prefix.rglob("*"))]:
                        if path.is_symlink() or not (path.is_dir() or path.is_file()):
                            raise ValueError(f"Only regular files/directories can be archived: {path}")
                        info = archive.gettarinfo(path, arcname=str(path.relative_to(prefix.parent)))
                        info.uid = info.gid = 0
                        info.uname = info.gname = ""
                        info.mtime = epoch
                        info.mode = 0o755 if path.is_dir() or info.mode & 0o111 else 0o644
                        if path.is_file():
                            with path.open("rb") as content:
                                archive.addfile(info, content)
                        else:
                            archive.addfile(info)
        temporary.replace(destination)
    finally:
        if temporary and temporary.exists():
            temporary.unlink()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    create_archive(args.prefix, args.destination, int(os.environ.get("SOURCE_DATE_EPOCH", "0")))
