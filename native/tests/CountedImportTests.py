"""Exercise header-derived borrows against a separately compiled C implementation."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--checker", required=True)
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
    subprocess.run(["xcrun", "clang", "-std=c17", "-fbounds-safety", "-Werror", "-c", "bytes.c"], cwd=root, check=True)
    subprocess.run(["ar", "rcs", "libbytes.a", "bytes.o"], cwd=root, check=True)
    manifest = root / "kalandra.toml"
    manifest.write_text('format=1\n[project]\nentry="main.k"\n[dependencies.bytes]\nheader="bytes.h"\nlibrary="libbytes.a"\n')
    src = root / "main.k"
    src.write_text(program)
    exe = root / "app"
    for mode in ["-O0", "-O2", "--sanitize"]:
        subprocess.run([tools.compiler, "--project", manifest, mode, "-o", exe], check=True)
        assert subprocess.check_output([exe], text=True) == "7:8:1:1:1:3\n"

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
    for original, replacement in [
        ("COUNT(srcCount) src", "src"),
        ("COUNT(srcCount) src", "COUNT(srcCount + 1) src"),
        ("src BORROW", "src"),
        ("src BORROW", "src __attribute__((unused))"),
        ("src BORROW", "src BORROW __attribute__((cf_consumed))"),
        ("_Nonnull COUNT(srcCount)", "_Nullable COUNT(srcCount)"),
        ("void prefix(", "uint8_t * _Nonnull prefix("),
        ("int calls(void)", "static inline int calls(void)"),
    ]:
        h.write_text(header.replace(original, replacement, 1))
        rejects("calls" if original.startswith("int calls") else "prefix")
