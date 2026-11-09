#pragma once
#include "String.h"
#include "../include/kalandra/FormatABI.h"
extern "C" void kalandra_format_scalar_v1(kalandra::runtime::Invocation *,
                                           kalandra::runtime::StringDescriptor *,
                                           std::uint32_t kind, std::uint64_t bits) noexcept;
