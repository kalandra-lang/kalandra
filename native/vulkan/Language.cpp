#include "Language.h"
#include <algorithm>
#include <mutex>
#include <new>
namespace kalandra::runtime::vulkan::language {
namespace raw = ::kalandra::runtime::vulkan;
constexpr auto backendSoftwareSuffix = u8".software.spv";
#include "../gpu-runtime/LanguageImpl.inc"
} // namespace kalandra::runtime::vulkan::language
