#pragma once
#include "ControlTensorInputs.h"
#include <algorithm>
#include <span>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <utility>

namespace kalandra::runtime::detail {
struct ControlArenaABI {
  static constexpr std::uint64_t MaxWords = UINT32_MAX, InitialWords = 32;
  static constexpr std::uint64_t Done = UINT64_MAX, Grow = UINT64_MAX - 1;
  static constexpr std::size_t PC = 0, Capacity = 1, Requested = 2, Resume = 3, Cells = 4;
};
// Capture role 2 is a read-only array wire, not a scalar broadcast or a map
// domain. Its actual backing length is sealed in the final frame cells by the
// runtime, never trusted from the wire's own bytes. Verify it after each fence.
inline std::size_t controlInputPrefix(std::uint8_t terminal) {
  if (ControlTensorInputs::resultRank(terminal))
    return ControlTensorExportABI::Cells;
  return terminal == DeviceArrayWireABI::ExportRole ? DeviceArrayExportABI::Cells : ControlArenaABI::Cells;
}
inline bool validControlInputRoles(std::span<const std::uint8_t> roles, std::size_t slots) {
  if (roles.empty() || !ControlTensorInputs::validTerminal(roles.back()))
    return false;
  const auto prefix = controlInputPrefix(roles.back());
  if (slots < prefix)
    return false;
  auto remaining = slots - prefix;
  for (auto role : roles.first(roles.size() - 1)) {
    if (role != 1 && role != DeviceArrayWireABI::InputRole && !ControlTensorInputs::rank(role))
      return false;
    const auto words = ControlTensorInputs::words(role);
    if (words > remaining)
      return false;
    remaining -= words;
  }
  return true;
}
template <class Dimensions, class Bytes, class CellBytes, class Read, class Write>
bool controlInputMetadata(std::span<const std::uint8_t> roles, std::size_t slots, bool initialize,
                          Dimensions dimensions, Bytes bytes, CellBytes cellBytes,
                          Read read, Write write) {
  const auto prefix = roles.empty() ? 0 : controlInputPrefix(roles.back());
  return sealControlInputs(roles, slots, prefix, ControlArenaABI::MaxWords, initialize,
                           dimensions, bytes, cellBytes, read, write);
}
template <class Bytes, class Read, class Write>
bool controlInputLengths(std::span<const std::uint8_t> roles, std::size_t slots, bool initialize,
                         Bytes bytes, Read read, Write write) {
  return controlInputMetadata(roles, slots, initialize,
      [](std::size_t) { return std::span<const std::int64_t>{}; }, bytes,
      [](std::size_t) { return 0u; }, read, write);
}
// Private invocation storage. Callbacks allocate charged device storage and
// perform fence-complete opaque GPU copies; this class never evaluates source
// operations or traverses source objects. One advance copies at most one page,
// allowing the completion service to check cancellation between growth steps.
template <class Buffer, class Error> class ControlArena final {
public:
  static constexpr std::uint64_t WordBytes = 8;
  static constexpr std::uint64_t CopyBytes = 64 * 1024;
  static constexpr std::uint64_t MaxWords = ControlArenaABI::MaxWords;
  struct Page {
    Buffer source, target;
    std::uint64_t offset = 0, bytes = 0;
    bool clear = false;
  };
  ControlArena() = default;
  explicit ControlArena(bool allocationIsBlank) : allocationIsBlank_(allocationIsBlank) {}
  ControlArena(const ControlArena &) = delete;
  ControlArena &operator=(const ControlArena &) = delete;
  ControlArena(ControlArena &&other) noexcept { *this = std::move(other); }
  ControlArena &operator=(ControlArena &&other) noexcept {
    if (this != &other) {
      current_ = std::move(other.current_);
      replacement_ = std::move(other.replacement_);
      requested_ = std::exchange(other.requested_, 0);
      copied_ = std::exchange(other.copied_, 0);
      cleared_ = std::exchange(other.cleared_, 0);
      pendingBytes_ = std::exchange(other.pendingBytes_, 0);
      pendingClear_ = std::exchange(other.pendingClear_, false);
      allocationIsBlank_ = other.allocationIsBlank_;
    }
    return *this;
  }
  const Buffer &buffer() const { return current_; }
  std::uint64_t words() const { return current_ ? current_.bytes() / WordBytes : 0; }
  bool growing() const { return requested_ != 0; }
  std::expected<void, Error> request(std::uint64_t words) {
    if (growing() || words <= this->words() || words > MaxWords ||
        words > std::numeric_limits<std::size_t>::max() / WordBytes)
      return std::unexpected(Error::Invalid);
    requested_ = words;
    return {};
  }
  // An asynchronous backend retains the returned page through the real fence,
  // then calls acknowledge(). It never blocks a TaskCore worker waiting for GPU
  // copies. No page may be requested again until the outstanding one settles.
  template <class Allocate>
  std::expected<std::optional<Page>, Error> beginPage(Allocate &&allocate) {
    if (!growing() || pendingBytes_)
      return std::unexpected(Error::Invalid);
    if (!replacement_) {
      auto next = allocate(requested_ * WordBytes);
      if (!next)
        return std::unexpected(next.error());
      if (!*next || next->bytes() != requested_ * WordBytes)
        return std::unexpected(Error::Invalid);
      replacement_ = std::move(*next);
      cleared_ = allocationIsBlank_ ? requested_ * WordBytes : 0;
    }
    if (cleared_ < replacement_.bytes()) {
      pendingBytes_ = std::min(CopyBytes, replacement_.bytes() - cleared_);
      pendingClear_ = true;
      return std::optional<Page>(Page{{}, replacement_, cleared_, pendingBytes_, true});
    }
    const auto oldBytes = current_ ? current_.bytes() : 0;
    if (copied_ < oldBytes) {
      pendingBytes_ = std::min(CopyBytes, oldBytes - copied_);
      pendingClear_ = false;
      return std::optional<Page>(Page{current_, replacement_, copied_, pendingBytes_, false});
    }
    commit();
    return std::optional<Page>{};
  }
  std::expected<bool, Error> acknowledge() {
    if (!pendingBytes_ || !growing())
      return std::unexpected(Error::Invalid);
    (pendingClear_ ? cleared_ : copied_) += pendingBytes_;
    pendingBytes_ = 0;
    if (cleared_ != replacement_.bytes() || copied_ != (current_ ? current_.bytes() : 0))
      return false;
    commit();
    return true;
  }
  template <class Allocate, class Copy>
  std::expected<bool, Error> advance(Allocate &&allocate, Copy &&copy) {
    auto page = beginPage(std::forward<Allocate>(allocate));
    if (!page)
      return std::unexpected(page.error());
    if (!*page)
      return true;
    if ((**page).clear)
      return std::unexpected(Error::Invalid); // Use the asynchronous clear-page protocol.
    const auto &p = **page;
    auto result = copy(p.source, p.target, p.offset, p.bytes);
    if (!result)
      return std::unexpected(result.error());
    return acknowledge();
  }

private:
  void commit() {
    current_ = std::move(replacement_);
    replacement_ = {};
    requested_ = copied_ = cleared_ = pendingBytes_ = 0;
    pendingClear_ = false;
  }
  Buffer current_, replacement_;
  std::uint64_t requested_ = 0, copied_ = 0, cleared_ = 0, pendingBytes_ = 0;
  bool allocationIsBlank_ = true, pendingClear_ = false;
};
} // namespace kalandra::runtime::detail
