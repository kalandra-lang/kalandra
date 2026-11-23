from "std/iter" import Iterator, next, close as closeCursor
from "std/net" import TcpStream, TcpHalves, TcpListener, TcpReader, TcpWriter, listenTcp, connectTcp, connectTcpWithin, localPort, setTimeout, accept, split, takeReader, takeWriter, shutdownWrite, close, NetError
from "std/time" import milliseconds, nanoseconds, sleep, TimeError
from "std/stream" import readInto, readAppend, writeAll, StreamError
from "std/buffer" import byteBuffer, append, toArray, BufferError
from "std/fs" import open, close as closeFile, OpenMode, FsError
from "std/io" import println, IoError
from "std/fmt" import format
from "std/parse" import parseU64, ParseError

fn sendFile(own writer: TcpWriter) -> unit ! (FsError | BufferError | StreamError):
  file = try open("input.bin", OpenMode.Read)
  buf = try byteBuffer(65536)
  while true:
    if try readInto(file, buf, 65536) == 0: break
    try writeAll(writer, buf)
  try closeFile(file)
  return ()

fn receiveFile(own reader: TcpReader) -> unit ! (FsError | BufferError | StreamError):
  file = try open("result.bin", OpenMode.Create)
  buf = try byteBuffer(16384)
  while true:
    if try readInto(reader, buf, 16384) == 0: break
    try writeAll(file, buf)
  try closeFile(file)
  return ()

fn exchange(host: String, port: u16) -> unit ! (NetError | StreamError | FsError | BufferError | TimeError | TaskError):
  sock = try connectTcpWithin(host, port, try milliseconds(10000))
  halves = try split(sock)
  reader = try takeReader(halves)
  writer = try takeWriter(halves)
  try close(halves)
  return try parallel:
    tx = spawn sendFile(writer)
    rx = spawn receiveFile(reader)
    try await rx
    try await tx

fn timedAccept(listener: TcpListener) -> i64 ! (NetError | StreamError):
  peer = try accept(listener)
  try close(peer)
  return 1

fn serve(host: String) -> unit ! (NetError | IoError | BufferError | StreamError | TimeError):
  listener = try listenTcp(host, 0)
  try println(format("{}", try localPort(listener)))
  peer = try accept(listener)
  buf = try byteBuffer(4096)
  while true:
    count = try readInto(peer, buf, 4096)
    if count == 0: break
    try writeAll(peer, buf)
  try shutdownWrite(peer)
  try close(peer)
  try setTimeout(listener, try milliseconds(30))
  outcome = timedAccept(listener) catch fn(e): return match e:
    NetError.TimedOut: 42
    _: 0
  try println(format("{}", outcome))
  return ()

type ListenerError = TaskError | NetError | StreamError

fn holding(own listener: TcpListener) -> Iterator[i64, ListenerError]:
  return for i in range(1):
    result = try parallel:
      pending = spawn timedAccept(listener)
      yield i
      try await pending

fn echoOne(own peer: TcpStream) -> unit ! (BufferError | StreamError):
  buffer = try byteBuffer(1)
  count = try readInto(peer, buffer, 1)
  try writeAll(peer, buffer)
  return ()

fn cancelListener() -> unit ! (NetError | StreamError | IoError | TaskError | TimeError | BufferError):
  listener = try listenTcp("127.0.0.1", 0)
  try println(format("{}", try localPort(listener)))
  peer = try accept(listener)
  cursor = holding(listener)
  first = try next(cursor)
  return try parallel:
    active = spawn echoOne(peer)
    try await active
    try sleep(try milliseconds(20))
    closeCursor(cursor)
    try println("settled")

fn connectOutcome(host: String, port: u16, mode: String) -> i64 ! (NetError | StreamError):
  if mode == "deadline":
    peer = try connectTcpWithin(host, port, nanoseconds(0))
    try close(peer)
    return 0
  peer = try connectTcpWithin(host, port, nanoseconds(1000000000))
  try close(peer)
  return 0

fn probes(port: u16) -> unit ! IoError:
  expired = connectOutcome("localhost", port, "deadline") catch fn(e): return match e:
    NetError.TimedOut: 1
    _: 0
  refused = connectOutcome("127.0.0.1", port, "") catch fn(e): return match e:
    NetError.System {code, message}: 1
    NetError.TimedOut: 1
    _: 0
  invalid = connectOutcome("", port, "") catch fn(e): return match e:
    NetError.InvalidArgument: 1
    _: 0
  resolution = connectOutcome("bad::v6::literal", port, "") catch fn(e): return match e:
    NetError.Resolution {code}: 1
    _: 0
  try println(format("{}{}{}{}", expired, refused, invalid, resolution))
  return ()

fn timedRead() -> unit ! (NetError | StreamError | BufferError | TimeError | IoError):
  listener = try listenTcp("127.0.0.1", 0)
  client = try connectTcp("127.0.0.1", try localPort(listener))
  server = try accept(listener)
  chunk = try byteBuffer(4)
  try append(chunk, [99])
  try setTimeout(client, try milliseconds(15))
  timed = readAppend(client, chunk, 3) catch fn(e): return match e:
    StreamError.TimedOut: u64(42)
    _: u64(0)
  try writeAll(server, chunk)
  n = try readAppend(client, chunk, 1)
  bytes = toArray(chunk)
  halves = try split(client)
  writer = try takeWriter(halves)
  duplicate = takeAgain(halves) catch fn(e): return match e:
    NetError.HalfUnavailable: 1
    _: 0
  try close(writer)
  try println(format("{}:{}:{}:{}:{}", timed, n, bytes[0], bytes[1], duplicate))
  return ()

fn takeAgain(halves: TcpHalves) -> i64 ! (NetError | StreamError):
  writer = try takeWriter(halves)
  try close(writer)
  return 0

fn sendUntilBlocked(port: u16) -> i64 ! (NetError | StreamError | BufferError | TimeError):
  peer = try connectTcp("127.0.0.1", port)
  try setTimeout(peer, try milliseconds(30))
  chunk = try byteBuffer(65536)
  zeros: [u8] = generate(65536, fn(i: i64): return u8(0))
  try append(chunk, zeros)
  for i in range(2048):
    try writeAll(peer, chunk)
  return 0

fn main(args: [String]) -> unit ! (NetError | StreamError | FsError | BufferError | TimeError | TaskError | IoError | ParseError):
  if args[0] == "fill":
    port = u16(try parseU64(args[1]))
    result = sendUntilBlocked(port) catch fn(e): return match e:
      StreamError.TimedOut: 42
      StreamError.BrokenPipe {code}: 43
      StreamError.System {code, message}: 44
      _: 0
    try println(format("{}", result))
    return ()
  if args[0] == "probe":
    return try probes(u16(try parseU64(args[1])))
  if args[0] == "read":
    return try timedRead()
  if args[0] == "cancel":
    return try cancelListener()
  if args[0] == "serve":
    return try serve(args[1])
  return try exchange(args[1], u16(try parseU64(args[2])))
