"""Exercise header-derived borrows against a separately compiled C implementation."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--checker", required=True)
parser.add_argument("--portable", action="store_true")
parser.add_argument("--cc")
parser.add_argument("--resource-dir")
parser.add_argument("--ar")
parser.add_argument("--object-only", action="store_true")
parser.add_argument("--skip-sanitizer", action="store_true")
tools = parser.parse_args()

header = """#include <stdint.h>
#define BORROW __attribute__((noescape))
#define COUNT(x) __attribute__((counted_by(x)))
void prefix(uint32_t dstCount, const uint8_t * _Nonnull COUNT(srcCount) src BORROW,
            uint8_t * _Nonnull COUNT(dstCount) dst BORROW, uint64_t srcCount);
void scratch(uint32_t n, const uint8_t * _Nonnull COUNT(n) src BORROW);
int calls(void);
"""
implementation = """#include "bytes.h"
static int dispatched;
void scratch(uint32_t n, const uint8_t * _Nonnull COUNT(n) src BORROW) {
  if (n) ((uint8_t *)src)[0] = 255;
}
int calls(void) { return dispatched; }
void prefix(uint32_t d, const uint8_t * _Nonnull COUNT(s) source BORROW,
            uint8_t * _Nonnull COUNT(d) output BORROW, uint64_t s) {
  ++dispatched;
  for (uint32_t i = 0; i < d && i < s; ++i) output[i] = source[i];
}
"""
if tools.portable:
    header = """#include <stdint.h>
#define BORROW __attribute__((noescape))
#define EXTENT(pointer, count) __attribute__((diagnose_if( \
    (count) > __builtin_object_size((pointer), 0), "buffer too short", "error")))
void prefix(uint32_t dstCount, const uint8_t * _Nonnull src BORROW,
            uint8_t * _Nonnull dst BORROW, uint64_t srcCount)
    EXTENT(src, srcCount) EXTENT(dst, dstCount);
