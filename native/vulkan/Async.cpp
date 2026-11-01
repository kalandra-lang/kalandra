#include "Async.h"
#include "../runtime/NumericStorage.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
namespace kalandra::runtime::vulkan {
#include "../gpu-runtime/AsyncImpl.inc"
} // namespace kalandra::runtime::vulkan
