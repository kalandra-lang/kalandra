#include "FileSystem.h"
#include "../include/kalandra/Utf8.h"
#include "Console.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace {
using namespace kalandra::runtime;
using namespace std::chrono_literals;
#ifdef _WIN32
using NativeFile = HANDLE;
const NativeFile invalidFile = INVALID_HANDLE_VALUE;
std::int32_t lastError() noexcept { return static_cast<std::int32_t>(GetLastError()); }
constexpr auto ioError = ERROR_WRITE_FAULT;
#else
using NativeFile = int;
constexpr NativeFile invalidFile = -1;
std::int32_t lastError() noexcept { return errno; }
constexpr auto ioError = EIO;
#endif
bool healthy(Invocation &ctx) noexcept { return !ctx.poll() && ctx.usable(); }
std::int32_t closeNative(NativeFile file) noexcept {
  if (file == invalidFile)
    return 0;
#ifdef _WIN32
  return CloseHandle(file) ? 0 : lastError();
#else
  // Never retry close after EINTR: the descriptor may already have been reused.
  return ::close(file) == 0 ? 0 : lastError();
#endif
}
constexpr std::size_t fileSlots = 65536, noFileSlot = fileSlots;
std::size_t reserveFileSlot();
void releaseFileSlot(std::size_t) noexcept;
void deferClose(NativeFile &, std::size_t &) noexcept;
struct FileState;
struct ResourceScope {
  FileState *first = nullptr;
  ResourceScope *parent = nullptr;
  AllocationLease parentOwner;
  bool active = true;
  static void cleanup(void *p) noexcept { static_cast<ResourceScope *>(p)->~ResourceScope(); }
};
// Protect intrusive ownership metadata. Reads, writes and opens never hold it.
std::mutex &resourceMutex() {
  static auto *value = new std::mutex;
  return *value;
}
struct FileState {
  std::mutex mutex;
  NativeFile file = invalidFile;
  std::size_t closeSlot = noFileSlot;
  ResourceScope *scope = nullptr;
  AllocationLease scopeOwner;
  FileState *previous = nullptr, *next = nullptr;
  std::atomic<bool> retired{false};
  enum class Buffer { None, Read, Write } buffer = Buffer::None;
  std::uint64_t capacity = 0, begin = 0, end = 0;
  explicit FileState(std::uint64_t capacity) noexcept : capacity(capacity) {}
  unsigned char *data() noexcept { return reinterpret_cast<unsigned char *>(this + 1); }
  void detach() noexcept {
    if (!scope)
      return;
    if (previous)
      previous->next = next;
    else
      scope->first = next;
    if (next)
      next->previous = previous;
    scope = nullptr;
    previous = next = nullptr;
  }
  ~FileState() {
    {
      std::lock_guard lock(resourceMutex());
      detach();
    }
    deferClose(file, closeSlot);
  }
  static void cleanup(void *p) noexcept { static_cast<FileState *>(p)->~FileState(); }
};
struct ResourceControl {
  Invocation::OwnershipScope storage;
  ResourceScope *scope = nullptr;
  explicit ResourceControl(Invocation &ctx) noexcept : storage(ctx) {}
  ~ResourceControl() {
    if (!scope)
      return;
    std::lock_guard lock(resourceMutex());
    scope->active = false;
    while (scope->first) {
      auto *file = scope->first;
      file->detach();
      // This control still owns the scope until its storage retires. Drop the
      // file's metadata lease now, rather than retaining an inactive ancestor
      // chain until a request releases the final physical file backing.
      file->scopeOwner = {};
      file->retired.store(true, std::memory_order_release);
      // An in-flight service owns its lease and closes after its OS operation.
      // Source cleanup must never wait for that operation's native mutex.
      std::unique_lock native(file->mutex, std::try_to_lock);
      if (native)
        deferClose(file->file, file->closeSlot);
    }
  }
  static void cleanup(void *p) noexcept { static_cast<ResourceControl *>(p)->~ResourceControl(); }
};
ResourceControl *currentScope(Invocation &ctx) noexcept {
  return static_cast<ResourceControl *>(ctx.latestControl(ResourceControl::cleanup));
}
void adopt(Invocation &ctx, FileState &file, ResourceScope *scope) noexcept {
  auto owner = scope ? ctx.lease(scope) : std::optional<AllocationLease>{};
  if (scope && !owner) {
    ctx.fault(3);
    return;
  }
  std::lock_guard lock(resourceMutex());
  file.detach();
  file.scopeOwner = owner ? std::move(*owner) : AllocationLease{};
  if (!scope)
    return;
  file.scope = scope;
  file.next = scope->first;
  if (file.next)
    file.next->previous = &file;
  scope->first = &file;
}
FileDescriptor descriptor(FileState *file) noexcept {
  FileDescriptor result;
  if (file) {
    auto &s = result.storage;
    s.allocated = s.aligned = reinterpret_cast<std::uint8_t *>(file);
    s.sizes[0] = static_cast<std::int64_t>(sizeof(FileState) + file->capacity);
    s.strides[0] = 1;
  }
  return result;
}
FileState *createFile(Invocation &ctx, std::uint64_t capacity) noexcept {
  if (capacity > std::uint64_t(INT64_MAX) - sizeof(FileState)) {
    ctx.fault(2);
    return nullptr;
  }
  auto *p = ctx.allocateResource(sizeof(FileState) + capacity, FileState::cleanup);
  return p ? new (p) FileState(capacity) : nullptr;
}
FileState *inspectFile(Invocation &ctx, const FileDescriptor &value) noexcept {
  const auto &s = value.storage;
  if (!s.allocated || s.aligned != s.allocated || s.offset || s.strides[0] != 1 ||
      s.sizes[0] < std::int64_t(sizeof(FileState))) {
    ctx.fault(3);
    return nullptr;
  }
  auto *p = static_cast<FileState *>(
      ctx.readResource(s.allocated, FileState::cleanup, std::uint64_t(s.sizes[0])));
  if (p && std::uint64_t(s.sizes[0]) != sizeof(FileState) + p->capacity) {
    ctx.fault(3);
    return nullptr;
  }
  return p;
}
struct Job {
  std::atomic<bool> done{false}, abandoned{false};
  FileAction action;
  FileKind kind = FileKind::Success;
  std::int32_t error = 0;
  std::uint64_t value = 0, mode = 0, size = 0, count = 0;
  FileState *file = nullptr, *output = nullptr;
  AllocationLease fileOwner, outputOwner;
  explicit Job(FileAction action) noexcept : action(action) {}
  unsigned char *data() noexcept { return reinterpret_cast<unsigned char *>(this + 1); }
  static void cleanup(void *p) noexcept { static_cast<Job *>(p)->~Job(); }
  void system(std::int32_t code) noexcept {
    kind = FileKind::System;
    error = code;
  }
};
std::uint64_t nativeRead(Job &job, FileState &file, unsigned char *bytes,
                         std::uint64_t count) noexcept {
  if (job.abandoned.load(std::memory_order_acquire))
    return 0;
  const auto request = static_cast<std::uint32_t>(std::min<std::uint64_t>(count, 65536));
#ifdef _WIN32
  DWORD actual = 0;
  if (!ReadFile(file.file, bytes, request, &actual, nullptr)) {
    const auto error = lastError();
    if (error != ERROR_HANDLE_EOF)
      job.system(error);
    return 0;
  }
  return actual;
#else
  for (;;) {
    const auto actual = ::read(file.file, bytes, request);
    if (actual >= 0)
      return static_cast<std::uint64_t>(actual);
    if (errno != EINTR) {
      job.system(lastError());
      return 0;
    }
    if (job.abandoned.load(std::memory_order_acquire))
      return 0;
  }
#endif
}
std::uint64_t nativeWrite(Job &job, FileState &file, const unsigned char *bytes,
                          std::uint64_t count) noexcept {
  if (job.abandoned.load(std::memory_order_acquire))
    return 0;
  const auto request = static_cast<std::uint32_t>(std::min<std::uint64_t>(count, 65536));
#ifdef _WIN32
  DWORD actual = 0;
  if (!WriteFile(file.file, bytes, request, &actual, nullptr)) {
    job.system(lastError());
    return 0;
  }
  if (!actual && request)
    job.system(ioError);
  return actual;
#else
  for (;;) {
    const auto actual = ::write(file.file, bytes, request);
    if (actual >= 0) {
      if (!actual && request)
        job.system(ioError);
      return static_cast<std::uint64_t>(actual);
    }
    if (errno != EINTR) {
      job.system(lastError());
      return 0;
    }
    if (job.abandoned.load(std::memory_order_acquire))
      return 0;
  }
#endif
}
void flush(Job &job, FileState &file) noexcept {
  if (file.buffer != FileState::Buffer::Write)
    return;
  while (file.begin < file.end && job.kind == FileKind::Success && !job.abandoned) {
    const auto n = nativeWrite(job, file, file.data() + file.begin, file.end - file.begin);
    file.begin += n;
  }
  if (file.begin == file.end)
    file.begin = file.end = 0;
}
std::uint64_t read(Job &job, FileState &file, unsigned char *bytes, std::uint64_t count) noexcept {
  if (file.buffer != FileState::Buffer::Read)
    return nativeRead(job, file, bytes, count);
  if (!count)
    return 0;
  if (file.begin == file.end) {
    file.begin = 0;
    file.end = nativeRead(job, file, file.data(), file.capacity);
  }
  const auto n = std::min(count, file.end - file.begin);
  if (n)
    std::memcpy(bytes, file.data() + file.begin, n);
  file.begin += n;
  return n;
}
std::uint64_t write(Job &job, FileState &file, const unsigned char *bytes,
                    std::uint64_t count) noexcept {
  if (file.buffer != FileState::Buffer::Write)
    return nativeWrite(job, file, bytes, count);
  if (file.begin)
    flush(job, file);
  if (job.kind != FileKind::Success || job.abandoned)
    return 0;
  if (file.end == file.capacity)
    flush(job, file);
  if (job.kind != FileKind::Success || job.abandoned)
    return 0;
  const auto n = std::min(count, file.capacity - file.end);
  if (n)
    std::memcpy(file.data() + file.end, bytes, n);
  file.end += n;
  return n;
}
std::int32_t closeFile(FileState &file) noexcept {
  const auto result = closeNative(std::exchange(file.file, invalidFile));
  releaseFileSlot(std::exchange(file.closeSlot, noFileSlot));
  return result;
}
void open(Job &job) noexcept {
  const auto slot = reserveFileSlot();
  if (slot == noFileSlot) {
    job.kind = FileKind::ResourceLimit;
    return;
  }
  job.output->closeSlot = slot;
#ifdef _WIN32
  // Source paths are validated UTF-8; allocate no uncharged variable storage in
  // the worker. The request reserves this fixed Windows path conversion space.
  std::array<wchar_t, 32768> path;
  const auto n =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char *>(job.data()),
                          -1, path.data(), int(path.size()));
  if (!n) {
    job.system(lastError());
    return;
  }
  DWORD access = GENERIC_READ, creation = OPEN_EXISTING;
  switch (static_cast<FileMode>(job.mode)) {
  case FileMode::Read:
    break;
  case FileMode::Write:
    access = GENERIC_WRITE;
    break;
  case FileMode::Create:
    access = GENERIC_WRITE;
    creation = CREATE_ALWAYS;
    break;
  case FileMode::CreateNew:
    access = GENERIC_WRITE;
    creation = CREATE_NEW;
    break;
  case FileMode::Append:
    access = FILE_APPEND_DATA;
    creation = OPEN_ALWAYS;
    break;
  case FileMode::ReadWrite:
    access = GENERIC_READ | GENERIC_WRITE;
    break;
  }
  job.output->file =
      CreateFileW(path.data(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
  int flags = O_RDONLY;
  switch (static_cast<FileMode>(job.mode)) {
  case FileMode::Read:
    break;
  case FileMode::Write:
    flags = O_WRONLY;
    break;
  case FileMode::Create:
    flags = O_WRONLY | O_CREAT | O_TRUNC;
    break;
  case FileMode::CreateNew:
    flags = O_WRONLY | O_CREAT | O_EXCL;
    break;
  case FileMode::Append:
    flags = O_WRONLY | O_CREAT | O_APPEND;
    break;
  case FileMode::ReadWrite:
    flags = O_RDWR;
    break;
  }
  do {
    job.output->file = ::open(reinterpret_cast<const char *>(job.data()), flags | O_CLOEXEC, 0666);
  } while (job.output->file == invalidFile && errno == EINTR && !job.abandoned);
#endif
  if (job.output->file == invalidFile) {
    job.system(lastError());
    releaseFileSlot(std::exchange(job.output->closeSlot, noFileSlot));
  }
}
void execute(Job &job) noexcept {
  if (job.abandoned)
    return;
  if (job.action == FileAction::Open) {
    open(job);
    return;
  }
  auto &file = *job.file;
  std::lock_guard lock(file.mutex);
  if (file.file == invalidFile) {
    job.kind = FileKind::InvalidArgument;
    return;
  }
  switch (job.action) {
  case FileAction::Open:
    break;
  case FileAction::BufferedReader:
  case FileAction::BufferedWriter:
    if (file.buffer != FileState::Buffer::None || !job.value) {
      job.kind = FileKind::InvalidArgument;
      (void)closeFile(file);
      break;
    }
    job.output->file = std::exchange(file.file, invalidFile);
    job.output->closeSlot = std::exchange(file.closeSlot, noFileSlot);
    job.output->buffer = job.action == FileAction::BufferedReader ? FileState::Buffer::Read
                                                                  : FileState::Buffer::Write;
    break;
  case FileAction::Read:
  case FileAction::ReadInto:
  case FileAction::ReadExact:
    if (file.buffer == FileState::Buffer::Write) {
      job.kind = FileKind::InvalidArgument;
      break;
    }
    if (!job.size)
      break;
    do {
      const auto n = read(job, file, job.data() + job.count, job.size - job.count);
      job.count += n;
      if (!n || job.action != FileAction::ReadExact)
        break;
    } while (job.count < job.size && job.kind == FileKind::Success && !job.abandoned);
    if (job.action == FileAction::ReadExact && job.count < job.size &&
        job.kind == FileKind::Success && !job.abandoned)
      job.kind = FileKind::UnexpectedEnd;
    break;
  case FileAction::ReadLine: {
    if (file.buffer == FileState::Buffer::Write) {
      job.kind = FileKind::InvalidArgument;
      break;
    }
    bool seen = false, pendingCR = false, oversized = false;
    const auto append = [&](unsigned char ch) {
      if (job.count == job.size)
        oversized = true;
      else
        job.data()[job.count++] = ch;
    };
    while (!job.abandoned && job.kind == FileKind::Success) {
      unsigned char ch;
      if (!read(job, file, &ch, 1)) {
        if (pendingCR)
          append('\r');
        break;
      }
      seen = true;
      if (ch == '\n')
        break;
      if (pendingCR)
        append('\r');
      pendingCR = ch == '\r';
      if (!pendingCR)
        append(ch);
    }
    if (job.kind != FileKind::Success || job.abandoned)
      break;
    if (!seen) {
      job.kind = FileKind::End;
      break;
    }
    if (oversized) {
      job.kind = FileKind::LineTooLong;
      break;
    }
    const auto status = kalandra::validateUtf8(
        std::string_view(reinterpret_cast<const char *>(job.data()), job.count),
        [&] { return !job.abandoned; });
    if (status == kalandra::Utf8Status::Invalid)
      job.kind = FileKind::InvalidEncoding;
    break;
  }
  case FileAction::Write:
  case FileAction::WriteAll:
  case FileAction::WriteText:
  case FileAction::WriteLine:
    if (file.buffer == FileState::Buffer::Read) {
      job.kind = FileKind::InvalidArgument;
      break;
    }
    if (!job.size)
      break;
    do {
      const auto n = write(job, file, job.data() + job.count, job.size - job.count);
      job.count += n;
      if (!n || job.action == FileAction::Write)
        break;
    } while (job.count < job.size && job.kind == FileKind::Success && !job.abandoned);
    break;
  case FileAction::Flush:
    flush(job, file);
    break;
  case FileAction::Finish: {
    flush(job, file);
    const auto error = closeFile(file);
    if (error && job.kind == FileKind::Success)
      job.system(error);
    file.begin = file.end = 0;
    break;
  }
  case FileAction::Close: {
    const auto error = closeFile(file);
    if (error)
      job.system(error);
    file.begin = file.end = 0;
    break;
  }
  case FileAction::SeekTo:
    if (job.value > std::uint64_t(INT64_MAX)) {
      job.kind = FileKind::InvalidArgument;
      break;
    }
    flush(job, file);
    if (job.kind != FileKind::Success || job.abandoned)
      break;
#ifdef _WIN32
    {
      LARGE_INTEGER offset;
      offset.QuadPart = static_cast<LONGLONG>(job.value);
      if (!SetFilePointerEx(file.file, offset, nullptr, FILE_BEGIN))
        job.system(lastError());
    }
#else
    if (::lseek(file.file, static_cast<off_t>(job.value), SEEK_SET) == off_t(-1))
      job.system(lastError());
#endif
    // Retain read-ahead on failed seek: its logical cursor is still valid.
    if (job.kind == FileKind::Success)
      file.begin = file.end = 0;
    break;
  }
}
// Fixed-size executor: OS calls own request bytes and resource leases, never a
// source Invocation, stack/frame pointer or array backing that can be mutated.
// Cancellation retires source state without waiting for a blocked OS request.
struct Pending {
  Job *job = nullptr;
  AllocationLease owner;
};
struct Executor {
  std::mutex mutex;
  std::condition_variable changed;
  std::array<Pending, 64> queue;
  struct CloseSlot {
    NativeFile file = invalidFile;
    bool reserved = false;
  };
  std::array<CloseSlot, fileSlots> closes;
  std::array<std::uint32_t, fileSlots> closeQueue;
  std::size_t closeBegin = 0, closeCount = 0, nextSlot = 0, reserved = 0;
  std::size_t begin = 0, count = 0, workers = 0, active = 0;
  void worker() noexcept {
#ifndef _WIN32
    // These dedicated service threads never run source code. Keep SIGPIPE
    // blocked so a broken FIFO is an ordinary OS error, not process termination.
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    const auto signalError = pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
#endif
    for (;;) {
      Pending current;
      NativeFile closing = invalidFile;
      std::size_t closeIndex = noFileSlot;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return count != 0 || closeCount != 0; });
        if (closeCount) {
          closeIndex = closeQueue[closeBegin];
          closeBegin = (closeBegin + 1) % fileSlots;
          --closeCount;
          closing = closes[closeIndex].file;
        } else {
          current = std::move(queue[begin]);
          begin = (begin + 1) % queue.size();
          --count;
        }
        ++active;
      }
      if (closeIndex != noFileSlot) {
        (void)closeNative(closing);
        {
          std::lock_guard lock(mutex);
          closes[closeIndex] = {};
          --reserved;
          --active;
        }
        continue;
      }
#ifndef _WIN32
      if (signalError)
        current.job->system(signalError);
      else
#endif
        execute(*current.job);
      if (current.job->file && current.job->file->retired.load(std::memory_order_acquire)) {
        std::lock_guard native(current.job->file->mutex);
        deferClose(current.job->file->file, current.job->file->closeSlot);
      }
      if (current.job->output && current.job->output->retired.load(std::memory_order_acquire)) {
        std::lock_guard native(current.job->output->mutex);
        deferClose(current.job->output->file, current.job->output->closeSlot);
      }
      current.job->done.store(true, std::memory_order_release);
      {
        std::lock_guard lock(mutex);
        --active;
      }
    }
  }
  bool submit(Pending &pending) {
    std::lock_guard lock(mutex);
    if (count == queue.size())
      return false;
    // Match the default structured task in-flight bound. A tiny blocking pool
    // can deadlock when its FIFO readers depend on a sibling's queued write.
    // Grow only when current requests need another service, never per call.
    if (workers < 64 && active + count + 1 > workers) {
      std::thread([this] { worker(); }).detach();
      ++workers;
    }
    queue[(begin + count) % queue.size()] = std::move(pending);
    ++count;
    changed.notify_one();
    return true;
  }
};
Executor &executor() {
  static auto *value = new Executor;
  return *value;
}
std::size_t reserveFileSlot() {
  auto &service = executor();
  std::lock_guard lock(service.mutex);
  if (service.reserved == fileSlots)
    return noFileSlot;
  for (std::size_t n = 0; n < fileSlots; ++n) {
    const auto slot = service.nextSlot++ % fileSlots;
    if (!service.closes[slot].reserved) {
      service.closes[slot].reserved = true;
      ++service.reserved;
      return slot;
    }
  }
  return noFileSlot;
}
void releaseFileSlot(std::size_t slot) noexcept {
  if (slot == noFileSlot)
    return;
  auto &service = executor();
  std::lock_guard lock(service.mutex);
  service.closes[slot] = {};
  --service.reserved;
}
void deferClose(NativeFile &file, std::size_t &slot) noexcept {
  if (file == invalidFile) {
    releaseFileSlot(std::exchange(slot, noFileSlot));
    return;
  }
  // Valid files acquired a slot before their OS open. No allocation, source
  // pointer, flush, native close or worker join occurs on this cleanup path.
  auto &service = executor();
  std::lock_guard lock(service.mutex);
  const auto index = std::exchange(slot, noFileSlot);
  service.closes[index].file = std::exchange(file, invalidFile);
  service.closeQueue[(service.closeBegin + service.closeCount) % fileSlots] = std::uint32_t(index);
  ++service.closeCount;
  service.changed.notify_one();
}
struct Operation {
  Invocation::OwnershipScope storage;
  Job *job = nullptr;
  AllocationLease owner;
  bool submitted = false;
  explicit Operation(Invocation &ctx) noexcept : storage(ctx) {}
  ~Operation() {
    if (job)
      job->abandoned.store(true, std::memory_order_release);
  }
  static void cleanup(void *p) noexcept { static_cast<Operation *>(p)->~Operation(); }
};
void systemError(Invocation &ctx, FileResult &out, std::int32_t code) noexcept {
  char message[80] = "filesystem I/O failed (system code ";
  auto *start = message + std::strlen(message);
  const auto result = std::to_chars(start, message + sizeof(message) - 1, code);
  if (result.ec != std::errc{}) {
    ctx.fault(2);
    return;
  }
  *result.ptr = ')';
  kalandra_string_literal_v1(&ctx, &out.text, message, result.ptr + 1 - message);
}
bool needsBytes(FileAction action) noexcept {
  return action == FileAction::Write || action == FileAction::WriteAll ||
         action == FileAction::ReadInto;
}
bool writes(FileAction action) noexcept {
  return action == FileAction::Write || action == FileAction::WriteAll;
}
bool writesText(FileAction action) noexcept {
  return action == FileAction::WriteText || action == FileAction::WriteLine;
}
bool returnsBytes(FileAction action) noexcept {
  return action == FileAction::Read || action == FileAction::ReadExact;
}
Operation *prepare(Invocation &ctx, FileResult &out, FileAction action, const FileArguments &args,
                   void **state) {
  auto *memory = ctx.allocateControl(sizeof(Operation), Operation::cleanup);
  if (!memory)
    return nullptr;
  auto *op = new (memory) Operation(ctx);
  *state = op;
  if (!op->storage.valid())
    return nullptr;
  FileState *file = nullptr;
  if (action != FileAction::Open) {
    file = inspectFile(ctx, args.file);
    if (!file)
      return nullptr;
  }
  std::uint64_t size = 0;
  if (action == FileAction::Open) {
    if (!validateString(ctx, args.path))
      return nullptr;
    if (args.mode > std::uint64_t(FileMode::ReadWrite) ||
        (args.path.byteLength && std::memchr(args.path.data, 0, args.path.byteLength))) {
      out.kind = FileKind::InvalidArgument;
      return nullptr;
    }
    size = std::uint64_t(args.path.byteLength) + 1;
  } else if (writesText(action)) {
    if (!validateString(ctx, args.path))
      return nullptr;
    size = std::uint64_t(args.path.byteLength) + (action == FileAction::WriteLine ? 1 : 0);
  } else if (needsBytes(action)) {
    kalandra_array_validate_v1(&ctx, &args.bytes, std::uint64_t(kalandra::NumericType::UInt8));
    if (!healthy(ctx))
      return nullptr;
    size = std::uint64_t(args.bytes.sizes[0]);
  } else if (returnsBytes(action) || action == FileAction::ReadLine)
    size = args.value;
  if (size > std::uint64_t(INT64_MAX) - sizeof(Job)) {
    ctx.fault(2);
    return nullptr;
  }
  {
    Invocation::OwnershipSelection select(op->storage);
    auto *storage = ctx.allocateResource(sizeof(Job) + size, Job::cleanup);
    if (!storage)
      return nullptr;
    auto *job = new (storage) Job(action);
    op->job = job;
    op->owner = std::move(*ctx.lease(job));
    job->file = file;
    job->value = args.value;
    job->mode = args.mode;
    job->size = size;
    if (file)
      job->fileOwner = std::move(*ctx.lease(file));
    if (action == FileAction::Open || action == FileAction::BufferedReader ||
        action == FileAction::BufferedWriter) {
      job->output = createFile(ctx, action == FileAction::Open ? 0 : args.value);
      if (!job->output)
        return nullptr;
      job->outputOwner = std::move(*ctx.lease(job->output));
    }
    if (action == FileAction::Open) {
      if (args.path.byteLength)
        std::memcpy(job->data(), args.path.data, args.path.byteLength);
      job->data()[args.path.byteLength] = 0;
    } else if (writesText(action)) {
      for (std::uint64_t offset = 0; offset < std::uint64_t(args.path.byteLength);) {
        if (!healthy(ctx))
          return nullptr;
        const auto n = std::min<std::uint64_t>(1024, std::uint64_t(args.path.byteLength) - offset);
        std::memcpy(job->data() + offset, args.path.data + offset, n);
        offset += n;
      }
      if (action == FileAction::WriteLine)
        job->data()[size - 1] = '\n';
    } else if (writes(action)) {
      for (std::uint64_t i = 0; i < size; ++i) {
        if (i % 1024 == 0 && !healthy(ctx))
          return nullptr;
        job->data()[i] =
            args.bytes.aligned[args.bytes.offset + std::int64_t(i) * args.bytes.strides[0]];
      }
    }
  }
  return op;
}
bool step(Invocation &ctx, FileResult &out, FileAction action, const FileArguments &args,
          void **state) {
  auto *op = *state ? static_cast<Operation *>(*state) : prepare(ctx, out, action, args, state);
  if (!op || !op->job || !healthy(ctx))
    return true;
  auto &job = *op->job;
  if (!op->submitted) {
    auto retained = ctx.lease(&job);
    if (!retained) {
      ctx.fault(3);
      return true;
    }
    Pending pending{&job, std::move(*retained)};
    if (!executor().submit(pending))
      return false;
    op->submitted = true;
  }
  if (!job.done.load(std::memory_order_acquire))
    return false;
  if (job.kind == FileKind::ResourceLimit) {
    ctx.fault(2);
    return true;
  }
  out.kind = job.kind;
  out.code = job.error;
  out.count = job.count;
  if (job.kind == FileKind::System) {
    systemError(ctx, out, job.error);
    return true;
  }
  if (job.kind != FileKind::Success)
    return true;
  if (job.output) {
    if (!ctx.retainFrom(ctx, job.output))
      return true;
    if (auto *scope = currentScope(ctx))
      adopt(ctx, *job.output, scope->scope);
    if (!healthy(ctx))
      return true;
    out.file = descriptor(job.output);
  } else if (action == FileAction::ReadInto) {
    // The worker never received this destination. Publish bytes only into the
    // still-live exclusive caller array after the operation has completed.
    kalandra_array_validate_v1(&ctx, &args.bytes, std::uint64_t(kalandra::NumericType::UInt8));
    if (!healthy(ctx))
      return true;
    for (std::uint64_t i = 0; i < job.count; ++i) {
      if (i % 1024 == 0 && !healthy(ctx))
        return true;
      args.bytes.aligned[args.bytes.offset + std::int64_t(i) * args.bytes.strides[0]] =
          job.data()[i];
    }
  } else if ((returnsBytes(action) || action == FileAction::ReadLine) && job.count) {
    auto *data = static_cast<std::uint8_t *>(ctx.allocateRecord(job.count));
    if (!data)
      return true;
    for (std::uint64_t offset = 0; offset < job.count;) {
      if (!healthy(ctx))
        return true;
      const auto n = std::min<std::uint64_t>(1024, job.count - offset);
      std::memcpy(data + offset, job.data() + offset, n);
      offset += n;
    }
    if (action == FileAction::ReadLine)
      out.text = {data, reinterpret_cast<const char *>(data), std::int64_t(job.count)};
    else {
      out.bytes.allocated = out.bytes.aligned = data;
      out.bytes.sizes[0] = std::int64_t(job.count);
      out.bytes.strides[0] = 1;
    }
  } else if (returnsBytes(action))
    out.bytes.strides[0] = 1;
  return true;
}
} // namespace
extern "C" bool kalandra_file_step_v1(Invocation *ctx, FileResult *out, std::uint32_t action,
                                      const FileArguments *args, void **state) noexcept {
  *out = {};
  bool done = true;
  try {
    if (healthy(*ctx)) {
      if (action > std::uint32_t(FileAction::WriteLine))
        ctx->fault(3);
      else
        done = step(*ctx, *out, static_cast<FileAction>(action), *args, state);
    }
  } catch (...) {
    ctx->fault(2);
  }
  if (!healthy(*ctx)) {
    done = true;
    *out = {};
  }
  if (done && *state) {
    ctx->releaseControl(*state);
    *state = nullptr;
  }
  return done;
}
extern "C" void kalandra_file_v1(Invocation *ctx, FileResult *out, std::uint32_t action,
                                 const FileArguments *args) noexcept {
  void *state = nullptr;
  while (!kalandra_file_step_v1(ctx, out, action, args, &state))
    std::this_thread::sleep_for(1ms);
}
extern "C" void kalandra_file_pause_v1(kalandra::runtime::Task *task, void *) noexcept {
  // Reuse the fixed, bounded five-millisecond scheduler notification mailbox.
  kalandra_console_pause_v1(task, nullptr);
}

