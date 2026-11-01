#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace kalandra::runtime::vulkan {
// Data only: compiler tools can use these resources without a Vulkan loader.
// Names are stable relative paths; unknown names return an empty span.
std::span<const std::uint32_t> builtinShader(std::string_view name) noexcept;
std::span<const std::string_view> builtinShaderNames() noexcept;
} // namespace kalandra::runtime::vulkan
