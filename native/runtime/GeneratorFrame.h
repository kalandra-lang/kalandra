#pragma once
#include "OwnedResources.h"
#include "Tasks.h"
namespace kalandra::runtime {
enum class GeneratorStep : std::uint32_t { Item, Exhausted, Error, Stopped };
// Trusted compiler descriptors. Entry metadata is copied; layout tables and
// callback code outlive the cursor. A cursor has its own ledger and frame,
// but no TaskCore, scheduler admission record, worker or thread. resume runs only
// to the next item/terminal result; source-level suspension uses a separate
// continuation bridge rather than blocking a scheduler worker in this callback.
// A failing construct cleans partial payload fields itself; its managed backing
// is settled by the frame. A successful resume initializes the selected result.
struct GeneratorEntry {
  std::uint32_t revision;
  std::size_t frameBytes, frameAlignment;
  bool (*construct)(Invocation &, void *, const Packet &);
  void (*destroy)(void *) noexcept;
  GeneratorStep (*resume)(Invocation &, void *, void *);
  const ValueLayout *item;
  const ValueLayout *error; // null for `never`, with no error storage or wrapper
};
using GeneratorDescriptor = OwnedDescriptor;
struct IntegerRangeValue {
  std::int64_t start, stop, step;
};
// The compiler creates capture packets after evaluating source expressions in
// order. Construction only snapshots/retains captures; it never advances a body.
GeneratorDescriptor generatorCreate(Invocation &, const GeneratorEntry &, const Packet &) noexcept;
GeneratorStep generatorNext(Invocation &, const GeneratorDescriptor &, const ValueLayout &item,
                            const ValueLayout *error, void *output, void *failure) noexcept;
bool generatorClose(Invocation &, const GeneratorDescriptor &) noexcept;
// Task-aware erased dispatch reuses synchronous validation. Complete means the
// typed result envelope is initialized; Pending means retry on the same Task.
inline constexpr std::uint32_t generatorDispatchPending = 4, generatorDispatchComplete = 5;
std::uint32_t generatorNextTask(Task &, const GeneratorDescriptor &, const ValueLayout &item,
                                const ValueLayout *error, void *output, void *failure,
                                const ValueLayout &result, void *envelope) noexcept;
bool generatorCloseTask(Task &, const GeneratorDescriptor &) noexcept;
GeneratorDescriptor generatorRange(Invocation &, IntegerRangeValue, const ValueLayout &item,
                                   std::int64_t line, std::int64_t column) noexcept;
// Private compiler bridge. The source layout validates/retains a contiguous
// rank-one descriptor; the checker must keep its backing read-borrowed for the
// cursor lifetime. Contents are not cloned; retaining nested backing follows
// the source layout's validation and ownership rules.
GeneratorDescriptor generatorArray(Invocation &, const ValueLayout &source, const ValueLayout &item,
                                   const void *descriptor) noexcept;
} // namespace kalandra::runtime
extern "C" {
void kalandra_generator_create_v1(kalandra::runtime::Invocation *,
                                  kalandra::runtime::GeneratorDescriptor *,
                                  const kalandra::runtime::GeneratorEntry *,
                                  const kalandra::runtime::Packet *) noexcept;
std::uint32_t kalandra_generator_next_v1(kalandra::runtime::Invocation *,
                                         const kalandra::runtime::GeneratorDescriptor *,
                                         const kalandra::runtime::ValueLayout *,
                                         const kalandra::runtime::ValueLayout *, void *,
                                         void *) noexcept;
bool kalandra_generator_close_v1(kalandra::runtime::Invocation *,
                                 const kalandra::runtime::GeneratorDescriptor *) noexcept;
std::uint32_t kalandra_generator_next_task_v1(kalandra::runtime::Task *,
                                              const kalandra::runtime::GeneratorDescriptor *,
                                              const kalandra::runtime::ValueLayout *,
                                              const kalandra::runtime::ValueLayout *, void *,
                                              void *, const kalandra::runtime::ValueLayout *,
                                              void *) noexcept;
bool kalandra_generator_close_task_v1(kalandra::runtime::Task *,
                                      const kalandra::runtime::GeneratorDescriptor *) noexcept;
void kalandra_iterator_array_v1(kalandra::runtime::Invocation *,
                                kalandra::runtime::GeneratorDescriptor *,
                                const kalandra::runtime::ValueLayout *,
                                const kalandra::runtime::ValueLayout *, const void *) noexcept;
void kalandra_iterator_range_v1(kalandra::runtime::Invocation *,
                                kalandra::runtime::GeneratorDescriptor *,
                                const kalandra::runtime::IntegerRangeValue *,
                                const kalandra::runtime::ValueLayout *, std::int64_t,
                                std::int64_t) noexcept;
}
static_assert(sizeof(kalandra::runtime::GeneratorEntry) == 64 &&
              alignof(kalandra::runtime::GeneratorEntry) == 8 &&
              offsetof(kalandra::runtime::GeneratorEntry, item) == 48 &&
              offsetof(kalandra::runtime::GeneratorEntry, error) == 56);
static_assert(sizeof(kalandra::runtime::IntegerRangeValue) == 24 &&
              alignof(kalandra::runtime::IntegerRangeValue) == 8);
