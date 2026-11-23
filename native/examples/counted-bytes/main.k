from "bytes" import copyPrefix
from "std/buffer" import ByteBuffer, BufferError, byteBuffer, append
from "std/io" import println, IoError
from "std/fmt" import format

fn main() -> unit ! (BufferError | IoError):
  dst = try byteBuffer(3)
  src = try byteBuffer(3)
  vals: [u8] = [11, 22, 33]
  zeros: [u8] = [0, 0, 0]
  try append(src, vals)
  try append(dst, zeros)
  try copyPrefix(2, dst, src)
  try println(format("{} {} {}", dst[0], dst[1], dst[2]))
  return ()
