#include "SpirvInterface.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
namespace kalandra::runtime::vulkan {
std::expected<SpirvInterface, std::string> reflectSpirv(std::span<const std::uint32_t> words) {
  if (words.size() < 5 || words[0] != 0x07230203 || words[4] || !words[3] || words[3] > (1u << 24))
    return std::unexpected("invalid SPIR-V header");
  SpirvInterface result;
  std::map<std::uint32_t, std::uint32_t> bindings, sets, storage;
  std::set<std::uint32_t> used;
  std::uint32_t entry = 0, localEntry = 0;
  const auto id = [&](std::uint32_t value) { return value && value < words[3]; };
  for (std::size_t offset = 5; offset < words.size();) {
    auto count = words[offset] >> 16, op = words[offset] & 0xffff;
    if (!count || count > words.size() - offset)
      return std::unexpected("truncated SPIR-V instruction");
    auto args = words.subspan(offset + 1, count - 1);
    switch (op) {
    case 17: // OpCapability
      if (args.size() != 1)
        return std::unexpected("invalid capability");
      switch (args[0]) {
      case 0: // Matrix (implied by Shader in MLIR artifacts)
      case 1:
        break; // Shader
      case 10:
        result.float64 = true;
        break;
      case 11:
        result.int64 = true;
        break;
      case 22:
        result.int16 = true;
        break;
      case 39:
        result.int8 = true;
        break;
      case 4433:
        result.storage16 = true;
        break;
      case 4448:
        result.storage8 = true;
        break;
      default:
        return std::unexpected("unsupported shader capability");
      }
      break;
    case 15: { // OpEntryPoint
      if (args.size() < 3 || args[0] != 5 || !id(args[1]) || entry)
        return std::unexpected("expected one compute entry point");
      const auto *name = reinterpret_cast<const char *>(args.data() + 2);
      auto bytes = (args.size() - 2) * 4;
      const auto *end = static_cast<const char *>(std::memchr(name, 0, bytes));
      if (!end || end == name || end - name > 255)
        return std::unexpected("invalid entry name");
      result.entry.assign(name, end);
      result.flattenedGrid = result.entry.ends_with("_vk_flat_v1");
      entry = args[1];
      break;
    }
    case 16: // OpExecutionMode: fixed one-dimensional local size only.
      if (args.size() != 5 || args[1] != 17 || !id(args[0]) || localEntry || !args[2] ||
          args[3] != 1 || args[4] != 1)
        return std::unexpected("unsupported execution mode");
      localEntry = args[0];
      result.localSize = args[2];
      break;
    case 71: // OpDecorate
      if (args.size() < 2 || !id(args[0]))
        return std::unexpected("invalid decoration");
      if (args[1] == 33 || args[1] == 34) {
        if (args.size() != 3)
          return std::unexpected("invalid descriptor decoration");
        auto &map = args[1] == 33 ? bindings : sets;
        if (!map.emplace(args[0], args[2]).second)
          return std::unexpected("duplicate descriptor decoration");
      }
      break;
    case 59: // OpVariable
      if (args.size() < 3 || !id(args[1]))
        return std::unexpected("invalid variable");
      if (!storage.emplace(args[1], args[2]).second)
        return std::unexpected("duplicate variable");
      if (args[2] == 9)
        return std::unexpected("push constants are outside the compiler buffer ABI");
      break;
    default:
      break;
    }
    offset += count;
  }
  if (!entry || localEntry != entry)
    return std::unexpected("missing fixed compute local size");
  if (bindings.empty() || bindings.size() > 128 || sets.size() != bindings.size())
    return std::unexpected("invalid descriptor set interface");
  for (auto [variable, binding] : bindings) {
    if (!sets.contains(variable) || sets.at(variable) != 0 || !storage.contains(variable) ||
        (storage.at(variable) != 12 && storage.at(variable) != 2) || !used.insert(binding).second)
      return std::unexpected("invalid storage buffer binding");
  }
  result.buffers = static_cast<std::uint32_t>(bindings.size());
  if (*used.rbegin() != result.buffers - 1)
    return std::unexpected("noncontiguous storage buffer bindings");
  return result;
}
} // namespace kalandra::runtime::vulkan
