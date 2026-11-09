#pragma once
#include "String.h"
#include "../include/kalandra/FormatABI.h"
extern "C" void kalandra_format_scalar_v1(kalandra::runtime::Invocation *,
                                           kalandra::runtime::StringDescriptor *,
                                           std::uint32_t kind, std::uint64_t bits) noexcept;
// Pieces are an invocation-owned descriptor table. Output may alias one piece.
extern "C" void kalandra_format_join_v1(kalandra::runtime::Invocation *,
                                        kalandra::runtime::StringDescriptor *,
                                        const kalandra::runtime::StringDescriptor *,
                                        std::uint64_t count) noexcept;
