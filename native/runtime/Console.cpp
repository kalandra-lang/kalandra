#include "Console.h"
#include "../include/kalandra/Utf8.h"
#include "Tasks.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <unistd.h>
#endif

namespace {
using namespace kalandra::runtime;
using namespace std::chrono_literals;
constexpr std::size_t bufferBytes = 1024;
#ifdef _WIN32
constexpr std::int32_t zeroWriteError = ERROR_WRITE_FAULT;
#else
constexpr std::int32_t zeroWriteError = EIO;
#endif
struct Stream {
  // Logical ownership spans task resumes on different physical workers.
  std::mutex state;
  const void *owner = nullptr;
  ExternalCompletion waiter;
  std::condition_variable changed;
  std::array<char, bufferBytes> bytes{};
  std::size_t begin = 0, end = 0, requested = 0, transferred = 0;
  std::int32_t error = 0;
  bool pending = false, asynchronous = false, helper = false, invalidEncoding = false;
#ifdef _WIN32
  wchar_t highSurrogate = 0;
#endif
};
// Process-lifetime services may be blocked in OS I/O at exit. They never retain
// an Invocation or source backing, and do not run source cleanup/destructors.
Stream &stream(unsigned id) {
  static auto *streams = new std::array<Stream, 3>;
  return streams->at(id);
}
#ifndef _WIN32
struct PipeSignalScope {
  sigset_t previous{}, one{};
  bool changed = false, wasPending = false;
  int error = 0;
  PipeSignalScope() noexcept {
    sigemptyset(&one);
    sigaddset(&one, SIGPIPE);
    error = pthread_sigmask(SIG_BLOCK, &one, &previous);
    if (error)
      return;
    changed = true;
    sigset_t pending;
    if (sigpending(&pending) == 0)
      wasPending = sigismember(&pending, SIGPIPE) == 1;
  }
  void consume() noexcept {
    if (!changed || wasPending)
      return;
    sigset_t pending;
    if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
      int received = 0;
      (void)sigwait(&one, &received);
    }
  }
  ~PipeSignalScope() {
    if (changed)
      (void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  }
};
#endif
void operate(Stream &s, unsigned id) noexcept {
  s.error = 0;
  s.transferred = 0;
  s.invalidEncoding = false;
#ifdef _WIN32
  HANDLE handle = GetStdHandle(id == 0   ? STD_INPUT_HANDLE
                               : id == 1 ? STD_OUTPUT_HANDLE
                                         : STD_ERROR_HANDLE);
  DWORD mode = 0, count = 0;
  if (GetConsoleMode(handle, &mode)) {
    if (id == 0) {
      std::array<wchar_t, 256> wide{};
      for (;;) {
        const unsigned prefix = s.highSurrogate ? 1 : 0;
        if (prefix)
          wide[0] = s.highSurrogate;
        s.highSurrogate = 0;
        if (!ReadConsoleW(handle, wide.data() + prefix, DWORD(wide.size() - prefix), &count,
                          nullptr)) {
          s.error = static_cast<std::int32_t>(GetLastError());
          return;
        }
        if (!count) {
          s.invalidEncoding = prefix != 0;
          return;
        }
        count += prefix;
        if (wide[count - 1] >= 0xd800 && wide[count - 1] <= 0xdbff)
          s.highSurrogate = wide[--count];
        if (count)
          break;
        // A split surrogate is buffered by the service, never by a source task.
      }
      const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), int(count),
                                            s.bytes.data(), int(s.bytes.size()), nullptr, nullptr);
      if (!size) {
        const auto error = GetLastError();
        if (error == ERROR_NO_UNICODE_TRANSLATION)
          s.invalidEncoding = true;
        else
          s.error = static_cast<std::int32_t>(error);
      } else
        s.transferred = size;
    } else {
      std::array<wchar_t, bufferBytes> wide{};
      const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.bytes.data(),
                                            int(s.requested), wide.data(), int(wide.size()));
      if (!size) {
        s.error = static_cast<std::int32_t>(GetLastError());
        return;
      }
      if (!WriteConsoleW(handle, wide.data(), DWORD(size), &count, nullptr)) {
        s.error = static_cast<std::int32_t>(GetLastError());
        return;
      }
      // Translate partial UTF-16 output back to its UTF-8 byte count.
      if (count && count < unsigned(size) && wide[count - 1] >= 0xd800 &&
          wide[count - 1] <= 0xdbff) {
        s.error = ERROR_NO_UNICODE_TRANSLATION;
        return;
      }
      s.transferred =
          count == unsigned(size)
              ? s.requested
              : std::size_t(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                                int(count), nullptr, 0, nullptr, nullptr));
    }
  } else if (id == 0) {
    if (!ReadFile(handle, s.bytes.data(), DWORD(s.bytes.size()), &count, nullptr)) {
      const auto error = GetLastError();
      if (error != ERROR_BROKEN_PIPE && error != ERROR_HANDLE_EOF)
        s.error = static_cast<std::int32_t>(error);
    } else
      s.transferred = count;
  } else {
    if (!WriteFile(handle, s.bytes.data(), DWORD(s.requested), &count, nullptr))
      s.error = static_cast<std::int32_t>(GetLastError());
    else
      s.transferred = count;
  }
