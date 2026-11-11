#pragma once
#include "String.h"

namespace kalandra::runtime {
enum class EnvironmentResult : std::uint32_t { Missing, Value, InvalidName, InvalidEncoding };
}

extern "C" bool kalandra_environment_initialize_v1(kalandra::runtime::Invocation *) noexcept;
extern "C" std::uint32_t
kalandra_environment_get_v1(kalandra::runtime::Invocation *, kalandra::runtime::StringDescriptor *,
                            const kalandra::runtime::StringDescriptor *) noexcept;
