"""Round-trip exact UTF-8 bytes, native layout, handle identity and package imports."""
import argparse
from pathlib import Path
import subprocess
import tempfile

def run(args, cwd, *, error=None):
    p = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=120)
    if (p.returncode == 0) != (error is None) or (error and error not in p.stderr):
        raise AssertionError((args, p.returncode, p.stdout, p.stderr))
    return p.stdout.strip()

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', type=Path, required=True)
a = parser.parse_args()
compiler = a.compiler.resolve()
with tempfile.TemporaryDirectory(prefix='kalandra native values ') as directory:
    root = Path(directory).resolve()
    package, app = root/'measure', root/'app'
    package.mkdir()
    app.mkdir()
    header = '''#pragma once
struct Sample { int scale; double reading; unsigned tag; };
struct device;
struct device * _Nonnull attach(int id) __attribute__((acquire_handle("DeviceHandle")));
void detach(struct device * _Nonnull d __attribute__((release_handle("DeviceHandle"))))
    __attribute__((swift_attr("nonisolated")));
int inspect(struct device * _Nonnull a __attribute__((noescape,use_handle("DeviceHandle"))),
            const struct Sample value[static 1] __attribute__((noescape)),
            unsigned size, const char * _Nonnull bytes __attribute__((noescape)),
            struct device * _Nonnull b __attribute__((noescape,use_handle("DeviceHandle"))))
    __attribute__((diagnose_if(size > __builtin_object_size(bytes,0),"invalid byte range","error")));
int liveCount(void);
'''
    (package/'measure.h').write_text(header)
    (package/'measure.c').write_text('''#include "measure.h"
#include <stdlib.h>
#include <string.h>
struct device { int id; };
static int live;
struct device *attach(int id) {
    struct device *d = malloc(sizeof(*d));
    if (!d) abort();
    d->id = id; ++live; return d;
}
void detach(struct device *d) { --live; free(d); }
int inspect(struct device *a, const struct Sample v[static 1], unsigned size,
            const char *text, struct device *b) {
    const unsigned char expected[] = {65,0,0xe2,0x98,0x83};
    return a->id == 2 && b->id == 3 && v->scale == -2 && v->reading == 3.25 && v->tag == 5 &&
        (size == 0 || (size == sizeof(expected) && memcmp(text,expected,size)==0));
}
int liveCount(void) { return live; }
''')
    manifest = '''format=1
[package]
name="measure"
version="1.0.0"
entry="measure.k"
[dependencies.driver]
header="measure.h"
[dependencies.driver.targets]
linux={source="measure.c"}
windows={source="measure.c"}
macos={source="measure.c"}
'''
    (package/'kalandra.toml').write_text(manifest)
    (package/'measure.k').write_text('''from "driver" import attach, detach, inspect, Sample, liveCount

fn measure(text: String) -> bool:
  left = attach(2)
  right = attach(3)
  value = Sample {scale: -2, reading: 3.25, tag: 5}
  ok = inspect(left, value, text, right) == 1
  detach(left)
  return ok and liveCount() == 1

export fn verified(text: String) -> bool:
  return measure(text) and liveCount() == 0
''')
    run(['git','init','-q'], package)
    run(['git','add','.'], package)
    run(['git','-c','user.name=Fixture','-c','user.email=fixture@localhost','commit','-qm','fixture'], package)
    revision = run(['git','rev-parse','HEAD'], package)
    (app/'kalandra.toml').write_text(f'''format=1
[project]
entry="main.k"
[dependencies.measure]
git="{package.as_posix()}"
rev="{revision}"
''')
    (app/'main.k').write_text('''from "measure" import verified
from "std/process" import ExitStatus
fn main() -> ExitStatus:
  if verified("A\\u{0}☃") and verified(""):
    return ExitStatus {code: 0}
  return ExitStatus {code: 1}
''')
    run([compiler,'install','--project','kalandra.toml'], app)
    exe = app/'roundtrip.exe'
    for mode in ('-O0','-O2'):
        run([compiler,'--project','kalandra.toml',mode,'-o',exe], app)
        run([exe], app)
    # Check incompatible types and incomplete borrows on local source as well.
    (package/'kalandra.toml').write_text(manifest.replace('[package]\nname="measure"\nversion="1.0.0"', '[project]'))
    for malformed, expected in [
        (header.replace('int inspect(struct device *', 'struct other;\nint inspect(struct other *'), 'incompatible'),
        (header.replace('noescape,use_handle','use_handle'), 'handle borrow'),
        (header.replace('const struct Sample value[static 1]', 'const struct Sample *value'), 'byte pointers'),
    ]:
        (package/'measure.h').write_text(malformed)
        run([compiler,'--project','kalandra.toml','-o',exe], package, error=expected)