#else
  if (id == 0) {
    ssize_t count;
    do {
      count = ::read(0, s.bytes.data(), s.bytes.size());
    } while (count < 0 && errno == EINTR);
    if (count < 0)
      s.error = errno;
    else
      s.transferred = std::size_t(count);
  } else {
    PipeSignalScope signals;
    if (signals.error) {
      s.error = signals.error;
      return;
    }
    ssize_t count;
    do {
      count = ::write(int(id), s.bytes.data(), s.requested);
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
      s.error = errno;
      if (s.error == EPIPE)
        signals.consume();
    } else
      s.transferred = std::size_t(count);
  }
#endif
  if (id == 0) {
    s.begin = 0;
    s.end = s.transferred;
  }
}
bool healthy(Invocation &ctx) noexcept { return ctx.poll() == 0 && ctx.usable(); }
bool request(Invocation &ctx, Stream &s, unsigned id, std::unique_lock<std::mutex> &lock) {
  if (!healthy(ctx))
    return false;
  if (!ctx.cancellable()) {
    // Keep the logical owner while releasing the metadata mutex: another
    // cancellable caller must be able to retire while this OS call blocks.
    s.pending = true;
    s.asynchronous = false;
    lock.unlock();
    operate(s, id);
    lock.lock();
    s.pending = false;
    return true;
  }
  if (!s.helper) {
    std::thread([&s, id] {
      std::unique_lock worker(s.state);
      for (;;) {
        s.changed.wait(worker, [&] { return s.pending && s.asynchronous; });
        worker.unlock();
        operate(s, id);
        worker.lock();
        s.pending = false;
        s.asynchronous = false;
        auto completion = s.waiter;
        s.waiter = {};
        worker.unlock();
        if (completion)
          (void)completion.signal();
        worker.lock();
      }
    }).detach();
    s.helper = true;
  }
  s.pending = true;
  s.asynchronous = true;
  s.changed.notify_all();
  return false;
}
struct Operation {
  Invocation::OwnershipScope storage;
  Stream *stream;
  unsigned id;
  StringDescriptor text{};
  std::uint64_t limit = 0, offset = 0, used = 0, capacity = 0;
  char *data = nullptr;
  std::size_t bytes = 0, written = 0;
  bool newline = false, line = false, requested = false;
  bool seen = false, oversized = false, pendingCR = false;
  Operation(Invocation &ctx, Stream &value, unsigned index)
      : storage(ctx), stream(&value), id(index) {}
  ~Operation() {
    std::lock_guard lock(stream->state);
    if (stream->owner == this)
      stream->owner = nullptr;
  }
};
void destroyOperation(void *value) noexcept { static_cast<Operation *>(value)->~Operation(); }
Operation *operation(Invocation &ctx, void **state, unsigned id) {
  if (*state)
    return static_cast<Operation *>(*state);
  auto &s = stream(id);
  auto *memory = ctx.allocateControl(sizeof(Operation), destroyOperation);
  if (!memory)
    return nullptr;
  auto *op = new (memory) Operation(ctx, s, id);
  *state = op;
  return op;
}
void retire(Invocation &ctx, void **state) noexcept {
  if (*state) {
    ctx.releaseControl(*state);
    *state = nullptr;
  }
}
void systemError(Invocation &ctx, ConsoleResult &out, std::int32_t code) noexcept {
  char message[80] = "console I/O failed (system code ";
  auto *begin = message + std::strlen(message);
  const auto converted = std::to_chars(begin, message + sizeof(message) - 1, code);
  if (converted.ec != std::errc{}) {
    ctx.fault(2);
    return;
  }
  *converted.ptr = ')';
  StringDescriptor text;
  kalandra_string_literal_v1(&ctx, &text, message, converted.ptr + 1 - message);
  if (healthy(ctx))
    out = {ConsoleKind::System, code, text};
}
} // namespace

