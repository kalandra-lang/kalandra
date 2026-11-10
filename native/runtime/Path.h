#pragma once
#include "../include/kalandra/PathOperations.h"
#include "StringArray.h"
namespace kalandra::runtime {
enum class PathStyle { Posix, Windows };
// Private lexical engine. Exposing the style here permits both grammars to be
// qualified on every host; source programs use their executable target's style.
std::uint64_t pathOperation(Invocation &, StringDescriptor &, PathAction, const void *,
                            PathStyle) noexcept;
} // namespace kalandra::runtime
extern "C" std::uint64_t kalandra_path_v1(kalandra::runtime::Invocation *,
                                          kalandra::runtime::StringDescriptor *, std::uint32_t,
                                          const void *) noexcept;
