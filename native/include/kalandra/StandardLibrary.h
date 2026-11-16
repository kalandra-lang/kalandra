#pragma once
#include <optional>
#include <string_view>
namespace kalandra {
enum class StandardModule {
  Io,
  Format,
  Process,
  Filesystem,
  Buffer,
  Parse,
  Path,
  Env,
  Time,
  Option,
  Iteration,
  Comparison,
  Arithmetic
};
inline std::optional<StandardModule> standardModule(std::string_view name) {
  if (name == "std/ops")
    return StandardModule::Arithmetic;
  if (name == "std/cmp")
    return StandardModule::Comparison;
  if (name == "std/iter")
    return StandardModule::Iteration;
  if (name == "std/option")
    return StandardModule::Option;
  if (name == "std/buffer")
    return StandardModule::Buffer;
  if (name == "std/time")
    return StandardModule::Time;
  if (name == "std/env")
    return StandardModule::Env;
  if (name == "std/path")
    return StandardModule::Path;
  if (name == "std/parse")
    return StandardModule::Parse;
  if (name == "std/fs")
    return StandardModule::Filesystem;
  if (name == "std/io")
    return StandardModule::Io;
  if (name == "std/fmt")
    return StandardModule::Format;
  if (name == "std/process")
    return StandardModule::Process;
  return {};
}
inline std::string_view standardModulePath(StandardModule module) {
  switch (module) {
  case StandardModule::Arithmetic:
    return "kalandra-stdlib:std/ops.k";
  case StandardModule::Comparison:
    return "kalandra-stdlib:std/cmp.k";
  case StandardModule::Iteration:
    return "kalandra-stdlib:std/iter.k";
  case StandardModule::Option:
    return "kalandra-stdlib:std/option.k";
  case StandardModule::Buffer:
    return "kalandra-stdlib:std/buffer.k";
  case StandardModule::Time:
    return "kalandra-stdlib:std/time.k";
  case StandardModule::Env:
    return "kalandra-stdlib:std/env.k";
  case StandardModule::Path:
    return "kalandra-stdlib:std/path.k";
  case StandardModule::Parse:
    return "kalandra-stdlib:std/parse.k";
  case StandardModule::Filesystem:
    return "kalandra-stdlib:std/fs.k";
  case StandardModule::Io:
    return "kalandra-stdlib:std/io.k";
  case StandardModule::Format:
    return "kalandra-stdlib:std/fmt.k";
  case StandardModule::Process:
    return "kalandra-stdlib:std/process.k";
  }
  return {};
}
inline std::optional<StandardModule> standardModuleFromPath(std::string_view path) {
  if (path == standardModulePath(StandardModule::Arithmetic))
    return StandardModule::Arithmetic;
  if (path == standardModulePath(StandardModule::Comparison))
    return StandardModule::Comparison;
  if (path == standardModulePath(StandardModule::Iteration))
    return StandardModule::Iteration;
  if (path == standardModulePath(StandardModule::Option))
    return StandardModule::Option;
  if (path == standardModulePath(StandardModule::Buffer))
    return StandardModule::Buffer;
  if (path == standardModulePath(StandardModule::Time))
    return StandardModule::Time;
  if (path == standardModulePath(StandardModule::Env))
    return StandardModule::Env;
  if (path == standardModulePath(StandardModule::Path))
    return StandardModule::Path;
  if (path == standardModulePath(StandardModule::Parse))
    return StandardModule::Parse;
  if (path == standardModulePath(StandardModule::Filesystem))
    return StandardModule::Filesystem;
  if (path == standardModulePath(StandardModule::Io))
    return StandardModule::Io;
  if (path == standardModulePath(StandardModule::Format))
    return StandardModule::Format;
  if (path == standardModulePath(StandardModule::Process))
    return StandardModule::Process;
  return {};
}
// Read-only compiler declarations. Intrinsic bodies are provided by lowering;
// the unresolved names deliberately fail ordinary source checking unless the
// module loader has supplied its separate standard-module identity.
inline std::string_view standardModuleSource(StandardModule module) {
  switch (module) {
  case StandardModule::Arithmetic:
    return R"k(export trait Add[Rhs]:
  type Output
  fn add(left: Self, right: Rhs) -> Self.Output
fn __add[L, R](left: L, right: R):
  return left + right
export trait Sub[Rhs]:
  type Output
  fn sub(left: Self, right: Rhs) -> Self.Output
fn __sub[L, R](left: L, right: R):
  return left - right
export trait Mul[Rhs]:
  type Output
  fn mul(left: Self, right: Rhs) -> Self.Output
fn __mul[L, R](left: L, right: R):
  return left * right
export trait Div[Rhs]:
  type Output
  fn div(left: Self, right: Rhs) -> Self.Output
fn __div[L, R](left: L, right: R):
  return left / right
export trait Rem[Rhs]:
  type Output
  fn rem(left: Self, right: Rhs) -> Self.Output
fn __rem[L, R](left: L, right: R):
  return left % right
export trait Neg:
  type Output
  fn neg(value: Self) -> Self.Output
fn __neg[T](value: T):
  return -value
)k";
  case StandardModule::Comparison:
    return R"k(export trait Equal:
  fn equal(left: Self, right: Self) -> bool
export enum Ordering:
  Less
  Equal
  Greater
fn __orderingCode(value: Ordering) -> i64:
  return __kalandra_cmp_ordering_code(value)
impl Equal for Ordering:
  fn equal(left: Self, right: Self) -> bool:
    return __orderingCode(left) == __orderingCode(right)
export trait Order:
  fn compare(left: Self, right: Self) -> Ordering
export fn compare[T: Order](left: T, right: T) -> Ordering:
  return Order.compare(left, right)
impl Order for i8:
  fn compare(left: Self, right: Self) -> Ordering:
    return if left < right: Ordering.Less
    else:
      if left > right: Ordering.Greater
      else: Ordering.Equal
impl Order for i16:
  fn compare(left: Self, right: Self) -> Ordering:
    return if left < right: Ordering.Less
    else:
      if left > right: Ordering.Greater
      else: Ordering.Equal
impl Order for i32:
  fn compare(left: Self, right: Self) -> Ordering:
    return if left < right: Ordering.Less
    else:
      if left > right: Ordering.Greater
      else: Ordering.Equal
impl Order for i64:
  fn compare(left: Self, right: Self) -> Ordering:
    return if left < right: Ordering.Less
    else:
      if left > right: Ordering.Greater
      else: Ordering.Equal
impl Order for u8:
  fn compare(left: Self, right: Self) -> Ordering:
    return if left < right: Ordering.Less
    else:
      if left > right: Ordering.Greater
      else: Ordering.Equal
impl Order for u16:
  fn compare(left: Self, right: Self) -> Ordering:
    return if left < right: Ordering.Less
    else:
      if left > right: Ordering.Greater
      else: Ordering.Equal
impl Order for u32:
  fn compare(left: Self, right: Self) -> Ordering:
    return if left < right: Ordering.Less
    else:
      if left > right: Ordering.Greater
      else: Ordering.Equal
impl Order for u64:
  fn compare(left: Self, right: Self) -> Ordering:
    return if left < right: Ordering.Less
    else:
      if left > right: Ordering.Greater
      else: Ordering.Equal
export fn totalCompare[T](left: T, right: T) -> Ordering:
  a = __kalandra_cmp_bits(left)
  b = __kalandra_cmp_bits(right)
  sign = __kalandra_cmp_sign(left)
  largest = sign + (sign - 1)
  first = if a >= sign: largest - a
  else: a + sign
  second = if b >= sign: largest - b
  else: b + sign
  return if first < second: Ordering.Less
  else:
    if first > second: Ordering.Greater
    else: Ordering.Equal
impl Order for String:
  fn compare(left: Self, right: Self) -> Ordering:
    order = __kalandra_cmp_string_compare(left, right)
    return if order < 0: Ordering.Less
    else:
      if order > 0: Ordering.Greater
      else: Ordering.Equal
impl Order for bool:
  fn compare(left: Self, right: Self) -> Ordering:
    return if left == right: Ordering.Equal
    else:
      if left: Ordering.Greater
      else: Ordering.Less
fn __sortIndices[T](values: [T], comparator: fn(T, T) -> Ordering) -> [i64]:
  count = length(values)
  positions = generate(count, fn(i): return i)
  scratch = generate(count, fn(i): return 0)
  width: i64 = 1
  while width < count:
    start: i64 = 0
    while start < count:
      middle = if width < count - start: start + width
      else: count
      finish = if width < count - middle: middle + width
      else: count
      left = start
      right = middle
      output = start
      while left < middle and right < finish:
        leftPosition = positions[left]
        rightPosition = positions[right]
        decision = comparator(values[leftPosition], values[rightPosition])
        before = decision != Ordering.Greater
        if before:
          scratch[output] = leftPosition
          left = left + 1
        else:
          scratch[output] = rightPosition
          right = right + 1
        output = output + 1
      while left < middle:
        scratch[output] = positions[left]
        left = left + 1
        output = output + 1
      while right < finish:
        scratch[output] = positions[right]
        right = right + 1
        output = output + 1
      start = finish
    previous = positions
    positions = scratch
    scratch = previous
    width = if width < count - width: width + width
    else: count
  return positions
export fn sortedBy[T](values: [T], comparator: fn(T, T) -> Ordering) -> [T]:
  positions = __sortIndices(values, comparator)
  return generate(length(values), fn(i): return values[positions[i]])
fn __sortedValues[T](values: [T], comparator: fn(T, T) -> Ordering) -> [T]:
  count = length(values)
  result = copy(values)
  scratch = copy(values)
  width: i64 = 1
  while width < count:
    start: i64 = 0
    while start < count:
      middle = if width < count - start: start + width
      else: count
      finish = if width < count - middle: middle + width
      else: count
      left = start
      right = middle
      output = start
      while left < middle and right < finish:
        decision = comparator(result[left], result[right])
        if decision != Ordering.Greater:
          scratch[output] = result[left]
          left = left + 1
        else:
          scratch[output] = result[right]
          right = right + 1
        output = output + 1
      while left < middle:
        scratch[output] = result[left]
        left = left + 1
        output = output + 1
      while right < finish:
        scratch[output] = result[right]
        right = right + 1
        output = output + 1
      start = finish
    previous = result
    result = scratch
    scratch = previous
    width = if width < count - width: width + width
    else: count
  return result
fn __exchange[T](mut values: [T], left: i64, right: i64) -> unit:
  __kalandra_cmp_exchange(values, left, right)
  return ()
export fn sortBy[T](mut values: [T], comparator: fn(T, T) -> Ordering) -> [T]:
  positions = __sortIndices(values, comparator)
  destinations = generate(length(values), fn(i): return 0)
  for index in range(length(values)):
    destinations[positions[index]] = index
  for index in range(length(values)):
    while destinations[index] != index:
      target = destinations[index]
      __exchange(values, index, target)
      __exchange(destinations, index, target)
  return values
impl Equal for String:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl Equal for bool:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl Equal for unit:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl Equal for i8:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl Equal for i16:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl Equal for i32:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl Equal for i64:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl Equal for u8:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl Equal for u16:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl Equal for u32:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl Equal for u64:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl Equal for f32:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl Equal for f64:
  fn equal(left: Self, right: Self) -> bool:
    return left == right
impl[T: Equal] Equal for [T]:
  fn equal(left: Self, right: Self) -> bool:
    if length(left) != length(right):
      return false
    for index in range(length(left)):
      if left[index] != right[index]:
        return false
    return true
)k";
  case StandardModule::Iteration:
    return R"k(from "std/option" import Option
from "std/cmp" import Equal
export type Range:
  start: i64
  stop: i64
  step: i64
impl Equal for Range:
  fn equal(left: Self, right: Self) -> bool:
    return left.start == right.start and left.stop == right.stop and left.step == right.step
export fn range(start: i64, stop: i64, step: i64) -> Range:
  return __kalandra_range(start, stop, step)
export type Iterator[T, E]:
  __storage: [u8]
export trait Iterable:
  type Item
  type Error
  fn iterator(values: Self) -> Iterator[Self.Item, Self.Error]
impl Iterable for Range:
  type Item = i64
  type Error = never
  fn iterator(values: Self) -> Iterator[i64, never]:
    return __kalandra_iterator_range(values)
impl[T] Iterable for [T]:
  type Item = T
  type Error = never
  fn iterator(values: Self) -> Iterator[T, never]:
    return __kalandra_iterator_array(values)
export fn iter[S: Iterable](values: S) -> Iterator[S.Item, S.Error]:
  return Iterable.iterator(values)
export fn next[T, E](mut cursor: Iterator[T, E]) -> Option[T] ! E:
  return __kalandra_iterator_next(cursor)
export fn close[T, E](own cursor: Iterator[T, E]) -> unit:
  return __kalandra_iterator_close(cursor)
export fn map[T, U, E, F](own values: Iterator[T, E], transform: fn(T) -> U ! F):
  return for value in values:
    yield try transform(value)
export fn filter[T, E, F](own values: Iterator[T, E], predicate: fn(T) -> bool ! F):
  return for value in values:
    if try predicate(value):
      yield value
export fn take[T, E](own values: Iterator[T, E], count: i64) -> Iterator[T, E]:
  return while count > 0:
    item = try next(values)
    match item:
      Option.None: break
      Option.Some {value}:
        count = count - 1
        yield value
        ()
    ()
)k";
  case StandardModule::Option:
    return R"k(from "std/cmp" import Equal
