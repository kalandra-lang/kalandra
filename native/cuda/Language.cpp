#include "Language.h"
#include <algorithm>
#include <mutex>
#include <new>
namespace kalandra::runtime::cuda::language {
namespace raw = ::kalandra::runtime::cuda;
constexpr auto backendSoftwareSuffix = u8".software.ptx";
#include "../gpu-runtime/LanguageImpl.inc"
} // namespace kalandra::runtime::cuda::language
