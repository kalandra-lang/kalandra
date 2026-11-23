"""Build an independent C library and consume its header from a fresh Kalandra project."""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    for tool in ("compiler", "checker", "cc", "ar"):
        parser.add_argument("--" + tool, required=True)
    tools = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="Kalandra header import ") as temp:
        root = Path(temp)
        project = root / "app"
        project.mkdir()
        header = project / "geometry.h"
        header.write_text("#include <stdint.h>\ntypedef int32_t Score;\nScore twice(Score);\n"
                          "double half(double value);\nvoid unsupported(void *data);\n")
        (project / "geometry.c").write_text('#include "geometry.h"\n'
                                            "Score twice(Score x) { return x * 2; }\n"
                                            "double half(double x) { return x / 2; }\n")
        archive = project / ("geometry.lib" if os.name == "nt" else "libgeometry.a")
        subprocess.run([tools.cc, "-c", "geometry.c", "-o", "geometry.o"], cwd=project, check=True)
        if Path(tools.ar).stem.lower() in {"lib", "llvm-lib"}:
            command = [tools.ar, "/OUT:" + str(archive), "geometry.o"]
        else:
            command = [tools.ar, "rcs", str(archive), "geometry.o"]
        subprocess.run(command, cwd=project, check=True)
        manifest = project / "kalandra.toml"
        manifest.write_text('format=1\n[project]\nentry="main.k"\n[dependencies.geometry]\n'
                            f'header="geometry.h"\nlibrary="{archive.name}"\n')
        source = project / "main.k"
        source.write_text('from "geometry" import twice, half\nfrom "std/process" import ExitStatus\n'
                          'fn main() -> ExitStatus:\n'
                          '  if unsafe twice(21) == 42 and unsafe half(7.0) == 3.5:\n'
                          '    return ExitStatus {code: 0}\n  return ExitStatus {code: 1}\n')
        executable = root / ("consumer.exe" if os.name == "nt" else "consumer")
        subprocess.run([tools.checker, "--project", manifest], cwd=root, check=True)
        subprocess.run([tools.compiler, "--project", manifest, "-o", executable], cwd=root, check=True)
        subprocess.run([executable], cwd=root, check=True)
        source.write_text(source.read_text().replace("unsafe twice", "twice"))
        denied = subprocess.run([tools.checker, "--project", manifest], cwd=root, capture_output=True)
        assert denied.returncode and b"unsafe" in denied.stderr, denied
        header.write_text("typedef struct Missing Bad; Bad unsupported(void);\n")
        denied = subprocess.run([tools.checker, "--project", manifest], cwd=root, capture_output=True)
        assert denied.returncode and b"no supported external C scalar prototype" in denied.stderr, denied


if __name__ == "__main__":
    main()