export enum Option[T]:
  None
  Some:
    value: T
impl[T: Equal] Equal for Option[T]:
  fn equal(left: Self, right: Self) -> bool:
    return match left:
      Option.None:
        match right:
          Option.None: true
          Option.Some {value}: false
      Option.Some {value}:
        match right:
          Option.None: false
          Option.Some {value: other}: value == other
)k";
  case StandardModule::Buffer:
    return R"k(export type ByteBuffer:
  __storage: [u8]
export error BufferError:
  InvalidSize
  OutOfRange
export fn byteBuffer(capacity: i64) -> ByteBuffer ! BufferError:
  return __kalandra_buffer_create(capacity)
export fn length(buffer: ByteBuffer) -> i64:
  return __kalandra_buffer_length(buffer)
export fn capacity(buffer: ByteBuffer) -> i64:
  return __kalandra_buffer_capacity(buffer)
export fn reserve(mut buffer: ByteBuffer, minimum: i64) -> unit ! BufferError:
  return __kalandra_buffer_reserve(buffer, minimum)
export fn append(mut buffer: ByteBuffer, bytes: [u8]) -> unit ! BufferError:
  return __kalandra_buffer_append(buffer, bytes)
export fn truncate(mut buffer: ByteBuffer, count: i64) -> unit ! BufferError:
  return __kalandra_buffer_truncate(buffer, count)
