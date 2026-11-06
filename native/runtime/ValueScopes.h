#pragma once
#include "Tasks.h"
namespace kalandra::runtime {
// Private compiler-only storage: payloads are trivially relocatable, unlike
// arbitrary C++ ValueLayouts. Layout tables outlive the frame.
class ValueScopes final {
public:
  static constexpr std::size_t maxSlots = 20000; // Existing per-plan continuation bound.
  static ValueScopes *create(Invocation &, std::span<const ValueLayout *const>) noexcept;
  void destroy() noexcept;
  bool begin() noexcept;
  bool commit(std::size_t, void *) noexcept;
  // A stopped/failed result has no valid success payload to copy. Retire its
  // previous rooted value without interpreting zeroed enum/pointer fields.
  bool discard(std::size_t) noexcept;
  // Retire a slot without ending or starting an ownership interval.
  bool clear(std::size_t) noexcept;
  struct Write {
    std::uint64_t slot;
    void *value;
  };
  // Retain every result before retiring input storage shared by the results.
  bool commitBatch(const Write *, std::uint64_t) noexcept;
  // Keep a fully normalized element graph in a partially built array's scope.
  // The caller stores the rewritten payload, not the original graph's bytes.
  bool append(std::size_t, const ValueLayout &, void *) noexcept;
  bool collectBegin(std::size_t, const ValueLayout &, const ValueLayout &,
                    bool recordStorage) noexcept;
  bool collectAppend(std::size_t, const void *) noexcept;
  bool collectFinish(std::size_t, void *) noexcept;
  bool collectDiscard(std::size_t) noexcept;
  void end() noexcept;
  bool publish(std::size_t, void *, const void *) noexcept;

private:
  ValueScopes(Invocation &, std::span<const ValueLayout *const>) noexcept;
  ~ValueScopes() = default;
  void cleanup() noexcept;
  struct Collection;
  Collection **collections_ = nullptr;
  Invocation &context_;
  std::span<const ValueLayout *const> layouts_;
  Invocation::OwnershipScope work_, spare_;
  Invocation::OwnershipScope *slots_ = nullptr;
  std::byte *scratch_ = nullptr;
  std::size_t scratchBytes_ = 0;
  std::optional<Invocation::OwnershipSelection> selection_;
};
} // namespace kalandra::runtime

// Versioned private compiler bridge. Null destruction supports partial frames.
extern "C" {
kalandra::runtime::ValueScopes *
kalandra_value_scopes_create_v1(kalandra::runtime::Invocation *,
                                const kalandra::runtime::ValueLayout *const *,
                                std::uint64_t) noexcept;
bool kalandra_value_scopes_begin_v1(kalandra::runtime::ValueScopes *) noexcept;
bool kalandra_value_scopes_commit_v1(kalandra::runtime::ValueScopes *, std::uint64_t,
                                     void *) noexcept;
bool kalandra_value_scopes_publish_v1(kalandra::runtime::ValueScopes *, std::uint64_t, void *,
                                      const void *) noexcept;
void kalandra_value_scopes_end_v1(kalandra::runtime::ValueScopes *) noexcept;
bool kalandra_value_scopes_batch_v1(kalandra::runtime::ValueScopes *,
                                    const kalandra::runtime::ValueScopes::Write *,
                                    std::uint64_t) noexcept;
bool kalandra_value_scopes_append_v1(kalandra::runtime::ValueScopes *, std::uint64_t,
                                     const kalandra::runtime::ValueLayout *, void *) noexcept;
bool kalandra_collect_begin_v1(kalandra::runtime::ValueScopes *, std::uint64_t,
                               const kalandra::runtime::ValueLayout *,
                               const kalandra::runtime::ValueLayout *, bool) noexcept;
bool kalandra_collect_append_v1(kalandra::runtime::ValueScopes *, std::uint64_t,
                                const void *) noexcept;
bool kalandra_collect_finish_v1(kalandra::runtime::ValueScopes *, std::uint64_t, void *) noexcept;
bool kalandra_collect_discard_v1(kalandra::runtime::ValueScopes *, std::uint64_t) noexcept;
void kalandra_value_scopes_destroy_v1(kalandra::runtime::ValueScopes *) noexcept;
}
static_assert(sizeof(kalandra::runtime::ValueScopes::Write) == 16 &&
              alignof(kalandra::runtime::ValueScopes::Write) == 8);