namespace {
bool writeStep(Invocation &ctx, ConsoleResult &out, const StringDescriptor &text, unsigned id,
               bool newline, void **state) {
  if (!*state && !validateString(ctx, text))
    return true;
  auto *op = operation(ctx, state, id);
  if (!op)
    return true;
  if (!op->storage.valid())
    return true;
  auto &s = *op->stream;
  std::unique_lock lock(s.state, std::try_to_lock);
  if (!lock || (s.owner && s.owner != op) || s.pending)
    return false;
  s.owner = op;
  if (!op->requested && !op->bytes) {
    op->text = text;
    op->newline = newline;
  }
  if (op->requested) {
    op->requested = false;
    if (s.error || !s.transferred) {
      systemError(ctx, out, s.error ? s.error : zeroWriteError);
      return true;
    }
    op->written += s.transferred;
    if (op->written < op->bytes)
      std::memmove(s.bytes.data(), s.bytes.data() + s.transferred, op->bytes - op->written);
    else {
      if (!op->line)
        op->offset += op->bytes;
      op->bytes = op->written = 0;
    }
  }
  for (;;) {
    if (!healthy(ctx))
      return true;
    if (!op->bytes) {
      if (op->offset == std::uint64_t(op->text.byteLength) && (!op->newline || op->line))
        return true;
      auto bytes = std::min<std::size_t>(bufferBytes, op->text.byteLength - op->offset);
      while (bytes && op->offset + bytes < std::uint64_t(op->text.byteLength) &&
             (static_cast<unsigned char>(op->text.data[op->offset + bytes]) & 0xc0) == 0x80)
        --bytes;
      if (bytes)
        std::memcpy(s.bytes.data(), op->text.data + op->offset, bytes);
      else {
        s.bytes[0] = '\n';
        bytes = 1;
        op->line = true;
      }
      op->bytes = bytes;
    }
    s.requested = op->bytes - op->written;
    op->requested = true;
    if (!request(ctx, s, id, lock))
      return false;
    op->requested = false;
    if (s.error || !s.transferred) {
      systemError(ctx, out, s.error ? s.error : zeroWriteError);
      return true;
    }
    op->written += s.transferred;
    if (op->written < op->bytes)
      std::memmove(s.bytes.data(), s.bytes.data() + s.transferred, op->bytes - op->written);
    else {
      if (!op->line)
        op->offset += op->bytes;
      op->bytes = op->written = 0;
    }
  }
}
bool readStep(Invocation &ctx, ConsoleResult &out, std::uint64_t maxBytes, void **state) {
  auto *op = operation(ctx, state, 0);
  if (!op)
    return true;
  if (!op->storage.valid())
    return true;
  auto &s = *op->stream;
  std::unique_lock lock(s.state, std::try_to_lock);
  if (!lock || (s.owner && s.owner != op) || s.pending)
    return false;
  s.owner = op;
  op->limit = maxBytes;
  const auto append = [&](char byte) {
    if (op->oversized)
      return true;
    if (op->used == op->limit) {
      op->oversized = true;
      return true;
    }
    if (op->used == op->capacity) {
      const auto next =
          std::min(op->limit, op->capacity ? op->capacity + op->capacity / 2 + 1 : 64);
      Invocation::OwnershipScope grownScope(ctx);
      if (!grownScope.valid())
        return false;
      char *grown;
      {
        Invocation::OwnershipSelection selected(grownScope);
        grown = static_cast<char *>(ctx.allocateRecord(next));
      }
      if (!grown)
        return false;
      for (std::uint64_t offset = 0; offset < op->used;) {
        if (!healthy(ctx))
          return false;
        const auto bytes = std::min<std::uint64_t>(1024, op->used - offset);
        std::memcpy(grown + offset, op->data + offset, bytes);
        offset += bytes;
      }
      if (!op->storage.swap(grownScope))
        return false;
      op->data = grown;
      op->capacity = next;
    }
    op->data[op->used++] = byte;
    return true;
  };
  unsigned progressed = 0;
  for (;;) {
    if (!healthy(ctx))
      return true;
    if (op->requested) {
      op->requested = false;
      if (s.error) {
        const auto error = s.error;
        s.error = 0;
        systemError(ctx, out, error);
        return true;
      }
      if (s.invalidEncoding) {
        s.invalidEncoding = false;
        out.kind = ConsoleKind::InvalidEncoding;
        return true;
      }
      if (!s.end) {
        if (!op->seen) {
          out.kind = ConsoleKind::End;
          return true;
        }
        if (op->pendingCR && !append('\r'))
          return true;
        break;
      }
    }
    if (s.begin == s.end) {
      op->requested = true;
      if (!request(ctx, s, 0, lock))
        return false;
      continue;
    }
    const char byte = s.bytes[s.begin++];
    op->seen = true;
    if (byte == '\n')
      break;
    if (op->pendingCR && !append('\r'))
      return true;
    op->pendingCR = byte == '\r';
    if (!op->pendingCR && !append(byte))
      return true;
    // A task returns to its scheduler between bounded groups, rather than
    // occupying its worker until another task supplies input or fails.
    if (++progressed == 1024 && ctx.cancellable())
      return false;
  }
  if (!healthy(ctx))
    return true;
  if (op->oversized) {
    out.kind = ConsoleKind::LineTooLong;
    return true;
  }
  const auto valid = kalandra::validateUtf8(std::string_view(op->data ? op->data : "", op->used),
                                            [&] { return healthy(ctx); });
  if (valid == kalandra::Utf8Status::Interrupted)
    return true;
  if (valid == kalandra::Utf8Status::Invalid) {
    out.kind = ConsoleKind::InvalidEncoding;
    return true;
  }
  if (op->used && !ctx.retainFrom(ctx, op->data))
    return true;
  out.text = op->used ? StringDescriptor{op->data, op->data, static_cast<std::int64_t>(op->used)}
                      : StringDescriptor{};
  return true;
}
} // namespace
extern "C" bool kalandra_console_write_step_v1(Invocation *ctx, ConsoleResult *out,
                                               const StringDescriptor *input, std::uint32_t id,
                                               bool newline, void **state) noexcept {
  *out = {};
  bool done = true;
  try {
    if (healthy(*ctx)) {
      if (id != 1 && id != 2)
        ctx->fault(3);
      else
        done = writeStep(*ctx, *out, *input, id, newline, state);
    }
  } catch (...) {
    ctx->fault(2);
  }
  if (!healthy(*ctx)) {
    done = true;
    *out = {};
  }
  if (done)
    retire(*ctx, state);
  return done;
}
extern "C" bool kalandra_console_read_line_step_v1(Invocation *ctx, ConsoleResult *out,
                                                   std::uint64_t maxBytes, void **state) noexcept {
  *out = {};
  bool done = true;
  try {
    if (healthy(*ctx)) {
      if (maxBytes > std::uint64_t(INT64_MAX))
        ctx->fault(2);
      else
        done = readStep(*ctx, *out, maxBytes, state);
    }
  } catch (...) {
    ctx->fault(2);
  }
  if (!healthy(*ctx)) {
    done = true;
    *out = {};
  }
  if (done)
    retire(*ctx, state);
  return done;
}
extern "C" void kalandra_console_write_v1(Invocation *ctx, ConsoleResult *out,
                                          const StringDescriptor *input, std::uint32_t id,
                                          bool newline) noexcept {
  void *state = nullptr;
  while (!kalandra_console_write_step_v1(ctx, out, input, id, newline, &state))
    std::this_thread::sleep_for(1ms);
}
extern "C" void kalandra_console_read_line_v1(Invocation *ctx, ConsoleResult *out,
                                              std::uint64_t maxBytes) noexcept {
  void *state = nullptr;
  while (!kalandra_console_read_line_step_v1(ctx, out, maxBytes, &state))
    std::this_thread::sleep_for(1ms);
}

