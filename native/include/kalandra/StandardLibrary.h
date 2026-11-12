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
  Iteration
};
inline std::optional<StandardModule> standardModule(std::string_view name) {
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
  case StandardModule::Iteration:
    return R"k(export type Range:
  start: i64
  stop: i64
  step: i64
export fn range(start: i64, stop: i64, step: i64) -> Range:
  return __kalandra_range(start, stop, step)
)k";
  case StandardModule::Option:
    return R"k(export enum Option[T]:
  None
  Some:
    value: T
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
    return R"k(export type ExitStatus:
  code: u8
)k";
  case StandardModule::Format:
    return R"k(export fn format(template: String) -> String:
  return __kalandra_format(template)
)k";
  }
  return {};
}
} // namespace kalandra
