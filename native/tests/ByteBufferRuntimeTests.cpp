#include "../runtime/ByteBuffer.h"
#include "../runtime/FileSystem.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
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
  std::mutex mutex;
  Budget(std::uint64_t bytes, std::uint64_t maximum) : bytes(bytes), maximum(maximum) {}
};
static bool reserve(void *p, std::uint64_t bytes) noexcept {
  auto &s = *static_cast<Budget *>(p);
  std::lock_guard lock(s.mutex);
  if (bytes > s.maximum - s.bytes)
    return false;
  s.bytes += bytes;
  return true;
}
static void release(void *p, std::uint64_t bytes) noexcept {
  auto &s = *static_cast<Budget *>(p);
  std::lock_guard lock(s.mutex);
  CHECK(bytes <= s.bytes);
  s.bytes -= bytes;
}
static void accounting() {
  Budget late{0, 16384};
  {
    Invocation unbound;
    (void)call(unbound, BufferAction::Create, with({}, 4096));
    unbound.bindRecursiveBudget(&late, reserve, release);
    CHECK(unbound.status() == 3 && late.bytes == 0);
  }
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
// Growth must remain charged after temporary allocation frames and borrowed
// helper invocations retire. Retaining only the original capacity is unsound.
static void dynamicAccounting() {
  Budget budget{0, 1 << 24};
  {
    Invocation owner;
    owner.bindRecursiveBudget(&budget, reserve, release);
    auto buffer = call(owner, BufferAction::Create).buffer;
    const auto initial = owner.recursiveBytes();
    {
      Invocation::OwnershipScope temporary(owner);
      Invocation::OwnershipSelection select(temporary);
      call(owner, BufferAction::Reserve, with(buffer, 4096));
    }
    CHECK(owner.recursiveBytes() >= initial + 4096);
    CHECK(owner.recursiveBytes() == budget.bytes);
    const auto beforeBorrow = owner.recursiveBytes();
    {
      Invocation borrower;
      borrower.bindRecursiveBudget(&budget, reserve, release);
      CHECK(borrower.retainFrom(owner, buffer.storage.allocated));
      call(borrower, BufferAction::Reserve, with(buffer, 16384));
      CHECK(owner.recursiveBytes() >= beforeBorrow + 16384 - 4096);
    }
    CHECK(owner.recursiveBytes() == budget.bytes);
    const auto persistent = budget.bytes;
    {
      Invocation::OwnershipScope first(owner), second(owner);
      {
        Invocation::OwnershipSelection select(first);
        for (unsigned i = 0; i < 40; ++i)
          call(owner, BufferAction::Create, with({}, 64));
      }
      CHECK(first.swap(second));
      first.reset();
      CHECK(budget.bytes > persistent);
      second.reset();
      CHECK(budget.bytes == persistent);
    }
    // A helper's independent task budget may admit growth that the original
    // owner cannot afford. Reserve all observers transactionally; keep bytes
    // and capacity unchanged if any one refuses.
    Budget helperBudget{0, 1 << 24};
    {
      Invocation helper;
      helper.bindRecursiveBudget(&helperBudget, reserve, release);
      CHECK(helper.retainFrom(owner, buffer.storage.allocated));
      const auto before = budget.bytes;
      budget.maximum = before + 1024;
      auto args = with(buffer, 65536);
      BufferResult result;
      kalandra_buffer_v1(&helper, &result, unsigned(BufferAction::Reserve), &args);
      CHECK(helper.status() == 2);
      CHECK(budget.bytes == before);
      CHECK(bufferStats(owner, buffer).capacity == 16384);
      budget.maximum = 1 << 24;
    }
    CHECK(helperBudget.bytes == 0);
    // Cross-task growth updates a still-live owner's counters while that owner
    // allocates unrelated data. TSan checks this synchronization path.
    Invocation child;
    child.bindRecursiveBudget(&budget, reserve, release);
    CHECK(child.retainFrom(owner, buffer.storage.allocated));
    std::atomic<bool> start{false};
    std::thread worker([&] {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (unsigned i = 0; i < 20; ++i)
        call(child, BufferAction::Reserve, with(buffer, 20000 + i * 1024));
    });
    start.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 1000; ++i) {
      Invocation::OwnershipScope work(owner);
      Invocation::OwnershipSelection select(work);
      auto *p = owner.allocate(32);
      CHECK(p);
      owner.release(p);
      CHECK(owner.recursiveBytes() >= initial);
    }
    worker.join();
    CHECK(bufferStats(owner, buffer).capacity >= 39456);
  }
  CHECK(budget.bytes == 0);
}
struct CancellingBudget {
  Budget budget{0, 1 << 20};
  std::atomic<bool> cancel{false};
  unsigned largeReservations = 0;
  static bool reserveBytes(void *p, std::uint64_t bytes) noexcept {
    auto &s = *static_cast<CancellingBudget *>(p);
    if (!reserve(&s.budget, bytes))
      return false;
    if (bytes >= 4096 && ++s.largeReservations == 2)
      s.cancel.store(true, std::memory_order_release);
    return true;
  }
  static void releaseBytes(void *p, std::uint64_t bytes) noexcept {
    release(&static_cast<CancellingBudget *>(p)->budget, bytes);
  }
};
static void cancelledGrowthAccounting() {
  Invocation owner;
  auto buffer = call(owner, BufferAction::Create).buffer;
  CancellingBudget budget;
  {
    Invocation helper;
    helper.bindCancellation(&budget.cancel);
    helper.bindRecursiveBudget(&budget, CancellingBudget::reserveBytes,
                               CancellingBudget::releaseBytes);
    CHECK(helper.retainFrom(owner, buffer.storage.allocated));
    auto args = with(buffer, 4096);
    BufferResult result;
    kalandra_buffer_v1(&helper, &result, unsigned(BufferAction::Reserve), &args);
    CHECK(budget.largeReservations == 2 && helper.poll() == -1 && helper.status() == 0);
    CHECK(bufferStats(owner, buffer).capacity == 0);
  }
  CHECK(budget.budget.bytes == 0);
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
static void staleReadCommit() {
  Invocation owner, operation;
  auto buffer = call(owner, BufferAction::Create, with({}, 16)).buffer;
  auto args = with(buffer);
  args.bytes = bytes(owner, "old");
  call(owner, BufferAction::Append, args);
  CHECK(operation.retainFrom(owner, buffer.storage.allocated));
  BufferIO read;
  CHECK(bufferReadView(operation, buffer, 8, true, read));
  call(owner, BufferAction::Clear, with(buffer));
  args.bytes = bytes(owner, "new");
  call(owner, BufferAction::Append, args);
  CHECK(!bufferReadCommit(operation, read, 1) && operation.status() == 3);
  CHECK(text(call(owner, BufferAction::ToArray, with(buffer)).bytes) == "new");
}
static FileResult fileCall(Invocation &ctx, FileAction action, FileArguments args = {}) {
  FileResult result;
  kalandra_file_v1(&ctx, &result, unsigned(action), &args);
  CHECK(!ctx.poll() && result.kind == FileKind::Success);
  return result;
}
#ifndef _WIN32
static void blockedReadSnapshot() {
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path =
      std::filesystem::temp_directory_path() / ("kalandra-buffer-read-" + std::to_string(unique));
  CHECK(::mkfifo(path.c_str(), 0600) == 0);
  const auto peer = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
  CHECK(peer >= 0);
  Invocation owner;
  FileArguments file;
  const auto name = path.string();
  kalandra_string_literal_v1(&owner, &file.path, name.data(), name.size());
  file.mode = unsigned(FileMode::Read);
  auto handle = fileCall(owner, FileAction::Open, file).file;
  for (bool append : {false, true}) {
    auto buffer = call(owner, BufferAction::Create, with({}, 16)).buffer;
    auto args = with(buffer);
    args.bytes = bytes(owner, "old");
    call(owner, BufferAction::Append, args);
    std::atomic<bool> cancelled{false};
    Invocation operation;
    operation.bindCancellation(&cancelled);
    CHECK(operation.retainFrom(owner, handle.storage.allocated));
    CHECK(operation.retainFrom(owner, buffer.storage.allocated));
    FileArguments request;
    request.file = handle;
    request.buffer = buffer;
    request.value = 8;
    const auto action = append ? FileAction::ReadAppend : FileAction::ReadIntoBuffer;
    FileResult result;
    void *state = nullptr;
    CHECK(!kalandra_file_step_v1(&operation, &result, unsigned(action), &request, &state));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cancelled.store(true, std::memory_order_release);
    CHECK(kalandra_file_step_v1(&operation, &result, unsigned(action), &request, &state));
    operation.settle();
    CHECK(!state && !operation.live());
    CHECK(text(call(owner, BufferAction::ToArray, with(buffer)).bytes) == (append ? "old" : ""));
    args.bytes = bytes(owner, "new");
    call(owner, BufferAction::Append, args);
    CHECK(::write(peer, "late", 4) == 4);
    // Seek takes the file mutex even though a FIFO cannot seek. Its completion
    // proves the abandoned native read has stopped touching its snapshot.
    FileArguments seek;
    seek.file = handle;
    FileResult ignored;
    kalandra_file_v1(&owner, &ignored, unsigned(FileAction::SeekTo), &seek);
    CHECK(!owner.poll());
    CHECK(text(call(owner, BufferAction::ToArray, with(buffer)).bytes) ==
          (append ? "oldnew" : "new"));
  }
  FileArguments close;
  close.file = handle;
  fileCall(owner, FileAction::Close, close);
  CHECK(::close(peer) == 0);
  CHECK(std::filesystem::remove(path));
}
static void blockedWriteSnapshot() {
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path =
      std::filesystem::temp_directory_path() / ("kalandra-buffer-write-" + std::to_string(unique));
  CHECK(::mkfifo(path.c_str(), 0600) == 0);
  const auto peer = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
  CHECK(peer >= 0);
  Invocation owner;
  FileArguments file;
  const auto name = path.string();
  kalandra_string_literal_v1(&owner, &file.path, name.data(), name.size());
  file.mode = unsigned(FileMode::Write);
  auto handle = fileCall(owner, FileAction::Open, file).file;
  auto buffer = call(owner, BufferAction::Create).buffer;
  auto args = with(buffer);
  args.bytes = bytes(owner, std::string(1 << 20, 'A'));
  call(owner, BufferAction::Append, args);
  const auto *original = bufferStats(owner, buffer).data;
  std::atomic<bool> cancelled{false};
  Invocation operation;
  operation.bindCancellation(&cancelled);
  CHECK(operation.retainFrom(owner, handle.storage.allocated));
  CHECK(operation.retainFrom(owner, buffer.storage.allocated));
  FileArguments request;
  request.file = handle;
  request.buffer = buffer;
  FileResult result;
  void *state = nullptr;
  CHECK(!kalandra_file_step_v1(&operation, &result, unsigned(FileAction::WriteAllBuffer), &request,
                               &state));
  // Observe the first byte: the OS write is now in flight, not merely queued.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  unsigned char first = 0;
  while (::read(peer, &first, 1) != 1) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(first == 'A');
  cancelled.store(true, std::memory_order_release);
  const auto start = std::chrono::steady_clock::now();
  CHECK(kalandra_file_step_v1(&operation, &result, unsigned(FileAction::WriteAllBuffer), &request,
                              &state));
  CHECK(!state);
  operation.settle();
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
  call(owner, BufferAction::Clear, with(buffer));
  args.bytes = bytes(owner, "replacement");
  call(owner, BufferAction::Append, args);
  CHECK(bufferStats(owner, buffer).data != original);
  auto mutableBytes = call(owner, BufferAction::IntoArray, with(buffer)).bytes;
  mutableBytes.aligned[0] = 'X';
  // Retire source state before draining the outstanding OS request. Its pin
  // must preserve both bytes and the file until the native worker completes.
  owner.settle();
  CHECK(!owner.live());
  std::size_t received = 1;
  unsigned char chunk[4096];
  for (;;) {
    const auto count = ::read(peer, chunk, sizeof(chunk));
    if (count > 0) {
      for (std::ptrdiff_t i = 0; i < count; ++i)
        CHECK(chunk[i] == 'A');
      received += std::size_t(count);
    } else if (!count) {
      break;
    } else {
      CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  CHECK(received >= 65536 && received < (1 << 20));
  CHECK(::close(peer) == 0);
  CHECK(std::filesystem::remove(path));
}
#endif
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
  dynamicAccounting();
  cancelledGrowthAccounting();
  handoff();
  emptyAndFailure();
  snapshots();
  staleReadCommit();
  fileRoundTrip();
#ifndef _WIN32
  blockedReadSnapshot();
  blockedWriteSnapshot();
#endif
  std::puts("byte buffer runtime passed");
}
