"""Portable static generics, traits, associated types, callbacks and ownership."""
SOURCE = '''from "std/io" import println, IoError
from "std/fmt" import format
from "std/buffer" import ByteBuffer, BufferError, byteBuffer, append, intoArray

type Pair[A, B]:
  first: A
  second: B

enum Option[T]:
  None
  Some:
    value: T

type Box[T]:
  value: T

trait Plus:
  fn add(left: Self, right: Self) -> Self

impl Plus for i64:
  fn add(left: Self, right: Self) -> Self:
    return left + right

impl Plus for f64:
  fn add(left: Self, right: Self) -> Self:
    return left + right

trait Read:
  type Item
  fn read(value: Self) -> Self.Item

impl[T] Read for Box[T]:
  type Item = T
  fn read(value: Self) -> Self.Item:
    return value.value

fn identity[T](value: T) -> T:
  return value

fn add[T: Plus](left: T, right: T) -> T:
  return Plus.add(left, right)

fn first[A, B](value: Pair[A, B]) -> A:
  return value.first

fn unwrap[T](value: Option[T], fallback: T) -> T:
  return match value:
    Option.None: fallback
    Option.Some {value}: value

fn read[T: Read](value: T) -> T.Item:
  return Read.read(value)

fn makeAdder[T: Plus](offset: T) -> fn(T) -> T:
  return fn(value: T): return Plus.add(value, offset)

fn forward[T](own value: T) -> T:
  return value

error Invalid:
  Negative

fn checked(value: i64) -> i64 ! Invalid:
  if value < 0:
    fail Invalid.Negative
  return value

fn apply[T, U, E](value: T, callback: fn(T) -> U ! E) -> U ! E:
  return try callback(value)

fn main() -> unit ! (IoError | BufferError):
  pair = Pair {first: 42, second: "retained"}
  some = Option.Some {value: 9}
  none: Option[i64] = Option.None
  callback = identity[i64]
  increment = makeAdder[i64](22)
  expected = apply(-1, checked) catch fn(error): return 7
  buffer = try byteBuffer()
  try append(buffer, [40, 2])
  moved = forward(buffer)
  bytes = intoArray(moved)
  text = read(Box {value: "yes"})
  try println(format("{}:{}:{}:{}:{}:{}:{}:{}:{}", first(pair), callback(42), expected, unwrap(some, 0), unwrap(none, 12), increment(20), length(bytes), add(40.0, 2.0), text))
  return ()
'''
EXPECTED = '42:42:7:9:12:42:2:42:yes\n'
