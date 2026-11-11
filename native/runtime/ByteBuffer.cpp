#include "ByteBuffer.h"
#include <algorithm>
#include <cstring>
#include <utility>
namespace kalandra::runtime {
namespace {
bool healthy(Invocation &ctx) noexcept { return !ctx.poll() && ctx.usable(); }
struct BufferState : OwnedState {
  AllocationLease payload;
  unsigned char *data = nullptr;
  std::uint64_t length = 0, capacity = 0, growths = 0, copiedBytes = 0;
  BufferState() noexcept
      : OwnedState(OwnedKind::ByteBuffer, sizeof(BufferState), destroyBuffer, retireBuffer) {}
  static void destroyBuffer(void *p) noexcept { static_cast<BufferState *>(p)->~BufferState(); }
  static void retireBuffer(void *p) noexcept {
    auto &s = *static_cast<BufferState *>(p);
    s.payload = {};
    s.data = nullptr;
    s.length = s.capacity = 0;
    s.additionalBytes.store(0, std::memory_order_release);
  }
};
BufferState *inspect(Invocation &ctx, const BufferDescriptor &value) noexcept {
  auto *s = inspectOwned(ctx, value);
  if (s && (s->kind != OwnedKind::ByteBuffer || s->retired.load(std::memory_order_acquire))) {
    ctx.fault(3);
    return nullptr;
  }
  return static_cast<BufferState *>(s);
}
bool copy(Invocation &ctx, unsigned char *out, const unsigned char *in,
          std::uint64_t size) noexcept {
  for (std::uint64_t offset = 0; offset < size;) {
    if (!healthy(ctx))
      return false;
    auto n = std::min<std::uint64_t>(65536, size - offset);
    std::memcpy(out + offset, in + offset, n);
    offset += n;
  }
  return healthy(ctx);
}
bool grow(Invocation &ctx, BufferState &s, std::uint64_t minimum, bool exact = false,
          bool protectPins = false) noexcept {
  // This resource mutex excludes creation of new I/O snapshots. The source
  // checker separately proved exclusive mutation permission. A live payload
  // lease here can only be an earlier I/O pin; refcounts alone never grant
  // source mutation permission.
  const bool pinned = protectPins && s.payload && s.payload.referenceCount() > 1;
  if (minimum <= s.capacity && !pinned)
    return true;
  if (minimum > std::uint64_t(INT64_MAX)) {
    ctx.fault(2);
    return false;
  }
  std::uint64_t capacity = std::max(minimum, s.capacity);
  if (!exact && minimum > s.capacity) {
    const auto doubled =
        s.capacity > std::uint64_t(INT64_MAX) / 2 ? std::uint64_t(INT64_MAX) : s.capacity * 2;
    capacity = std::max(minimum, std::max<std::uint64_t>(16, doubled));
  }
  auto *data = static_cast<unsigned char *>(ctx.allocate(capacity));
  if (!data)
    return false;
  auto lease = ctx.lease(data);
  if (!lease) {
    ctx.fault(3);
    return false;
  }
  if (!copy(ctx, data, s.data, s.length)) {
    ctx.release(data);
    return false;
  }
  // The resource owns its payload independently of the allocating frame. The
  // allocating context keeps its normal conservative budget reservation.
  ctx.release(data);
  s.payload = std::move(*lease);
  s.data = data;
  s.capacity = capacity;
  ++s.growths;
  s.copiedBytes += s.length;
  s.additionalBytes.store(s.payload.reservationBytes(), std::memory_order_release);
  return true;
}
BufferBytes view(const BufferState &s) noexcept {
  BufferBytes result;
  result.allocated = result.aligned = s.data;
  result.sizes[0] = std::int64_t(s.length);
  result.strides[0] = 1;
  return result;
}
bool pin(Invocation &ctx, BufferState &s, BufferIO &out) noexcept {
  auto owner = ctx.lease(&s);
  if (!owner) {
    ctx.fault(3);
    return false;
  }
  out.owner = std::move(*owner);
  if (s.payload) {
    auto payload = s.payload.retain();
    if (!payload) {
      ctx.fault(3);
      return false;
    }
    out.payload = std::move(*payload);
  }
  out.state = &s;
  return true;
}
} // namespace
bool bufferReadView(Invocation &ctx, const BufferDescriptor &desc, std::uint64_t maximum,
                    bool append, BufferIO &out) noexcept {
  auto *s = inspect(ctx, desc);
  if (!s)
    return false;
  std::lock_guard lock(s->mutex);
  const auto offset = append ? s->length : 0;
  if (maximum > std::uint64_t(INT64_MAX) - offset) {
    ctx.fault(2);
    return false;
  }
  if (!grow(ctx, *s, offset + maximum, false, true) || !pin(ctx, *s, out))
    return false;
  out.offset = offset;
  out.size = maximum;
  out.data = s->data ? s->data + offset : nullptr;
  return true;
}
bool bufferWriteView(Invocation &ctx, const BufferDescriptor &desc, BufferIO &out) noexcept {
  auto *s = inspect(ctx, desc);
  if (!s)
    return false;
  std::lock_guard lock(s->mutex);
  if (!pin(ctx, *s, out))
    return false;
  out.size = s->length;
  out.data = s->data;
  return true;
}
bool bufferReadCommit(Invocation &ctx, BufferIO &io, std::uint64_t count) noexcept {
  if (!healthy(ctx) || !io.state)
    return false;
  auto &s = *static_cast<BufferState *>(io.state);
  std::lock_guard lock(s.mutex);
  if (s.retired.load(std::memory_order_acquire) || count > io.size ||
      io.offset + count > s.capacity) {
    ctx.fault(3);
    return false;
  }
  s.length = io.offset + count;
  return true;
}
BufferStats bufferStats(Invocation &ctx, const BufferDescriptor &value) noexcept {
  auto *s = inspect(ctx, value);
  if (!s)
    return {};
  std::lock_guard lock(s->mutex);
  return {s->capacity, s->length, s->growths, s->copiedBytes, s->data};
}
} // namespace kalandra::runtime
using namespace kalandra;
using namespace kalandra::runtime;
extern "C" void kalandra_buffer_v1(Invocation *ctx, BufferResult *out, std::uint32_t raw,
                                   const BufferArguments *args) noexcept {
  *out = {};
  if (!healthy(*ctx))
    return;
  if (!args || raw > unsigned(BufferAction::WriteByte)) {
    ctx->fault(3);
    return;
  }
  const auto action = BufferAction(raw);
  if (action == BufferAction::Create) {
    if (args->value < 0) {
      out->kind = unsigned(BufferKind::InvalidSize);
      return;
    }
    auto *memory =
        ctx->allocateResource(sizeof(BufferState), OwnedState::cleanup, OwnedState::cost);
    if (!memory)
      return;
    auto *s = new (memory) BufferState;
    if (!grow(*ctx, *s, std::uint64_t(args->value), true))
      return;
    if (auto *scope = currentScope(*ctx))
      adopt(*ctx, *s, scope->scope);
    out->buffer = ownedDescriptor(s);
    return;
  }
  auto *s = inspect(*ctx, args->buffer);
  if (!s)
    return;
  std::lock_guard lock(s->mutex);
  switch (action) {
  case BufferAction::Create:
    break;
  case BufferAction::Length:
    out->count = std::int64_t(s->length);
    break;
  case BufferAction::Capacity:
    out->count = std::int64_t(s->capacity);
    break;
  case BufferAction::CheckIndex:
  case BufferAction::ReadByte:
  case BufferAction::WriteByte:
    if (args->value < 0 || std::uint64_t(args->value) >= s->length) {
      ctx->fault(5, args->line, args->column);
      return;
    }
    if (action == BufferAction::CheckIndex)
      out->count = args->value;
    else if (action == BufferAction::ReadByte)
      out->count = s->data[args->value];
    else {
      if (!grow(*ctx, *s, s->capacity, false, true))
        return;
      s->data[args->value] = args->byte;
    }
    break;
  case BufferAction::Reserve:
    if (args->value < 0)
      out->kind = unsigned(BufferKind::InvalidSize);
    else
      (void)grow(*ctx, *s, std::uint64_t(args->value));
    break;
  case BufferAction::Append: {
    kalandra_array_validate_v1(ctx, &args->bytes, unsigned(NumericType::UInt8));
    if (!healthy(*ctx))
      return;
    const auto size = std::uint64_t(args->bytes.sizes[0]);
    if (!size)
      break;
    if (size > std::uint64_t(INT64_MAX) - s->length) {
      out->kind = unsigned(BufferKind::InvalidSize);
      break;
    }
    if (!grow(*ctx, *s, s->length + size, false, true))
      return;
    // Source arrays cannot alias a live ByteBuffer: toArray copies, intoArray
    // consumes its resource. Respect strided array descriptors nevertheless.
    if (args->bytes.strides[0] == 1) {
      if (!copy(*ctx, s->data + s->length, args->bytes.aligned + args->bytes.offset, size))
        return;
    } else
      for (std::uint64_t i = 0; i < size; ++i) {
        if (i % 65536 == 0 && !healthy(*ctx))
          return;
        s->data[s->length + i] =
            args->bytes.aligned[args->bytes.offset + std::int64_t(i) * args->bytes.strides[0]];
      }
    s->length += size;
    break;
  }
  case BufferAction::Truncate:
    if (args->value < 0 || std::uint64_t(args->value) > s->length)
      out->kind = unsigned(BufferKind::OutOfRange);
    else
      s->length = std::uint64_t(args->value);
    break;
  case BufferAction::Clear:
    s->length = 0;
    break;
  case BufferAction::ToArray: {
    auto *data = static_cast<unsigned char *>(ctx->allocate(s->length));
    if (!data || !copy(*ctx, data, s->data, s->length))
      return;
    out->bytes.allocated = out->bytes.aligned = data;
    out->bytes.sizes[0] = std::int64_t(s->length);
    out->bytes.strides[0] = 1;
    break;
  }
  case BufferAction::IntoArray:
    if (s->payload && s->payload.referenceCount() == 1) {
      if (!ctx->retainLease(s->payload))
        return;
      out->bytes = view(*s);
    } else {
      auto *data = static_cast<unsigned char *>(ctx->allocate(s->length));
      if (!data)
        return;
      if (!copy(*ctx, data, s->data, s->length))
        return;
      out->bytes.allocated = out->bytes.aligned = data;
      out->bytes.sizes[0] = std::int64_t(s->length);
      out->bytes.strides[0] = 1;
    }
    BufferState::retireBuffer(s);
    s->retired.store(true, std::memory_order_release);
    break;
  }
}
