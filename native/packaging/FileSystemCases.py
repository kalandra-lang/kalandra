"""One public-entry filesystem contract shared by native and cross-target packages."""
SOURCE = '''from "std/fs" import File, open, OpenMode, FsError, bufferedWriter, bufferedReader, writeLine, finish, readLine, readInto, seekTo, close, createDir, readDir, metadata, removeFile, removeDir
from "std/io" import println, IoError, Line
from "std/fmt" import format
fn passAlong(own file:File)->File:
  return file
fn main()->unit ! (FsError | IoError):
  output=try open("cross Ω.bin",OpenMode.Create)
  writer=try bufferedWriter(output,2)
  try writeLine(writer,"λ")
  try finish(writer)
  input=try open("cross Ω.bin",OpenMode.Read)
  owned=passAlong(input)
  reader=try bufferedReader(owned,2)
  line=try readLine(reader,20)
  try seekTo(reader,0)
  buffer:[u8]=[9,9,9,9,9]
  count=try readInto(reader,buffer)
  try match line:
    Line.End:println("end")
    Line.Text {text}:println(format("{}:{}:{}:{}",text,count,buffer[0],buffer[4]))
  try close(reader)
  try createDir("cross-directory Ω")
  temporary=try open("cross-directory Ω/item",OpenMode.CreateNew)
  try writeLine(temporary,"hello")
  try close(temporary)
  info=try metadata("cross-directory Ω/item")
  total=0
  try for entry in try readDir("cross-directory Ω"):
    total=total+length(entry.name)
  try println(format("{}:{}",info.size,total))
  try removeFile("cross-directory Ω/item")
  try removeDir("cross-directory Ω")
  return ()
'''
# readInto may short-read from the reader's two-byte buffer. Its suffix stays 9.
EXPECTED = 'λ:2:206:9\n6:4\n'
