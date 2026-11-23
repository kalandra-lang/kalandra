#pragma once
#include "OwnedResources.h"

namespace kalandra::runtime {
extern "C" void *kalandra_subscription_prepare_v1(Invocation *, void (*)(std::uint64_t)) noexcept;
extern "C" bool kalandra_subscription_publish_v1(Invocation *, void *, std::uint64_t,
                                                 OwnedDescriptor *) noexcept;
extern "C" bool kalandra_subscription_close_v1(Invocation *, const OwnedDescriptor *) noexcept;
} // namespace kalandra::runtime
