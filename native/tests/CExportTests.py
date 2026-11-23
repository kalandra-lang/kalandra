"""Exercise public C symbols through independent C and C++ translation units."""
import argparse
import os
import platform
import re
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


def check_application_callback(args, path):
    header = path / "events.h"
    contract = """#include <stdint.h>
typedef int32_t (*Event)(int32_t);
int32_t dispatch(Event _Nonnull handler __attribute__((noescape)), int32_t argument);
"""
    header.write_text(contract)
    (path / "events.c").write_text('''#include "events.h"
int32_t dispatch(Event _Nonnull handler __attribute__((noescape)), int32_t argument) {
  return handler(argument);
}
''')
    run(args.cc, "-c", "events.c", "-o", "events.o", cwd=path)
    (path / "kalandra.toml").write_text(
        'format=1\n[project]\nentry="app.k"\n[dependencies.events]\n'
        'header="events.h"\nlibrary="events.o"\n')
    source = path / "app.k"
    program = '''from "events" import dispatch
from "std/process" import ExitStatus
export "C" direct fn identity(value: i32) -> i32:
  return value
export "C" direct fn answer(value: i32) -> i32:
  return 42
fn main() -> ExitStatus:
  if dispatch(identity, 17) == 17 and dispatch(answer, 17) == 42:
    return ExitStatus {code: 0}
  return ExitStatus {code: 1}
'''
    source.write_text(program)
    exe = path / ("app.exe" if sys.platform == "win32" else "app")
    modes = ["-O0", "-O2"]
    if sys.platform != "win32" or platform.machine().lower() not in {"arm64", "aarch64"}:
        modes.append("--sanitize")
    for mode in modes:
        run(args.compiler, "--project", "kalandra.toml", mode, "--emit-llvm", "app.ll", "-o", exe, cwd=path)
        run(exe, cwd=path)
        ir = (path / "app.ll").read_text()
        for symbol in ("identity", "answer"):
            assert re.search(rf"^define internal .*@kalandra_c_callback_{symbol}\(", ir, re.M)
            assert not re.search(rf"^define .*@{symbol}\(", ir, re.M)
    # Enter from a coordinating application as well as an ordinary CPU main.
    source.write_text(program.replace('from "std/process" import ExitStatus',
                                      'from "std/io" import println, IoError')
                      .replace('fn main() -> ExitStatus:', 'fn main() -> unit ! IoError:')
                      .replace('return ExitStatus {code: 0}', 'try println("delivered")\n    return ()')
                      .replace('return ExitStatus {code: 1}', 'return ()'))
    run(args.compiler, "--project", "kalandra.toml", "-O2", "-o", exe, cwd=path)
    assert subprocess.check_output([exe], cwd=path, text=True).strip() == "delivered"
    for changed in (program.replace('export "C" direct fn identity', 'fn identity'),
                    program.replace('direct fn identity', 'checked fn identity')):
        source.write_text(changed)
        run(args.checker, "--project", "kalandra.toml", cwd=path, expect="direct C callback")
    source.write_text(program.replace("  return value", "  return value * 2"))
    run(args.compiler, "--project", "kalandra.toml", "-o", exe, cwd=path, expect="fault-free")
    source.write_text(program)
    header.write_text(contract.replace("_Nonnull", "_Nullable"))
    run(args.checker, "--project", "kalandra.toml", cwd=path, expect="dispatch")



