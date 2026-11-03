#include "ValueScopes.h"
#include <algorithm>
#include <cstring>
#include <limits>
namespace kalandra::runtime {
using Scope = Invocation::OwnershipScope;
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
  if (layouts.size() > (maximum - slotsOffset - alignof(std::max_align_t)) / sizeof(Scope)) {
    context.fault(2);
    return nullptr;
  }
  const auto scratchOffset =
      (slotsOffset + layouts.size() * sizeof(Scope) + alignof(std::max_align_t) - 1) &
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
  arena->scratch_ = storage + scratchOffset;
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
void kalandra_value_scopes_end_v1(ValueScopes *scopes) noexcept {
  if (scopes)
    scopes->end();
}
void kalandra_value_scopes_destroy_v1(ValueScopes *scopes) noexcept {
  if (scopes)
    scopes->destroy();
}
}
