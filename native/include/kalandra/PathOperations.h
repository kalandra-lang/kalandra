#pragma once
#include <cstdint>
namespace kalandra {
// Private v1 path bridge. The public module selects these through checked declarations.
enum class PathAction : std::uint32_t { Join = 1, FileName, ParentPath, Extension, IsAbsolute };
} // namespace kalandra