def check_retained_callback(args, path):
    (path / "listener.h").write_text('''typedef int (*Listener)(int);
void subscribe(Listener _Nonnull listener);
int deliver(int n);
void unsubscribe(void);
''')
    # Registration stores the function address after the Kalandra frame returns.
    # Each dispatch joins its foreign thread before reusing the stack argument.
    (path / "listener.c").write_text('''#include "listener.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
static Listener current;
struct Message { Listener callback; int value; };
#ifdef _WIN32
static DWORD WINAPI invoke(void *data) {
#else
static void *invoke(void *data) {
#endif
  struct Message *m = data;
  m->value = m->callback(m->value);
  return 0;
}
void subscribe(Listener _Nonnull cb) { current = cb; }
void unsubscribe(void) { current = 0; }
int deliver(int n) {
  if (!current) return -1;
  struct Message m = {current, n};
#ifdef _WIN32
  HANDLE t = CreateThread(0, 0, invoke, &m, 0, 0);
  if (!t) return -2;
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
#else
  pthread_t t;
  if (pthread_create(&t, 0, invoke, &m) || pthread_join(t, 0)) return -2;
#endif
  return m.value;
}
''')
    run(args.cc, "-c", "listener.c", "-o", "listener.o", cwd=path)
    (path / "kalandra.toml").write_text(
        'format=1\n[project]\nentry="app.k"\n[dependencies.events]\n'
        'header="listener.h"\nlibrary="listener.o"\n')
    program = '''from "events" import subscribe, deliver, unsubscribe
from "std/process" import ExitStatus
export "C" direct fn identity(n: i32) -> i32:
  return n
fn register() -> unit:
  subscribe(identity)
  return ()
fn main() -> ExitStatus:
  register()
  first = deliver(37)
  unsubscribe()
  second = deliver(17)
  if first == 37 and second == -1:
    return ExitStatus {code: 0}
  return ExitStatus {code: 1}
'''
    source = path / "app.k"
    source.write_text(program)
    exe = path / ("listener.exe" if sys.platform == "win32" else "listener")
    modes = ["-O0", "-O2"]
    if sys.platform != "win32" or platform.machine().lower() not in {"arm64", "aarch64"}:
        modes.append("--sanitize")
    for mode in modes:
        run(args.compiler, "--project", "kalandra.toml", mode, "-o", exe, cwd=path)
        run(exe, cwd=path)
    source.write_text(program.replace("export \"C\" direct fn identity", "fn identity"))
    run(args.checker, "--project", "kalandra.toml", cwd=path, expect="direct C callback")


def check_captured_subscription(args, path):
    fixture = Path(__file__).resolve().parent / "native-subscription"
    source = path / "app.k"
    header = path / "sensor.h"
    original = (fixture / "app.k").read_text()
    contract = (fixture / "sensor.h").read_text()
    (path / "kalandra.toml").write_text(
        'format=1\n[project]\nentry="app.k"\n[dependencies.sensor]\n'
        'header="sensor.h"\nlibrary="sensor.o"\n')
    for blocks in ([False, True] if sys.platform == "darwin" else [False]):
        header.write_text(("#define SENSOR_BLOCKS 1\n" if blocks else "") + contract)
        (path / "sensor.cpp").write_text((fixture / "sensor.cpp").read_text())
        source.write_text(original)
        exe = path / ("subscription.exe" if os.name == "nt" else "subscription")
        modes = ["-O0", "-O2"]
        if sys.platform != "win32" or platform.machine().lower() not in {"arm64", "aarch64"}:
            modes.append("--sanitize")
        for mode in modes:
            instrument = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if (
                mode == "--sanitize" and os.name != "nt") else []
            runtime = ["-fms-runtime-lib=static"] if os.name == "nt" else []
            run(args.cxx, "-std=c++17", *runtime, *(["-fblocks"] if blocks else []), *instrument,
                "-c", "sensor.cpp", "-o", "sensor.o", cwd=path)
            run(args.compiler, "--project", "kalandra.toml", mode, "-o", exe, cwd=path)
            run(exe, cwd=path)
        source.write_text(original.replace("  unsubscribe(a)", "  unsubscribe(a)\n  unsubscribe(a)"))
        run(args.checker, "--project", "kalandra.toml", cwd=path, expect="moved")
        source.write_text(original.replace("return cap(value, limit)", "return value * limit"))
        run(args.compiler, "--project", "kalandra.toml", "-o", exe, cwd=path, expect="fault-free")
        source.write_text(original)
        annotated = ("#define SENSOR_BLOCKS 1\n" if blocks else "") + contract
        header.write_text(annotated.replace('"Subscription"', '"sensor_listener"'))
        source.write_text(original.replace("Subscription", "SensorListener"))
        run(args.checker, "--project", "kalandra.toml", cwd=path)
        source.write_text(original)
        header.write_text(annotated.replace('swift_attr("nonisolated")', 'swift_attr("@MainActor")'))
        run(args.checker, "--project", "kalandra.toml", cwd=path, expect="subscription release")
        header.write_text(annotated.replace('release_handle("Subscription")', 'release_handle("Other")'))
        run(args.checker, "--project", "kalandra.toml", cwd=path, expect="subscription release")


