#pragma once
#include "GeneratorFrame.h"
namespace kalandra::runtime {
// Private compiler descriptor for a coordinating producer. The body's result is
// the same typed Option/error envelope returned by next. Classification and None
// construction are generated alongside that layout; none of these callbacks
// executes user code. The descriptor and its layout tables outlive the cursor.
struct GeneratorTaskEntry {
  std::uint32_t revision;
  const TaskEntry *body;
  GeneratorStep (*classify)(const void *) noexcept;
  void (*exhausted)(void *) noexcept;
};
// Construction captures but does not run the body. It requires the owning
// application task so failed capture construction has an asynchronous cleanup
// boundary. The compiler retains captures directly in the body's logical store.
GeneratorDescriptor generatorTaskCreate(Task &, const GeneratorTaskEntry &,
                                        const Packet &) noexcept;
bool isTaskGenerator(Invocation &, const GeneratorDescriptor &) noexcept;
// False means the same source continuation must retry after its borrowed call or
// cleanup fence settles. A cursor never allocates a scheduled task of its own.
bool generatorTaskNext(Task &, const GeneratorDescriptor &, const ValueLayout &, void *) noexcept;
bool generatorTaskClose(Task &, const GeneratorDescriptor &) noexcept;
} // namespace kalandra::runtime
extern "C" {
void kalandra_generator_task_create_v1(kalandra::runtime::Task *,
                                       kalandra::runtime::GeneratorDescriptor *,
                                       const kalandra::runtime::GeneratorTaskEntry *,
                                       const kalandra::runtime::Packet *) noexcept;
bool kalandra_generator_task_kind_v1(kalandra::runtime::Invocation *,
                                     const kalandra::runtime::GeneratorDescriptor *) noexcept;
bool kalandra_generator_task_next_v1(kalandra::runtime::Task *,
                                     const kalandra::runtime::GeneratorDescriptor *,
                                     const kalandra::runtime::ValueLayout *, void *) noexcept;
bool kalandra_generator_task_close_v1(kalandra::runtime::Task *,
                                      const kalandra::runtime::GeneratorDescriptor *) noexcept;
}
static_assert(sizeof(kalandra::runtime::GeneratorTaskEntry) == 32 &&
              alignof(kalandra::runtime::GeneratorTaskEntry) == 8 &&
              offsetof(kalandra::runtime::GeneratorTaskEntry, body) == 8 &&
              offsetof(kalandra::runtime::GeneratorTaskEntry, classify) == 16 &&
              offsetof(kalandra::runtime::GeneratorTaskEntry, exhausted) == 24);
