"""Portable owned buffer and file-I/O application conformance."""
SOURCE='''from "std/buffer" import byteBuffer, append, toArray, intoArray, clear, BufferError
from "std/fs" import open, OpenMode, readInto, readAppend, writeAll, close, FsError
from "std/io" import println, IoError
from "std/fmt" import format
fn main()->unit ! (BufferError | FsError | IoError):
  data=try byteBuffer(16)
  try append(data,[40,2,0,9])
  saved=toArray(data)
  data[0]=41
  output=try open("buffer.bin",OpenMode.Create)
  try writeAll(output,data)
  try close(output)
  input=try open("buffer.bin",OpenMode.Read)
  clear(data)
  first=try readInto(input,data,2)
  rest=try readAppend(input,data,8)
  try close(input)
  values=intoArray(data)
  try println(format("{}:{}:{}:{}:{}:{}",saved[0],first,rest,length(values),values[0],values[2]))
  return ()
'''
EXPECTED='40:2:2:4:41:0\n'
