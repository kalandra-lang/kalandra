#pragma once
#include "../include/kalandra/ParseABI.h"
#include "String.h"
extern "C" void kalandra_parse_scalar_v1(kalandra::runtime::Invocation *,
                                         kalandra::runtime::ParseScalarResult *, std::uint32_t,
                                         const kalandra::runtime::StringDescriptor *) noexcept;
