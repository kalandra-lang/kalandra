"""Exercise public C symbols through independent C and C++ translation units."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def run(*args, cwd, expect=None):
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True, timeout=120)
    if expect is None:
        result.check_returncode()
    elif result.returncode == 0 or expect not in result.stderr:
        raise AssertionError((result.args, result.returncode, result.stderr))


def main():
    parser = argparse.ArgumentParser()
    for name in ("compiler", "checker", "cc", "cxx"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="kalandra C export ") as root:
        path = Path(root)
        source = path / "geometry.k"
        source.write_text('''export "C" fn geometry_positive(x: f64) -> bool:
  return x > 0.0

export "C" fn geometry_scale(x: f64) -> f64:
  return x * 2.0

export "C" fn geometry_id(x: i16) -> i16:
  return x

export fn packageOnly(x: i32) -> i32:
  return x
''', encoding="utf-8")
        run(args.checker, source, cwd=path)
        run(args.compiler, source, "--emit", "c-header", "-o", "geometry.h", cwd=path)
        header = (path / "geometry.h").read_bytes()
        assert b"packageOnly" not in header and b"geometry_scale" in header
        (path / "caller.c").write_text('''#include "geometry.h"
int main(void) {
  return !(geometry_scale(21.0) == 42.0 && geometry_id(-32768) == -32768 &&
           geometry_positive(0.125) && !geometry_positive(-0.5));
}
''', encoding="utf-8")
        windows = sys.platform == "win32"
        shared = "geometry.dll" if windows else "libgeometry.dylib" if sys.platform == "darwin" else "libgeometry.so"
        for kind, name in (("object", "geometry.o"), ("static", "archive.lib" if windows else "libgeometry.a"),
                           ("shared", shared)):
            run(args.compiler, source, "--emit", kind, "--header", "generated.h", "-o", name, cwd=path)
            assert (path / "generated.h").read_bytes() == header
            link = "geometry.lib" if windows and kind == "shared" else name
            flags = [] if windows or kind != "shared" else [
                "-Wl,-rpath,@loader_path" if sys.platform == "darwin" else "-Wl,-rpath,$ORIGIN"]
            for compiler, language, standard in ((args.cc, "c", "c17"), (args.cxx, "c++", "c++20")):
                executable = path / ("caller.exe" if windows else "caller")
                run(compiler, "-x", language, "-std=" + standard, "-Wall", "-Werror", "caller.c",
                    "-x", "none", link, *flags, "-o", executable, cwd=path)
                run(executable, cwd=path)

        for signature in ("(x: String) -> String", "(x: [u8]) -> [u8]",
                          "(x: unit) -> unit", "[T](x: T) -> T"):
            source.write_text('export "C" fn bad' + signature + ':\n  return x\n', encoding="utf-8")
            run(args.checker, source, cwd=path, expect="C export")
        source.write_text('export "C" fn value() -> i32:\n  return 42\n', encoding="utf-8")
        previous = source.read_bytes()
        run(args.compiler, source, "--emit", "shared", "--header", source, "-o", shared,
            cwd=path, expect="distinct")
        assert source.read_bytes() == previous


if __name__ == "__main__":
    main()
