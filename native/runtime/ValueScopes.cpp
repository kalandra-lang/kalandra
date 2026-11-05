#include "ValueScopes.h"
#include "Tensor.h"
#include <algorithm>
#include <cstring>
#include <limits>
namespace kalandra::runtime {
using Scope = Invocation::OwnershipScope;
// Eager, initialized-prefix collection. Growth copies relocatable compiler
// payloads; element backing is retained once into a separate scope. The live
// storage is O(capacity), not a chain of old array concatenations.
struct ValueScopes::Collection {
  ValueScopes &parent;
  std::size_t slot;
  const ValueLayout &element, &array;
  bool recordStorage;
  Scope dataScope, elements;
  std::byte *data = nullptr;
  std::uint64_t count = 0, capacity = 0;
  Collection(ValueScopes &parent, std::size_t slot, const ValueLayout &element,
             const ValueLayout &array, bool recordStorage)
      : parent(parent), slot(slot), element(element), array(array), recordStorage(recordStorage),
        dataScope(parent.context_), elements(parent.context_) {}
  static void cleanup(void *value) noexcept {
    auto *self = static_cast<Collection *>(value);
    self->parent.collections_[self->slot] = nullptr;
    self->~Collection();
  }
  bool append(const void *value) noexcept {
    auto &ctx = parent.context_;
    if (ctx.poll() || !ctx.usable())
      return false;
    const auto maximum = std::uint64_t(INT64_MAX) / element.bytes;
    if (count == maximum) {
      ctx.fault(2);
      return false;
    }
    if (count == capacity) {
      const auto grown = capacity ? (capacity > maximum / 2 ? maximum : capacity * 2)
                                  : std::min<std::uint64_t>(16, maximum);
      Scope next(ctx);
      if (!next.valid())
        return false;
      std::byte *storage;
      {
        Invocation::OwnershipSelection selection(next);
        storage = static_cast<std::byte *>(recordStorage ? ctx.allocateRecord(grown * element.bytes)
                                                         : ctx.allocate(grown * element.bytes));
      }
      if (!storage)
        return false;
      for (std::uint64_t offset = 0; offset < count * element.bytes;) {
        if (ctx.poll())
          return false;
        const auto bytes = std::min<std::uint64_t>(4096, count * element.bytes - offset);
        std::memcpy(storage + offset, data + offset, bytes);
        offset += bytes;
      }
      if (!dataScope.swap(next))
        return false;
      data = storage;
      capacity = grown;
    }
    auto *cell = data + count * element.bytes;
    for (std::size_t offset = 0; offset < element.bytes;) {
      if (ctx.poll())
        return false;
      const auto bytes = std::min<std::size_t>(4096, element.bytes - offset);
      std::memset(cell + offset, 0, bytes);
      offset += bytes;
    }
    Invocation::OwnershipSelection selection(elements);
    if (!element.copy(ctx, cell, ctx, value) || ctx.poll()) {
      if (!ctx.poll())
        ctx.fault(3);
      return false;
    }
    ++count;
    return true;
  }
};
ValueScopes::ValueScopes(Invocation &context, std::span<const ValueLayout *const> layouts) noexcept
    : context_(context), layouts_(layouts), work_(context), spare_(context) {}
ValueScopes *ValueScopes::create(Invocation &context,
                                 std::span<const ValueLayout *const> layouts) noexcept {
  if (context.poll() || !context.usable())
    return nullptr;
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  std::size_t scratch = 0;
  if (layouts.size() > maxSlots) {
    context.fault(2);
    return nullptr;
  }
  for (auto *layout : layouts) {
    if (!layout)
      continue;
    if (!layout->copy || !layout->destroy || !layout->bytes || !layout->alignment ||
        layout->alignment > alignof(std::max_align_t) ||
        (layout->alignment & (layout->alignment - 1))) {
      context.fault(3);
      return nullptr;
    }
    scratch = std::max(scratch, layout->bytes);
  }
  constexpr auto slotsOffset = (sizeof(ValueScopes) + alignof(Scope) - 1) & ~(alignof(Scope) - 1);
  if (layouts.size() > (maximum - slotsOffset - alignof(std::max_align_t)) /
                           (sizeof(Scope) + sizeof(Collection *))) {
    context.fault(2);
    return nullptr;
  }
  const auto scratchOffset =
      (slotsOffset + layouts.size() * (sizeof(Scope) + sizeof(Collection *)) + alignof(std::max_align_t) - 1) &
      ~(alignof(std::max_align_t) - 1);
  if (scratch > maximum - scratchOffset) {
    context.fault(2);
    return nullptr;
  }
  auto *storage = static_cast<std::byte *>(
      context.allocateControl(scratchOffset + scratch, [](void *value) noexcept {
        static_cast<ValueScopes *>(value)->cleanup();
      }));
  if (!storage)
    return nullptr;
  auto *arena = new (storage) ValueScopes(context, layouts);
  arena->slots_ = reinterpret_cast<Scope *>(storage + slotsOffset);
  arena->collections_ =
      reinterpret_cast<Collection **>(storage + slotsOffset + layouts.size() * sizeof(Scope));
  std::fill_n(arena->collections_, layouts.size(), nullptr);
  arena->scratch_ = storage + scratchOffset;
  arena->scratchBytes_ = scratch;
  std::size_t constructed = 0;
  for (; constructed < layouts.size(); ++constructed)
    new (arena->slots_ + constructed) Scope(context);
  bool good = arena->work_.valid() && arena->spare_.valid();
  for (std::size_t i = 0; i < constructed; ++i)
    good &= arena->slots_[i].valid();
  if (!good || context.poll()) {
    arena->destroy();
    return nullptr;
  }
  return arena;
}
bool ValueScopes::begin() noexcept {
  if (selection_) {
    context_.fault(3);
    return false;
  }
  if (context_.poll() || !context_.usable())
    return false;
  work_.reset();
  selection_.emplace(work_);
  return true;
}
void ValueScopes::end() noexcept {
  work_.reset();
  selection_.reset();
}
bool ValueScopes::append(std::size_t slot, const ValueLayout &layout, void *value) noexcept {
  if (!selection_ || slot >= layouts_.size() || !layouts_[slot] || !value || !layout.copy ||
      !layout.destroy || !layout.bytes || !layout.alignment ||
      layout.alignment > alignof(std::max_align_t) || (layout.alignment & (layout.alignment - 1)) ||
      layout.bytes > scratchBytes_) {
    context_.fault(3);
    return false;
  }
  if (context_.poll() || !context_.usable())
    return false;
  bool copied = false;
  {
    Invocation::OwnershipSelection target(slots_[slot]);
    for (std::size_t offset = 0; offset < layout.bytes;) {
      if (context_.poll())
        return false;
      const auto count = std::min<std::size_t>(4096, layout.bytes - offset);
      std::memset(scratch_ + offset, 0, count);
      offset += count;
    }
    copied = layout.copy(context_, scratch_, context_, value) && !context_.poll();
  }
  if (copied) {
    for (std::size_t offset = 0; offset < layout.bytes;) {
      if (context_.poll())
        return false;
      const auto count = std::min<std::size_t>(4096, layout.bytes - offset);
      std::memcpy(static_cast<std::byte *>(value) + offset, scratch_ + offset, count);
      offset += count;
    }
  } else if (!context_.poll())
    context_.fault(3);
  return copied;
}
bool ValueScopes::collectBegin(std::size_t slot, const ValueLayout &element,
                               const ValueLayout &array, bool recordStorage) noexcept {
  if (!selection_ || slot >= layouts_.size() || !layouts_[slot] || collections_[slot] ||
      !array.copy || array.bytes != 40 || !element.bytes || !element.copy || !element.destroy ||
      !element.alignment || element.bytes > std::uint64_t(INT64_MAX) ||
      element.alignment > alignof(std::max_align_t) ||
      (element.alignment & (element.alignment - 1))) {
    context_.fault(3);
    return false;
  }
  auto *storage = context_.allocateControl(sizeof(Collection), Collection::cleanup);
  if (!storage)
    return false;
  auto *collection = new (storage) Collection(*this, slot, element, array, recordStorage);
  collections_[slot] = collection;
  return collection->dataScope.valid() && collection->elements.valid() && !context_.poll();
}
bool ValueScopes::collectAppend(std::size_t slot, const void *value) noexcept {
  if (!selection_ || slot >= layouts_.size() || !collections_[slot] || !value) {
    context_.fault(3);
    return false;
  }
  return collections_[slot]->append(value);
}
bool ValueScopes::collectFinish(std::size_t slot, void *output) noexcept {
  if (!selection_ || slot >= layouts_.size() || !collections_[slot] || !output) {
    context_.fault(3);
    return false;
  }
  auto *collection = collections_[slot];
  // Every supported array uses the same rank-one native descriptor ABI.
  struct Descriptor {
    void *allocated, *aligned;
    std::int64_t offset, size, stride;
  };
  Descriptor descriptor{collection->data, collection->data, 0,
                        static_cast<std::int64_t>(collection->count), collection->count ? 1 : 0};
  static_assert(sizeof(Descriptor) == 40);
  const bool ok =
      collection->array.copy(context_, output, context_, &descriptor) && !context_.poll();
  if (!ok && !context_.poll())
    context_.fault(3);
  context_.releaseControl(collection);
  return ok;
}
bool ValueScopes::collectDiscard(std::size_t slot) noexcept {
  if (slot >= layouts_.size() || !collections_[slot]) {
    context_.fault(3);
    return false;
  }
  context_.releaseControl(collections_[slot]);
  return true;
}
bool ValueScopes::discard(std::size_t slot) noexcept {
  if (!selection_ || slot >= layouts_.size()) {
    context_.fault(3);
    end();
    return false;
  }
  slots_[slot].reset();
  end();
  return !context_.poll();
}
bool ValueScopes::commit(std::size_t slot, void *value) noexcept {
  if (!selection_ || slot >= layouts_.size() || !value) {
    context_.fault(3);
    end();
    return false;
  }
  auto *layout = layouts_[slot];
  if (!layout) {
    const bool ok = !context_.poll();
    end();
    return ok;
  }
  bool copied = false;
  {
    Invocation::OwnershipSelection target(spare_);
    std::memset(scratch_, 0, layout->bytes);
    copied = layout->copy(context_, scratch_, context_, value) && !context_.poll();
  }
  if (copied) {
    slots_[slot].reset();
    work_.reset();
    copied = slots_[slot].swap(spare_);
    if (copied)
      std::memcpy(value, scratch_, layout->bytes);
  }
  if (!copied) {
    spare_.reset();
    if (!context_.poll())
      context_.fault(3);
  }
  end();
  return copied;
}
bool ValueScopes::commitBatch(const Write *writes, std::uint64_t count) noexcept {
  const auto reject = [&](std::int64_t code) {
    context_.fault(code);
    end();
    return false;
  };
  if (count > maxSlots)
    return reject(2);
  if (!selection_ || count > layouts_.size() || (!writes && count))
    return reject(3);
  if (!count) {
    const bool ok = !context_.poll();
    end();
    return ok;
  }
  if (count == 1)
    return commit(writes[0].slot, writes[0].value);
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  constexpr auto align = alignof(std::max_align_t);
  std::size_t scratchBytes = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (context_.poll()) {
      end();
      return false;
    }
    if (writes[i].slot >= layouts_.size() || !writes[i].value)
      return reject(3);
    if (auto *layout = layouts_[writes[i].slot]) {
      if (layout->bytes > maximum - (align - 1))
        return reject(2);
      const auto stride = (layout->bytes + align - 1) & ~(align - 1);
      if (stride > maximum - scratchBytes)
        return reject(2);
      scratchBytes += stride;
    }
  }
  struct Temporary {
    std::size_t constructed = 0;
    Scope *scopes = nullptr;
  };
  // maxSlots bounds fixed metadata independently of payload sizes.
  constexpr auto scopeOffset = (sizeof(Temporary) + alignof(Scope) - 1) & ~(alignof(Scope) - 1);
  const auto bitmapOffset = scopeOffset + count * sizeof(Scope);
  const auto bitmapBytes = (layouts_.size() + 7) / 8;
  const auto scratchOffset = (bitmapOffset + bitmapBytes + align - 1) & ~(align - 1);
  if (scratchBytes > maximum - scratchOffset)
    return reject(2);
  auto *storage = static_cast<std::byte *>(
      context_.allocateControl(scratchOffset + scratchBytes, [](void *pointer) noexcept {
        auto *temporary = static_cast<Temporary *>(pointer);
        for (std::size_t i = 0; i < temporary->constructed; ++i)
          temporary->scopes[i].~Scope();
        temporary->~Temporary();
      }));
  if (!storage) {
    end();
    return false;
  }
  auto *temporary = new (storage) Temporary;
  temporary->scopes = reinterpret_cast<Scope *>(storage + scopeOffset);
  auto *bitmap = storage + bitmapOffset;
  auto *scratch = storage + scratchOffset;
  std::memset(bitmap, 0, bitmapBytes);
  const auto finish = [&](bool ok) {
    context_.releaseControl(temporary);
    end();
    return ok;
  };
  std::size_t offset = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (context_.poll())
      return finish(false);
    const auto slot = writes[i].slot;
    const auto mask = std::byte(1u << (slot % 8));
    if ((bitmap[slot / 8] & mask) != std::byte{}) {
      context_.fault(3);
      return finish(false);
    }
    bitmap[slot / 8] |= mask;
    auto *scope = new (temporary->scopes + i) Scope(context_);
    ++temporary->constructed;
    if (!scope->valid())
      return finish(false);
    if (auto *layout = layouts_[slot]) {
      bool copied = true;
      {
        Invocation::OwnershipSelection target(*scope);
        for (std::size_t byte = 0; byte < layout->bytes;) {
          if (context_.poll()) {
            copied = false;
            break;
          }
          const auto bytes = std::min<std::size_t>(4096, layout->bytes - byte);
          std::memset(scratch + offset + byte, 0, bytes);
          byte += bytes;
        }
        if (copied)
          copied = layout->copy(context_, scratch + offset, context_, writes[i].value);
      }
      // Restore the work selection before destroying temporary scopes.
      if (!copied || context_.poll()) {
        if (!context_.poll())
          context_.fault(3);
        return finish(false);
      }
      offset += (layout->bytes + align - 1) & ~(align - 1);
    }
  }
  // All outputs have complete graphs; no old slot/work owner has retired yet.
  // A stopped publication aborts the task. Cleanup ignores partial frame bytes.
  offset = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto slot = writes[i].slot;
    if (!slots_[slot].swap(temporary->scopes[i]))
      return finish(false);
    if (auto *layout = layouts_[slot]) {
      for (std::size_t byte = 0; byte < layout->bytes;) {
        if (context_.poll())
          return finish(false);
        const auto bytes = std::min<std::size_t>(4096, layout->bytes - byte);
        std::memcpy(static_cast<std::byte *>(writes[i].value) + byte, scratch + offset + byte,
                    bytes);
        byte += bytes;
      }
      offset += (layout->bytes + align - 1) & ~(align - 1);
    }
  }
  return finish(true);
}
bool ValueScopes::publish(std::size_t slot, void *output, const void *input) noexcept {
  end(); // Restore the caller's storage scope before acquiring return ownership.
  if (slot >= layouts_.size() || !layouts_[slot] || !output || !input) {
    context_.fault(3);
    return false;
  }
  const bool copied = layouts_[slot]->copy(context_, output, context_, input) && !context_.poll();
  if (!copied && !context_.poll())
    context_.fault(3);
  return copied;
}
void ValueScopes::cleanup() noexcept {
  end();
  for (std::size_t i = 0; i < layouts_.size(); ++i)
    slots_[i].~Scope();
  this->~ValueScopes();
}
void ValueScopes::destroy() noexcept { context_.releaseControl(this); }
} // namespace kalandra::runtime