namespace {
using namespace kalandra::runtime;
// A wait leases a task only until this short scheduler notification, never
// until OS I/O completes. There are no source pointers in this fixed mailbox.
struct Notifications {
  std::mutex mutex;
  std::condition_variable changed;
  std::array<ExternalCompletion, 1024> waiting{};
  std::size_t count = 0;
  Notifications() {
    std::thread([this] {
      for (;;) {
        std::array<ExternalCompletion, 1024> ready;
        {
          std::unique_lock lock(mutex);
          changed.wait(lock, [&] { return count != 0; });
          changed.wait_for(lock, std::chrono::milliseconds(5), [] { return false; });
          ready.swap(waiting);
          count = 0;
        }
        for (const auto &completion : ready)
          if (completion)
            (void)completion.signal();
      }
    }).detach();
  }
  void add(const ExternalCompletion &completion) {
    {
      std::lock_guard lock(mutex);
      if (count < waiting.size()) {
        waiting[count++] = completion;
        changed.notify_one();
        return;
      }
    }
    // Saturation preserves progress and bounded memory; signalling here queues
    // another resume only after the current callback returns to the scheduler.
    (void)completion.signal();
  }
};
Notifications &notifications() {
  static auto *value = new Notifications;
  return *value;
}
} // namespace
extern "C" void kalandra_console_pause_v1(kalandra::runtime::Task *task, void *operation) noexcept {
  ExternalCompletion completion;
  try {
    auto &service = notifications();
    completion = task->suspendExternal();
    if (!completion)
      return;
    bool immediate = false;
    if (operation) {
      auto &op = *static_cast<Operation *>(operation);
      std::lock_guard lock(op.stream->state);
      if (op.stream->owner == &op) {
        if (op.stream->pending)
          op.stream->waiter = completion;
        else
          immediate = true;
      }
    }
    if (immediate)
      (void)completion.signal();
    else
      service.add(completion);
  } catch (...) {
    if (completion)
      (void)completion.signal();
    task->context().fault(2);
  }
}