export fn clear(mut buffer: ByteBuffer) -> unit:
  return __kalandra_buffer_clear(buffer)
export fn toArray(buffer: ByteBuffer) -> [u8]:
  return __kalandra_buffer_to_array(buffer)
export fn intoArray(own buffer: ByteBuffer) -> [u8]:
  return __kalandra_buffer_into_array(buffer)
)k";
  case StandardModule::Time:
    return R"k(export type Instant:
  __ticks: u64
export type Duration:
  __nanoseconds: u64
export error TimeError:
  Unavailable
  OutOfRange
export fn now() -> Instant ! TimeError:
  return __kalandra_time_now()
export fn elapsed(start: Instant) -> Duration ! TimeError:
  return __kalandra_time_elapsed(start)
export fn nanoseconds(value: u64) -> Duration:
  return __kalandra_time_nanoseconds(value)
export fn milliseconds(value: u64) -> Duration ! TimeError:
  return __kalandra_time_milliseconds(value)
export fn asNanoseconds(duration: Duration) -> u64:
  return __kalandra_time_as_nanoseconds(duration)
export fn asSeconds(duration: Duration) -> f64:
  return __kalandra_time_as_seconds(duration)
export fn sleep(duration: Duration) -> unit ! TimeError:
  return __kalandra_time_sleep(duration)
)k";
  case StandardModule::Env:
    return R"k(export enum EnvValue:
  Missing
  Value:
    value: String
