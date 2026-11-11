#include "../runtime/ByteBuffer.h"
#include "../runtime/FileSystem.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
using namespace kalandra;
using namespace kalandra::runtime;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "buffer assertion %d: %s\n", __LINE__, #x);                             \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
static BufferResult call(Invocation &ctx, BufferAction action, BufferArguments args = {}) {
  BufferResult result;
  kalandra_buffer_v1(&ctx, &result, unsigned(action), &args);
  CHECK(!ctx.poll());
  return result;
}
static BufferArguments with(BufferDescriptor buffer, std::int64_t value = 0) {
  BufferArguments args;
  args.buffer = buffer;
  args.value = value;
  return args;
}
static BufferBytes bytes(Invocation &ctx, const std::string &text) {
  BufferBytes result;
  result.allocated = result.aligned = static_cast<unsigned char *>(ctx.allocate(text.size()));
  result.sizes[0] = std::int64_t(text.size());
  result.strides[0] = 1;
  if (!text.empty())
    std::memcpy(result.aligned, text.data(), text.size());
  return result;
}
static std::string text(const BufferBytes &bytes) {
  return bytes.sizes[0] ? std::string(reinterpret_cast<const char *>(bytes.aligned + bytes.offset),
                                      bytes.sizes[0])
                        : "";
}
static void operations() {
  Invocation ctx;
  auto *scope = kalandra_file_scope_enter_v1(&ctx, nullptr, 0);
  CHECK(scope);
  auto buffer = call(ctx, BufferAction::Create).buffer;
  CHECK(call(ctx, BufferAction::Length, with(buffer)).count == 0);
  CHECK(call(ctx, BufferAction::Capacity, with(buffer)).count == 0);
  auto source = bytes(ctx, std::string("h\0i", 3));
  auto args = with(buffer);
  args.bytes = source;
  for (unsigned i = 0; i < 2000; ++i)
    CHECK(call(ctx, BufferAction::Append, args).kind == 0);
  const auto stats = bufferStats(ctx, buffer);
  CHECK(stats.length == 6000 && stats.capacity >= stats.length && stats.growths < 12 &&
        stats.copiedBytes < 12000);
  CHECK(call(ctx, BufferAction::Reserve, with(buffer, -1)).kind ==
        unsigned(BufferKind::InvalidSize));
  CHECK(call(ctx, BufferAction::Truncate, with(buffer, 6001)).kind ==
        unsigned(BufferKind::OutOfRange));
  CHECK(bufferStats(ctx, buffer).length == 6000);
  auto copy = call(ctx, BufferAction::ToArray, with(buffer)).bytes;
  CHECK(copy.allocated != stats.data && copy.sizes[0] == 6000 && copy.aligned[1] == 0);
  copy.aligned[0] = 'x';
  CHECK(call(ctx, BufferAction::ToArray, with(buffer)).bytes.aligned[0] == 'h');
  call(ctx, BufferAction::Clear, with(buffer));
  CHECK(bufferStats(ctx, buffer).capacity == stats.capacity);
  call(ctx, BufferAction::Append, args);
  CHECK(bufferStats(ctx, buffer).growths == stats.growths);
  auto result = call(ctx, BufferAction::IntoArray, with(buffer)).bytes;
  CHECK(result.allocated == stats.data && text(result) == std::string("h\0i", 3));
  kalandra_array_validate_v1(&ctx, &result, unsigned(NumericType::UInt8));
  CHECK(!ctx.poll());
  kalandra_file_scope_leave_v1(&ctx, scope, nullptr);
  CHECK(text(result) == std::string("h\0i", 3));
}
struct Budget {
  std::uint64_t bytes = 0, maximum = 0;
};
static bool reserve(void *p, std::uint64_t bytes) noexcept {
  auto &s = *static_cast<Budget *>(p);
  if (bytes > s.maximum - s.bytes)
    return false;
  s.bytes += bytes;
  return true;
}
static void release(void *p, std::uint64_t bytes) noexcept {
  static_cast<Budget *>(p)->bytes -= bytes;
}
static void accounting() {
  Invocation source;
  auto buffer = call(source, BufferAction::Create, with({}, 4096)).buffer;
  Budget rejected{0, 2048};
  {
    Invocation destination;
    destination.bindRecursiveBudget(&rejected, reserve, release);
    CHECK(!destination.retainFrom(source, buffer.storage.allocated));
    CHECK(destination.status() == 2);
  }
  CHECK(rejected.bytes == 0);
  Budget accepted{0, 16384};
  {
    Invocation destination;
    destination.bindRecursiveBudget(&accepted, reserve, release);
    CHECK(destination.retainFrom(source, buffer.storage.allocated));
    CHECK(accepted.bytes >= 4096);
    CHECK(bufferStats(destination, buffer).capacity == 4096);
  }
  CHECK(accepted.bytes == 0);
}
static void handoff() {
  Invocation receiver;
  BufferDescriptor value;
  {
    Invocation source;
    value = call(source, BufferAction::Create, with({}, 32)).buffer;
    auto args = with(value);
    args.bytes = bytes(source, "abc");
    call(source, BufferAction::Append, args);
    CHECK(receiver.retainFrom(source, value.storage.allocated));
  }
  CHECK(text(call(receiver, BufferAction::ToArray, with(value)).bytes) == "abc");
  auto *scope = kalandra_file_scope_enter_v1(&receiver, nullptr, 0);
  CHECK(scope);
  CHECK(kalandra_file_receive_v1(&receiver, &value));
  kalandra_file_scope_leave_v1(&receiver, scope, nullptr);
  BufferResult result;
  auto args = with(value);
  kalandra_buffer_v1(&receiver, &result, unsigned(BufferAction::Length), &args);
  CHECK(receiver.status() == 3);
}
static void emptyAndFailure() {
  for (unsigned point = 1; point < 9; ++point) {
    Invocation ctx;
    ctx.failAllocationAt(point);
    BufferResult result;
    auto args = with({}, 100);
    kalandra_buffer_v1(&ctx, &result, unsigned(BufferAction::Create), &args);
    if (ctx.status())
      CHECK(ctx.status() == 2);
    else
      CHECK(result.buffer.storage.allocated);
  }
  Invocation ctx;
  auto value = call(ctx, BufferAction::Create).buffer;
  auto array = call(ctx, BufferAction::IntoArray, with(value)).bytes;
  CHECK(array.sizes[0] == 0);
  kalandra_array_validate_v1(&ctx, &array, unsigned(NumericType::UInt8));
  CHECK(!ctx.poll());
}
static void snapshots() {
  Invocation ctx;
  auto buffer = call(ctx, BufferAction::Create, with({}, 16)).buffer;
  auto args = with(buffer);
  args.bytes = bytes(ctx, "abc");
  call(ctx, BufferAction::Append, args);
  BufferIO pending;
  CHECK(bufferWriteView(ctx, buffer, pending));
  const auto *original = pending.data;
  call(ctx, BufferAction::Clear, with(buffer));
  args.bytes = bytes(ctx, "xyz");
  call(ctx, BufferAction::Append, args);
  CHECK(bufferStats(ctx, buffer).data != original && std::memcmp(pending.data, "abc", 3) == 0);
  BufferIO next;
  CHECK(bufferWriteView(ctx, buffer, next));
  auto result = call(ctx, BufferAction::IntoArray, with(buffer)).bytes;
  CHECK(result.allocated != next.data && text(result) == "xyz");
  result.aligned[0] = 'Q';
  CHECK(std::memcmp(next.data, "xyz", 3) == 0);
  auto input = call(ctx, BufferAction::Create).buffer;
  BufferIO read;
  CHECK(bufferReadView(ctx, input, 8, false, read));
  std::memcpy(read.data, "hello", 5);
  CHECK(bufferStats(ctx, input).length == 0);
  CHECK(bufferReadCommit(ctx, read, 5));
  CHECK(text(call(ctx, BufferAction::ToArray, with(input)).bytes) == "hello");
}
static FileResult fileCall(Invocation &ctx, FileAction action, FileArguments args = {}) {
  FileResult result;
  kalandra_file_v1(&ctx, &result, unsigned(action), &args);
  CHECK(!ctx.poll() && result.kind == FileKind::Success);
  return result;
}
static void fileRoundTrip() {
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path =
      std::filesystem::temp_directory_path() / ("kalandra-byte-buffer-" + std::to_string(unique));
  for (bool buffered : {false, true}) {
    Invocation ctx;
    FileArguments file;
    const auto name = path.string();
    kalandra_string_literal_v1(&ctx, &file.path, name.data(), name.size());
    file.mode = unsigned(FileMode::Create);
    auto writer = fileCall(ctx, FileAction::Open, file).file;
    FileArguments output;
    output.file = writer;
    if (buffered) {
      output.value = 2;
      writer = fileCall(ctx, FileAction::BufferedWriter, output).file;
      output.file = writer;
    }
    auto buffer = call(ctx, BufferAction::Create).buffer;
    auto args = with(buffer);
    args.bytes = bytes(ctx, "abcdef");
    call(ctx, BufferAction::Append, args);
    output.buffer = buffer;
    CHECK(fileCall(ctx, FileAction::WriteAllBuffer, output).count == 6);
    fileCall(ctx, buffered ? FileAction::Finish : FileAction::Close, output);
    file.mode = unsigned(FileMode::Read);
    FileArguments input;
    input.file = fileCall(ctx, FileAction::Open, file).file;
    if (buffered) {
      input.value = 8;
      input.file = fileCall(ctx, FileAction::BufferedReader, input).file;
    }
    input.buffer = buffer;
    input.value = 3;
    CHECK(fileCall(ctx, FileAction::ReadIntoBuffer, input).count == 3);
    CHECK(text(call(ctx, BufferAction::ToArray, with(buffer)).bytes) == "abc");
    input.value = 2;
    CHECK(fileCall(ctx, FileAction::ReadAppend, input).count == 2);
    CHECK(text(call(ctx, BufferAction::ToArray, with(buffer)).bytes) == "abcde");
    input.value = 8;
    CHECK(fileCall(ctx, FileAction::ReadIntoBuffer, input).count == 1);
    CHECK(text(call(ctx, BufferAction::ToArray, with(buffer)).bytes) == "f");
    CHECK(fileCall(ctx, FileAction::ReadAppend, input).count == 0);
    CHECK(bufferStats(ctx, buffer).length == 1);
    input.value = 0;
    CHECK(fileCall(ctx, FileAction::ReadIntoBuffer, input).count == 0);
    CHECK(bufferStats(ctx, buffer).length == 0);
    fileCall(ctx, FileAction::Close, input);
  }
  std::filesystem::remove(path);
}
int main() {
  operations();
  accounting();
  handoff();
  emptyAndFailure();
  snapshots();
  fileRoundTrip();
  std::puts("byte buffer runtime passed");
}
