#pragma once
#include "ControlArena.h"
#include "ControlTensorExport.h"
namespace kalandra::runtime::detail {
// These cells exist only for owned-array-result kernels. Source slots follow
// this prefix; scalar-result frame layouts remain unchanged. The device reports
// a checked descriptor, not a native address. Host work only transfers bytes.
using ControlArrayExportABI = DeviceArrayExportABI;
struct ControlArrayExportRange {
  DeviceArrayResultMetadata metadata;
  std::uint64_t offsetWords;
};
template <class Read>
std::optional<ControlArrayExportRange> controlArrayExportRange(Read read, std::uint64_t arenaWords,
                                                               std::size_t captures) {
  using A = ControlArrayExportABI;
  const auto root = read(A::Root), offset = read(A::Data);
  DeviceArrayResultMetadata value{read(A::Layout), read(A::Length), read(A::Origin)};
  if (!value.layout || value.layout > 11 ||
      value.length > DeviceArrayWireABI::MaxWords - DeviceArrayWireABI::HeaderWords ||
      value.origin > captures || arenaWords > ControlArenaABI::MaxWords)
    return {};
  if (value.origin) {
    if (offset || root <= DeviceArrayWireABI::ExternalHandle ||
        root - DeviceArrayWireABI::ExternalHandle > captures)
      return {};
  } else if (!root) {
    if (offset || value.length)
      return {};
  } else if (root >= DeviceArrayWireABI::ExternalHandle || arenaWords < 4 ||
             root > arenaWords - 4 || offset != root + 4 || value.length > arenaWords - offset)
    return {};
  return ControlArrayExportRange{value, offset};
}
// Final device computation is already complete. This transports a checked
// arena byte range into a separate result owner without interpreting source
// objects. Retain both allocations until the actual fence acknowledges each
// bounded page, exactly as for arena growth. A partial destination is private.
template <class Buffer, class Error> class ControlExport final {
public:
  static constexpr std::uint64_t CopyBytes = 64 * 1024;
  struct Page {
    Buffer source, target;
    std::uint64_t sourceOffset, targetOffset, bytes;
  };
  ControlExport() = default;
  ControlExport(const ControlExport &) = delete;
  ControlExport &operator=(const ControlExport &) = delete;
  ControlExport(ControlExport &&other) noexcept { *this = std::move(other); }
  ControlExport &operator=(ControlExport &&other) noexcept {
    if (this != &other) {
      source_ = std::move(other.source_);
      target_ = std::move(other.target_);
      offset_ = std::exchange(other.offset_, 0);
      words_ = std::exchange(other.words_, 0);
      copied_ = std::exchange(other.copied_, 0);
      pending_ = std::exchange(other.pending_, 0);
      started_ = std::exchange(other.started_, false);
    }
    return *this;
  }
  std::expected<void, Error> start(Buffer source, std::uint64_t offsetWords, std::uint64_t words) {
    if (started_ || !source || source.bytes() % 8 || !words ||
        words > DeviceArrayWireABI::MaxWords || words > SIZE_MAX / 8 ||
        offsetWords > source.bytes() / 8 || words > source.bytes() / 8 - offsetWords)
      return std::unexpected(Error::Invalid);
    source_ = std::move(source);
    offset_ = offsetWords * 8;
    words_ = words;
    started_ = true;
    return {};
  }
  bool active() const noexcept { return started_ && bool(source_); }
  bool complete() const noexcept { return started_ && !source_ && bool(target_) && !pending_; }
  std::uint64_t words() const noexcept { return words_; }
  std::optional<Buffer> result() const noexcept {
    if (!complete())
      return {};
    return target_;
  }
  template <class Allocate> std::expected<Page, Error> beginPage(Allocate &&allocate) {
    if (!active() || pending_)
      return std::unexpected(Error::Invalid);
    if (!target_) {
      auto result = allocate(words_ * 8);
      if (!result)
        return std::unexpected(result.error());
      if (!*result || result->bytes() != words_ * 8)
        return std::unexpected(Error::Invalid);
      target_ = std::move(*result);
    }
    pending_ = std::min(CopyBytes, words_ * 8 - copied_);
    return Page{source_, target_, offset_ + copied_, copied_, pending_};
  }
  std::expected<bool, Error> acknowledge() {
    if (!active() || !pending_)
      return std::unexpected(Error::Invalid);
    copied_ += pending_;
    pending_ = 0;
    if (copied_ != words_ * 8)
      return false;
    source_ = {};
    return true;
  }
  template <class Allocate, class Copy>
  std::expected<bool, Error> advance(Allocate &&allocate, Copy &&copy) {
    auto page = beginPage(std::forward<Allocate>(allocate));
    if (!page)
      return std::unexpected(page.error());
    auto status =
        copy(page->source, page->target, page->sourceOffset, page->targetOffset, page->bytes);
    if (!status)
      return std::unexpected(status.error());
    return acknowledge();
  }

private:
  Buffer source_, target_;
  std::uint64_t offset_ = 0, words_ = 0, copied_ = 0, pending_ = 0;
  bool started_ = false;
};
} // namespace kalandra::runtime::detail