export error EnvError:
  InvalidName
  InvalidEncoding
export fn get(name: String) -> EnvValue ! EnvError:
  return __kalandra_environment_get(name)
)k";
  case StandardModule::Path:
    return R"k(export fn join(parts: [String]) -> String:
  return __kalandra_path_join(parts)
export fn fileName(path: String) -> String:
  return __kalandra_path_file_name(path)
export fn parentPath(path: String) -> String:
  return __kalandra_path_parent(path)
export fn extension(path: String) -> String:
  return __kalandra_path_extension(path)
export fn isAbsolute(path: String) -> bool:
  return __kalandra_path_absolute(path)
)k";
  case StandardModule::Parse:
    return R"k(export error ParseError:
  Invalid:
    offset: u64
  OutOfRange
export fn parseI8(text: String) -> i8 ! ParseError:
  return __kalandra_parse_i8(text)
export fn parseI16(text: String) -> i16 ! ParseError:
  return __kalandra_parse_i16(text)
export fn parseI32(text: String) -> i32 ! ParseError:
  return __kalandra_parse_i32(text)
export fn parseI64(text: String) -> i64 ! ParseError:
  return __kalandra_parse_i64(text)
export fn parseU8(text: String) -> u8 ! ParseError:
  return __kalandra_parse_u8(text)