extern "C" {
using namespace kalandra::runtime;
ValueScopes *kalandra_value_scopes_create_v1(Invocation *ctx, const ValueLayout *const *layouts,
                                             std::uint64_t count) noexcept {
  if (!ctx)
    return nullptr;
  if (count > ValueScopes::maxSlots) {
    ctx->fault(2);
    return nullptr;
  }
  if (!layouts && count) {
    ctx->fault(3);
    return nullptr;
  }
  return ValueScopes::create(*ctx, {layouts, static_cast<std::size_t>(count)});
}
bool kalandra_value_scopes_begin_v1(ValueScopes *scopes) noexcept {
  return scopes && scopes->begin();
}
bool kalandra_value_scopes_commit_v1(ValueScopes *scopes, std::uint64_t slot,
                                     void *value) noexcept {
  return scopes && scopes->commit(slot, value);
}
bool kalandra_value_scopes_publish_v1(ValueScopes *scopes, std::uint64_t slot, void *output,
                                      const void *input) noexcept {
  return scopes && scopes->publish(slot, output, input);
}
bool kalandra_value_scopes_batch_v1(ValueScopes *scopes, const ValueScopes::Write *writes,
                                    std::uint64_t count) noexcept {
  return scopes && scopes->commitBatch(writes, count);
}
bool kalandra_value_scopes_append_v1(ValueScopes *scopes, std::uint64_t slot,
                                     const ValueLayout *layout, void *value) noexcept {
  return scopes && layout && scopes->append(slot, *layout, value);
}
bool kalandra_collect_begin_v1(ValueScopes *scopes, std::uint64_t slot, const ValueLayout *element,
                               const ValueLayout *array, bool recordStorage) noexcept {
  return scopes && element && array && scopes->collectBegin(slot, *element, *array, recordStorage);
}
bool kalandra_collect_append_v1(ValueScopes *scopes, std::uint64_t slot,
                                const void *value) noexcept {
  return scopes && scopes->collectAppend(slot, value);
}
bool kalandra_collect_finish_v1(ValueScopes *scopes, std::uint64_t slot, void *output) noexcept {
  return scopes && scopes->collectFinish(slot, output);
}
bool kalandra_collect_discard_v1(ValueScopes *scopes, std::uint64_t slot) noexcept {
  return scopes && scopes->collectDiscard(slot);
}
void kalandra_value_scopes_end_v1(ValueScopes *scopes) noexcept {
  if (scopes)
    scopes->end();
}
void kalandra_value_scopes_destroy_v1(ValueScopes *scopes) noexcept {
  if (scopes)
    scopes->destroy();
}
}
