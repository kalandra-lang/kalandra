"""Exercise public C symbols through independent C and C++ translation units."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def run(*args, cwd, expect=None):
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True, timeout=120)
    if expect is None:
        if result.returncode:
            raise AssertionError((result.args, result.returncode, result.stdout, result.stderr))
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
        source.write_text('''export "C" direct fn geometry_positive(x: f64) -> bool:
  return x > 0.0

export "C" direct fn geometry_scale(x: f64) -> f64:
  return x * 2.0

export "C" direct fn geometry_id(x: i16) -> i16:
  return x

export "C" fn geometry_twice(x: i32) -> i32:
  return x * 2

export "C" checked fn geometry_checked_float(x: f64) -> f64:
  return x

export "C" fn geometry_checked_bool(x: bool) -> bool:
  return x

export "C" fn geometry_finish() -> unit:
  return ()

export fn packageOnly(x: i32) -> i32:
  return x
''', encoding="utf-8")
        run(args.checker, source, cwd=path)
        run(args.compiler, source, "--emit", "c-header", "-o", "geometry.h", cwd=path)
        header = (path / "geometry.h").read_bytes()
        assert b"packageOnly" not in header and b"geometry_scale" in header
        assert b"int32_t geometry_twice(int32_t arg_x, int32_t *out_value);" in header
        assert b"int32_t geometry_finish(void);" in header
        assert b"double geometry_scale(double arg_x);" in header
        (path / "caller.c").write_text('''#include "geometry.h"
#include <limits.h>
#include <math.h>
int main(void) {
  int32_t out = 17;
  if (geometry_twice(INT32_MAX, &out) != KALANDRA_C_FAULT || out != 17) return 1;
  if (geometry_twice(21, &out) != KALANDRA_C_OK || out != 42) return 2;
  if (geometry_twice(INT32_MIN / 2, &out) != KALANDRA_C_OK || out != INT32_MIN) return 3;
  if (geometry_twice(0, 0) != KALANDRA_C_INVALID_ARGUMENT) return 4;
  double number = 0.0;
  if (geometry_checked_float(NAN, &number) != KALANDRA_C_OK || !isnan(number)) return 5;
  bool flag = false;
  if (geometry_checked_bool(true, &flag) != KALANDRA_C_OK || !flag) return 6;
  if (geometry_finish() != KALANDRA_C_OK) return 7;
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

        # Public output names may coincide with internal staging filenames.
        run(args.compiler, source, "--emit", "static", "-o", "c-exports.o", cwd=path)
        assert (path / "c-exports.o").read_bytes().startswith(b"!<arch>")

        for symbol in ("class", "int32_t", "kalandra_reserved", "KALANDRA_C_OK"):
            source.write_text(f'export "C" fn {symbol}() -> i32:\n  return 42\n', encoding="utf-8")
            run(args.checker, source, cwd=path, expect="reserved")
        source.write_text('export "C" direct fn signed_scale(x: i32) -> i32:\n  return x * 2\n', encoding="utf-8")
        old_header = (path / "geometry.h").read_bytes()
        run(args.compiler, source, "--emit", "c-header", "-o", "geometry.h",
            cwd=path, expect="fault-free")
        assert (path / "geometry.h").read_bytes() == old_header
        for level in ("-O0", "-O1", "-O2", "-O3"):
            run(args.compiler, source, level, "--emit", "object", "-o", "fault.o",
                cwd=path, expect="fault-free")

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
