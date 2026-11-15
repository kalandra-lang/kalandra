#pragma once
#include "../include/kalandra/FormatABI.h"
#include "String.h"
extern "C" void kalandra_format_scalar_v1(kalandra::runtime::Invocation *,
                                          kalandra::runtime::StringDescriptor *, std::uint32_t kind,
                                          std::uint64_t bits) noexcept;
extern "C" void kalandra_format_number_v1(kalandra::runtime::Invocation *,
                                          kalandra::runtime::StringDescriptor *, std::uint32_t kind,
                                          std::uint64_t bits, std::uint32_t presentation,
                                          std::uint64_t precision) noexcept;
extern "C" void kalandra_format_align_v1(kalandra::runtime::Invocation *,
                                         kalandra::runtime::StringDescriptor *,
                                         const kalandra::runtime::StringDescriptor *,
                                         std::uint64_t width, std::uint32_t alignment) noexcept;
// Pieces are an invocation-owned descriptor table. Output may alias one piece.
extern "C" void kalandra_format_join_v1(kalandra::runtime::Invocation *,
                                        kalandra::runtime::StringDescriptor *,
                                        const kalandra::runtime::StringDescriptor *,
                                        std::uint64_t count) noexcept;