void scratch(uint32_t n, const uint8_t * _Nonnull src BORROW) EXTENT(src, n);
int calls(void);
"""
    # The independent implementation includes the annotated declaration. Its
    # parameter names can differ and its ordinary machine ABI stays the same.
    implementation = implementation.replace("COUNT(n) ", "").replace("COUNT(s) ", "").replace("COUNT(d) ", "")

program = '''from "bytes" import prefix, scratch, calls
from "std/buffer" import ByteBuffer, BufferError, byteBuffer, append
from "std/fmt" import format
from "std/io" import IoError, println

fn copy(d: u32, src: ByteBuffer, mut dst: ByteBuffer, s: u64) -> i64 ! BufferError:
  try prefix(d, src, dst, s)
  return 0

fn checked(d: u32, src: ByteBuffer, mut dst: ByteBuffer, s: u64) -> i64:
  code = copy(d, src, dst, s) catch fn(error):
    return match error:
      BufferError.OutOfRange: 1
      BufferError.InvalidSize: 2
  return code

fn main() -> unit ! (IoError | BufferError):
  src = try byteBuffer(9)
  dst = try byteBuffer(9)
  empty = try byteBuffer()
  try append(src, [7, 8, 9])
  try append(dst, [0, 0])
  try scratch(3, src)
  try prefix(2, src, dst, 3)
  try prefix(0, empty, dst, 0)
  try prefix(0, src, empty, 3)
  badSrc = checked(2, src, dst, 4)
  badDst = checked(3, src, dst, 1)
  huge = checked(0, src, dst, 18446744073709551615)
  try println(format("{}:{}:{}:{}:{}:{}", dst[0], dst[1], badSrc, badDst, huge, calls()))
  return ()
'''

with tempfile.TemporaryDirectory(prefix="Kalandra counted native ") as temp:
    root = Path(temp)
    h = root / "bytes.h"
    h.write_text(header)
    (root / "bytes.c").write_text(implementation)
    cc = [tools.cc] if tools.portable else ["xcrun", "clang", "-fbounds-safety"]
    if tools.resource_dir:
        cc.extend(["-resource-dir", tools.resource_dir])
    archive = "bytes.lib" if os.name == "nt" else "libbytes.a"
    subprocess.run([*cc, "-x", "c", "-std=c17", "-Werror", "-c", "bytes.c", "-o", "bytes.o"], cwd=root, check=True)
    ar = tools.ar or "ar"
    arguments = ["/OUT:" + archive] if Path(ar).stem.lower() in {"lib", "llvm-lib"} else ["rcs", archive]
    if tools.object_only:
        archive = "bytes.o"
    else:
        subprocess.run([ar, *arguments, "bytes.o"], cwd=root, check=True)
    manifest = root / "kalandra.toml"
    manifest.write_text('format=1\n[project]\nentry="main.k"\n[dependencies.bytes]\nheader="bytes.h"\n'
                        f'library="{archive}"\n')
    src = root / "main.k"
    src.write_text(program)
    exe = root / ("app.exe" if os.name == "nt" else "app")
    for mode in (["-O0", "-O2"] if tools.skip_sanitizer else ["-O0", "-O2", "--sanitize"]):
        subprocess.run([tools.compiler, "--project", manifest, mode, "-o", exe], check=True)
        assert subprocess.check_output([exe], text=True) == "7:8:1:1:1:3\n"

    if tools.portable:
        # Ownership/bounds and thread isolation can describe the same function.
        h.write_text(header.replace("EXTENT(src, srcCount) EXTENT(dst, dstCount);",
                                    'EXTENT(src, srcCount) EXTENT(dst, dstCount) __attribute__((swift_attr("@MainActor")));'))
        subprocess.run([tools.compiler, "--project", manifest, "-O2", "-o", exe], check=True)
        assert subprocess.check_output([exe], text=True) == "7:8:1:1:1:3\n"
        h.write_text(header)

    def rejects(message):
        result = subprocess.run([tools.checker, "--project", manifest], capture_output=True, text=True)
        assert result.returncode and message in result.stderr, result

    src.write_text(program.replace("try prefix(2, src, dst, 3)", "try prefix(2, dst, dst, 2)"))
    rejects("S081")
    reader = "fn reading(b: ByteBuffer) -> u8:\n  return b[0]\n\n"
    racing = program.replace("fn main()", reader + "fn main()").replace(
        "  try prefix(2, src, dst, 3)",
        "  try parallel:\n    pending = spawn reading(dst)\n    try prefix(2, src, dst, 3)\n    try await pending")
    src.write_text(racing.replace("IoError | BufferError", "IoError | BufferError | TaskError"))
    rejects("S077")
    src.write_text(program)
    negative = [
        ("src BORROW", "src"),
        ("src BORROW", "src __attribute__((unused))"),
        ("src BORROW", "src BORROW __attribute__((cf_consumed))"),
        ("const uint8_t * _Nonnull", "const uint8_t * _Nullable"),
        ("void prefix(", "uint8_t * _Nonnull prefix("),
        ("int calls(void)", "static inline int calls(void)"),
    ]
    if tools.portable:
        negative += [
            ("EXTENT(src, srcCount)", ""),
            ("EXTENT(src, srcCount)", "EXTENT(src, srcCount + 1)"),
            ("EXTENT(dst, dstCount)", "EXTENT(src, dstCount)"),
            ("(count) >", "(count) <"),
            ("(pointer), 0", "(pointer), 1"),
            ('"error"', '"warning"'),
            ("(count) >", "(uint8_t)(count) >"),
        ]
    else:
        negative += [("COUNT(srcCount) src", "src"), ("COUNT(srcCount) src", "COUNT(srcCount + 1) src")]
    for original, replacement in negative:
        h.write_text(header.replace(original, replacement, 1))
        rejects("calls" if original.startswith("int calls") else "prefix")
