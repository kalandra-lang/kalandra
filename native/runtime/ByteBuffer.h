#pragma once
#include "../include/kalandra/BufferOperations.h"
#include "OwnedResources.h"
namespace kalandra::runtime {
using BufferDescriptor = OwnedDescriptor;
using BufferBytes = CpuTensor<NumericType::UInt8, 1>;
enum class BufferKind : std::uint32_t { Success, InvalidSize, OutOfRange };
struct BufferArguments {
  BufferDescriptor buffer;
  BufferBytes bytes;
  std::int64_t value = 0;
  std::uint8_t byte = 0;
  std::uint64_t line = 0, column = 0;
};
struct BufferResult {
  std::uint32_t kind = 0;
  std::int64_t count = 0;
  BufferDescriptor buffer;
  BufferBytes bytes;
};
struct BufferIO {
  void *state = nullptr;
  unsigned char *data = nullptr;
  std::uint64_t size = 0, offset = 0;
  AllocationLease owner, payload;
};
// No worker accesses an Invocation or caller descriptor. These views pin both
// the resource and the exact payload allocation across external completion.
bool bufferReadView(Invocation &, const BufferDescriptor &, std::uint64_t, bool,
                    BufferIO &) noexcept;
bool bufferWriteView(Invocation &, const BufferDescriptor &, BufferIO &) noexcept;
bool bufferReadCommit(Invocation &, BufferIO &, std::uint64_t) noexcept;
struct BufferStats {
  std::uint64_t capacity = 0, length = 0, growths = 0, copiedBytes = 0;
  const void *data = nullptr;
};
BufferStats bufferStats(Invocation &, const BufferDescriptor &) noexcept;
} // namespace kalandra::runtime
extern "C" void kalandra_buffer_v1(kalandra::runtime::Invocation *,
                                   kalandra::runtime::BufferResult *, std::uint32_t,
                                   const kalandra::runtime::BufferArguments *) noexcept;
