#pragma once
#include <expected>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace kalandra::runtime::vulkan {
#include "../gpu-runtime/ExecutablePath.inc"
} // namespace kalandra::runtime::vulkan