def check_native_thread_requirement(args, path):
    # An instrument binding is independent of window systems and SDKs.
    header = path / "instrument.h"
    header.write_text('''int threadIdentity(void);
int readInstrument(void) __attribute__((swift_attr("@MainActor")));
''')
    (path / "instrument.c").write_text('''#include "instrument.h"
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
static DWORD initial;
__attribute__((constructor)) static void remember(void) { initial = GetCurrentThreadId(); }
int threadIdentity(void) { return initial == GetCurrentThreadId(); }
#else
#include <pthread.h>
static pthread_t initial;
__attribute__((constructor)) static void remember(void) { initial = pthread_self(); }
int threadIdentity(void) { return pthread_equal(initial, pthread_self()) != 0; }
#endif
int readInstrument(void) {
  if (!threadIdentity()) fputs("entered on wrong thread", stderr);
  return threadIdentity();
}
''')
    run(args.cc, "-c", "instrument.c", "-o", "instrument.o", cwd=path)
    (path / "kalandra.toml").write_text(
        'format=1\n[project]\nentry="app.k"\n[dependencies.instrument]\n'
        'header="instrument.h"\nlibrary="instrument.o"\n')
    source = path / "app.k"
    program = '''from "instrument" import threadIdentity, readInstrument
from "std/io" import println, IoError
from "std/fmt" import format
from "std/time" import sleep, milliseconds, TimeError
fn probe() -> i32:
  return threadIdentity()
fn main() -> unit ! (IoError | TimeError | TaskError):
  before = readInstrument()
  child: i32 = try parallel:
    work = spawn probe()
    try await work
  try sleep(try milliseconds(u64(3)))
  try println(format("{} {} {}", before, child, readInstrument()))
  return ()
'''
    source.write_text(program)
    exe = path / ("app.exe" if sys.platform == "win32" else "app")
    for workers, expected in ((1, "1 1 1"), (2, "1 0 1"), (4, "1 0 1")):
        if workers == 1:
            run(args.compiler, "--project", "kalandra.toml", "-O2", "-o", exe, cwd=path)
        env = dict(os.environ, KALANDRA_WORKERS=str(workers))
        assert subprocess.check_output([exe], cwd=path, text=True, env=env, timeout=20).strip() == expected
    # Exercise an indirect call from a worker. The native function must never enter.
    source.write_text(program.replace('return threadIdentity()', 'return readInstrument()'))
    run(args.compiler, "--project", "kalandra.toml", "-O2", "-o", exe, cwd=path)
    wrong = subprocess.run([exe], cwd=path, capture_output=True, text=True,
                           env=dict(os.environ, KALANDRA_WORKERS="2"), timeout=20)
    assert wrong.returncode and "entered on wrong thread" not in wrong.stderr, wrong
    source.write_text(program.replace(", readInstrument\n", "\n").replace("readInstrument()", "threadIdentity()"))
    run(args.compiler, "--project", "kalandra.toml", "-O2", "--emit-llvm", "plain.ll", "-o", exe, cwd=path)
    assert "kalandra_run_native_application_v1" not in (path / "plain.ll").read_text()


def main():
    parser = argparse.ArgumentParser()
    for name in ("compiler", "checker", "cc", "cxx"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="kalandra C export ") as root:
        path = Path(root)
        check_application_callback(args, path)
        check_retained_callback(args, path)
        check_captured_subscription(args, path)
        check_native_thread_requirement(args, path)
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
