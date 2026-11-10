#pragma once
#include "StringArray.h"
namespace kalandra::runtime {
// Called once before entering generated source. argv is supplied by the host
// CRT and remains alive throughout main. UTF validity is checked before source
// starts, then the intrinsic creates invocation-owned String backing on demand.
#ifdef _WIN32
bool initializeApplicationArguments(int argc, wchar_t **argv) noexcept;
#else
bool initializeApplicationArguments(int argc, char **argv) noexcept;
#endif
} // namespace kalandra::runtime
extern "C" void
kalandra_application_arguments_v1(kalandra::runtime::Invocation *,
                                  kalandra::runtime::StringArrayDescriptor *) noexcept;

extern "C" bool kalandra_application_initialize_v1(std::int32_t argc, void *argv) noexcept;