export fn parseU16(text: String) -> u16 ! ParseError:
  return __kalandra_parse_u16(text)
export fn parseU32(text: String) -> u32 ! ParseError:
  return __kalandra_parse_u32(text)
export fn parseU64(text: String) -> u64 ! ParseError:
  return __kalandra_parse_u64(text)
export fn parseF32(text: String) -> f32 ! ParseError:
  return __kalandra_parse_f32(text)
export fn parseF64(text: String) -> f64 ! ParseError:
  return __kalandra_parse_f64(text)
export fn parseBool(text: String) -> bool ! ParseError:
  return __kalandra_parse_bool(text)
)k";
  case StandardModule::Filesystem:
    return R"k(from "std/io" import Line
from "std/buffer" import ByteBuffer
from "std/iter" import Iterator
from "std/option" import Option
from "std/cmp" import Equal
export enum FileKind:
  File
  Directory
  Symlink
  Other
export type DirEntry:
  name: String
  kind: FileKind
export type FileInfo:
  kind: FileKind
  size: u64
export type File:
  __storage: [u8]
export type BufferedReader:
  __storage: [u8]
export type BufferedWriter:
  __storage: [u8]
export enum OpenMode:
  Read
  Write
  Create
  CreateNew
  Append
  ReadWrite
fn __kindCode(value: FileKind) -> i64:
  return match value:
    FileKind.File: 1
    FileKind.Directory: 2
    FileKind.Symlink: 3
    FileKind.Other: 4
impl Equal for FileKind:
  fn equal(left: Self, right: Self) -> bool:
    return __kindCode(left) == __kindCode(right)
fn __modeCode(value: OpenMode) -> i64:
  return match value:
    OpenMode.Read: 1
    OpenMode.Write: 2
    OpenMode.Create: 3
    OpenMode.CreateNew: 4
    OpenMode.Append: 5
    OpenMode.ReadWrite: 6
impl Equal for OpenMode:
  fn equal(left: Self, right: Self) -> bool:
    return __modeCode(left) == __modeCode(right)
impl Equal for DirEntry:
  fn equal(left: Self, right: Self) -> bool:
    return left.name == right.name and left.kind == right.kind
impl Equal for FileInfo:
  fn equal(left: Self, right: Self) -> bool:
    return left.kind == right.kind and left.size == right.size
export error FsError:
  System:
    code: i32
    message: String
  InvalidEncoding
  LineTooLong:
    limit: u64
  UnexpectedEnd:
    expected: u64
    actual: u64
  InvalidArgument
fn __openDirectory(path: String) -> File ! FsError:
  return __kalandra_fs_open_directory(path)
fn __readDirectory(directory: File) -> Option[DirEntry] ! FsError:
  return __kalandra_fs_read_directory(directory)
export fn readDir(path: String) -> Iterator[DirEntry, FsError] ! FsError:
  directory = try __openDirectory(path)
  return while true:
    entry = try __readDirectory(directory)
    match entry:
      Option.None: break
      Option.Some {value}:
        yield value
        ()
export fn metadata(path: String) -> FileInfo ! FsError:
  return __kalandra_fs_metadata(path)
export fn createDir(path: String) -> unit ! FsError:
  return __kalandra_fs_create_directory(path)
export fn removeFile(path: String) -> unit ! FsError:
  return __kalandra_fs_remove_file(path)
export fn removeDir(path: String) -> unit ! FsError:
  return __kalandra_fs_remove_directory(path)
