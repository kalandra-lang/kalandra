from "std/stream" import ByteReader, ByteWriter, StreamError, readInto, writeAll
from "std/fs" import open, close, OpenMode, FsError, bufferedReader, bufferedWriter, finish
from "std/buffer" import ByteBuffer, byteBuffer, BufferError

fn pump[R: ByteReader, W: ByteWriter](source: R, target: W, mut chunk: ByteBuffer) -> u64 ! StreamError:
  total: u64 = 0
  while true:
    count = try readInto(source, chunk, 65536)
    if count == 0:
      break
    try writeAll(target, chunk)
    total = total + count
  return total

fn main() -> unit ! (FsError | StreamError | BufferError):
  chunk = try byteBuffer(65536)
  source = try open("source.bin", OpenMode.Read)
  target = try open("target.bin", OpenMode.Create)
  total = try pump(source, target, chunk)
  try close(target)
  try close(source)
  reader = try bufferedReader(try open("target.bin", OpenMode.Read), 8192)
  writer = try bufferedWriter(try open("buffered.bin", OpenMode.Create), 4096)
  again = try pump(reader, writer, chunk)
  try finish(writer)
  try close(reader)
  return ()