extern "C" void *kalandra_file_scope_enter_v1(Invocation *ctx, const FileDescriptor *const *owned,
                                              std::uint64_t count) noexcept {
  if (!healthy(*ctx))
    return nullptr;
  auto *parent = currentScope(*ctx);
  auto *memory = ctx->allocateControl(sizeof(ResourceControl), ResourceControl::cleanup);
  if (!memory)
    return nullptr;
  auto *control = new (memory) ResourceControl(*ctx);
  if (!control->storage.valid())
    return nullptr;
  {
    Invocation::OwnershipSelection selected(control->storage);
    auto *data = ctx->allocateResource(sizeof(ResourceScope), ResourceScope::cleanup);
    if (!data)
      return nullptr;
    control->scope = new (data) ResourceScope;
    if (parent) {
      control->scope->parent = parent->scope;
      auto lease = ctx->lease(parent->scope);
      if (!lease) {
        ctx->fault(3);
        return nullptr;
      }
      control->scope->parentOwner = std::move(*lease);
    }
    for (std::uint64_t n = 0; n < count; ++n) {
      if (!owned || !owned[n]) {
        ctx->fault(3);
        return nullptr;
      }
      auto *file = inspectFile(*ctx, *owned[n]);
      if (!file)
        return nullptr;
      adopt(*ctx, *file, control->scope);
    }
  }
  return healthy(*ctx) ? control : nullptr;
}
extern "C" void kalandra_file_scope_leave_v1(Invocation *ctx, void *scope,
                                             const FileDescriptor *escaping) noexcept {
  if (!scope)
    return;
  auto *control = static_cast<ResourceControl *>(scope);
  if (escaping && healthy(*ctx)) {
    if (auto *file = inspectFile(*ctx, *escaping))
      adopt(*ctx, *file, control->scope->parent);
  }
  ctx->releaseControl(scope);
}
extern "C" bool kalandra_file_receive_v1(Invocation *ctx, const FileDescriptor *value) noexcept {
  auto *file = inspectFile(*ctx, *value);
  if (!file)
    return false;
  bool unowned;
  {
    std::lock_guard lock(resourceMutex());
    unowned = file->scope == nullptr && !file->retired.load(std::memory_order_acquire);
  }
  // A returned worker value has left its producer's logical scope. Adopt it in
  // its receiver; ordinary borrowed task parameters keep their existing owner.
  if (unowned)
    if (auto *scope = currentScope(*ctx))
      adopt(*ctx, *file, scope->scope);
  return healthy(*ctx);
}

kalandra::runtime::FileServiceStats kalandra::runtime::fileServiceStats() noexcept {
  auto &service = executor();
  std::lock_guard lock(service.mutex);
  return {std::uint32_t(service.reserved), std::uint32_t(service.closeCount),
          std::uint32_t(service.workers)};
}