export fn open(path: String, mode: OpenMode) -> File ! FsError:
  return __kalandra_fs_open(path, mode)
export fn bufferedReader(own file: File, capacity: u64) -> BufferedReader ! FsError:
  return __kalandra_fs_reader(file, capacity)
export fn bufferedWriter(own file: File, capacity: u64) -> BufferedWriter ! FsError:
  return __kalandra_fs_writer(file, capacity)
export fn read(file: File, maxBytes: u64) -> [u8] ! FsError:
  return __kalandra_fs_read(file, maxBytes)
export fn readExact(file: File, count: u64) -> [u8] ! FsError:
  return __kalandra_fs_read_exact(file, count)
export fn readInto(file: File, mut bytes: [u8]) -> u64 ! FsError:
  return __kalandra_fs_read_into(file, bytes)
export fn readAppend(file: File, mut buffer: ByteBuffer, maxBytes: u64) -> u64 ! FsError:
  return __kalandra_fs_read_append(file, buffer, maxBytes)
export fn readLine(file: File, maxBytes: u64) -> Line ! FsError:
  return __kalandra_fs_read_line(file, maxBytes)
export fn write(file: File, bytes: [u8]) -> u64 ! FsError:
  return __kalandra_fs_write(file, bytes)
export fn writeAll(file: File, bytes: [u8]) -> unit ! FsError:
  return __kalandra_fs_write_all(file, bytes)
export fn writeText(file: File, text: String) -> unit ! FsError:
  return __kalandra_fs_write_text(file, text)
export fn writeLine(file: File, text: String) -> unit ! FsError:
  return __kalandra_fs_write_line(file, text)
export fn flush(file: File) -> unit ! FsError:
  return __kalandra_fs_flush(file)
export fn finish(own writer: BufferedWriter) -> unit ! FsError:
  return __kalandra_fs_finish(writer)
export fn close(own file: File) -> unit ! FsError:
  return __kalandra_fs_close(file)
export fn seekTo(file: File, offset: u64) -> unit ! FsError:
  return __kalandra_fs_seek(file, offset)
)k";
  case StandardModule::Io:
    return R"k(export error IoError:
  System:
    code: i32
    message: String
  InvalidEncoding
  LineTooLong:
    limit: u64

export enum Line:
  End
  Text:
    text: String

export fn print(text: String) -> unit ! IoError:
  return __kalandra_console_print(text)
export fn println(text: String) -> unit ! IoError:
  return __kalandra_console_println(text)
export fn eprint(text: String) -> unit ! IoError:
  return __kalandra_console_eprint(text)
export fn eprintln(text: String) -> unit ! IoError:
  return __kalandra_console_eprintln(text)
export fn readLine(maxBytes: u64) -> Line ! IoError:
  return __kalandra_console_read_line(maxBytes)
)k";
  case StandardModule::Process:
    return R"k(from "std/cmp" import Equal
export type ExitStatus:
  code: u8
impl Equal for ExitStatus:
  fn equal(left: Self, right: Self) -> bool:
    return left.code == right.code
)k";
  case StandardModule::Format:
    return R"k(export fn format(template: String) -> String:
  return __kalandra_format(template)
export trait Display:
  fn display(value: Self) -> String
impl Display for String:
  fn display(value: Self) -> String:
    return value
impl Display for bool:
  fn display(value: Self) -> String:
    return format("{}", value)
impl Display for i8:
  fn display(value: Self) -> String:
    return format("{}", value)
impl Display for i16:
  fn display(value: Self) -> String:
    return format("{}", value)
impl Display for i32:
  fn display(value: Self) -> String:
    return format("{}", value)
impl Display for i64:
  fn display(value: Self) -> String:
    return format("{}", value)
impl Display for u8:
  fn display(value: Self) -> String:
    return format("{}", value)
impl Display for u16:
  fn display(value: Self) -> String:
    return format("{}", value)
impl Display for u32:
  fn display(value: Self) -> String:
    return format("{}", value)
impl Display for u64:
  fn display(value: Self) -> String:
    return format("{}", value)
impl Display for f32:
  fn display(value: Self) -> String:
    return format("{}", value)
impl Display for f64:
  fn display(value: Self) -> String:
    return format("{}", value)
)k";
  }
  return {};
}
} // namespace kalandra
