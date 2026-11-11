#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
namespace kalandra {
enum class BufferAction : std::uint32_t {
  Create,
  Length,
  Capacity,
  Reserve,
  Append,
  Truncate,
  Clear,
  ToArray,
  IntoArray,
  CheckIndex,
  ReadByte,
  WriteByte
};
inline std::optional<BufferAction> bufferAction(std::string_view name) {
  constexpr std::string_view names[] = {"byteBuffer", "length", "capacity", "reserve",  "append",
                                        "truncate",   "clear",  "toArray",  "intoArray"};
  for (std::uint32_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
    if (name == names[i])
      return static_cast<BufferAction>(i);
  return {};
}
inline bool mutatesBuffer(BufferAction action) {
  return action == BufferAction::Reserve || action == BufferAction::Append ||
         action == BufferAction::Truncate || action == BufferAction::Clear ||
         action == BufferAction::IntoArray || action == BufferAction::WriteByte;
}
inline bool fallibleBuffer(BufferAction action) {
  return action == BufferAction::Create || action == BufferAction::Reserve ||
         action == BufferAction::Append || action == BufferAction::Truncate;
}
} // namespace kalandra
