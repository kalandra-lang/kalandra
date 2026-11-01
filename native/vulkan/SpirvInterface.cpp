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
  std::map<std::uint32_t, std::uint32_t> bindings, sets, storage, floats;
  std::map<std::uint32_t, std::uint32_t> variableTypes, pointers, members, integers, constants;
  std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> arrays;
  std::set<std::uint32_t> used;
  std::uint32_t entry = 0, localEntry = 0;
  bool declaredDenorm = false, declaredPreserve = false;
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
      case 4464: // DenormPreserve; only admitted for explicitly checked float64.
        if (declaredDenorm)
          return std::unexpected("duplicate denormal capability");
        declaredDenorm = true;
        break;
      case 4466: // SignedZeroInfNanPreserve; execution width checked below.
        if (declaredPreserve)
          return std::unexpected("duplicate preservation capability");
        declaredPreserve = true;
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
      if (!end || end == name)
        return std::unexpected("invalid entry name");
      result.entry.assign(name, end);
      result.flattenedGrid = result.entry.ends_with("_vk_flat_v1");
      result.groupOnlyGrid = result.entry.ends_with("_vk_groups_v1");
      entry = args[1];
      break;
    }
    case 16: // Fixed grid and explicitly admitted preservation modes only; never RTE.
      if (args.size() == 3 && (args[1] == 4461 || args[1] == 4459)) {
        if (!id(args[0]) || args[0] != entry || (args[2] != 32 && args[2] != 64) ||
            (args[1] == 4459 && args[2] != 64))
          return std::unexpected("unsupported floating-point execution mode");
        auto &mode = args[1] == 4459 ? result.denormFloat64
                     : args[2] == 32 ? result.preserveFloat32
                                     : result.preserveFloat64;
        if (mode)
          return std::unexpected("duplicate floating-point execution mode");
        mode = true;
        break;
      }
      if (args.size() != 5 || args[1] != 17 || !id(args[0]) || localEntry || !args[2] ||
          args[3] != 1 || args[4] != 1)
        return std::unexpected("unsupported execution mode");
      localEntry = args[0];
      result.localSize = args[2];
      break;
    case 21: // OpTypeInt
      if (args.size() != 3 || !id(args[0]))
        return std::unexpected("invalid integer type");
      integers[args[0]] = args[1];
      break;
    case 22: // OpTypeFloat
      if (args.size() != 2 || !id(args[0]) || (args[1] != 32 && args[1] != 64))
        return std::unexpected("invalid floating type");
      floats[args[0]] = args[1];
      break;
    case 136: // OpFDiv; scalar f64 division has a separate device admission rule.
      if (args.size() != 4 || !id(args[0]) || !id(args[1]) || !id(args[2]) || !id(args[3]) ||
          !floats.contains(args[0]))
        return std::unexpected("invalid scalar floating division");
      result.float64Division |= floats.at(args[0]) == 64;
      break;
    case 28: // OpTypeArray
      if (args.size() != 3 || !id(args[0]) || !id(args[1]) || !id(args[2]))
        return std::unexpected("invalid array type");
      arrays[args[0]] = {args[1], args[2]};
      break;
    case 30: // OpTypeStruct; only single-member blocks form the parameter ABI.
      if (args.empty() || !id(args[0]))
        return std::unexpected("invalid struct type");
      if (args.size() == 2)
        members[args[0]] = args[1];
      break;
    case 32: // OpTypePointer
      if (args.size() != 3 || !id(args[0]) || !id(args[2]))
        return std::unexpected("invalid pointer type");
      pointers[args[0]] = args[2];
      break;
    case 43: // OpConstant: array lengths in the compiler ABI are uint32 words.
      if (args.size() >= 3 && integers.contains(args[0]) && integers.at(args[0]) == 32)
        constants[args[1]] = args[2];
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
      variableTypes[args[1]] = args[0];
      if (args[2] == 9)
        return std::unexpected("push constants are outside the compiler buffer ABI");
      break;
    default:
      break;
    }
    offset += count;
  }
  if (declaredDenorm != result.denormFloat64 ||
      declaredPreserve != (result.preserveFloat32 || result.preserveFloat64))
    return std::unexpected("floating control capability/mode mismatch");
  if ((result.preserveFloat64 || result.denormFloat64) && !result.float64)
    return std::unexpected("float64 preservation without Float64 capability");
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
  for (auto [variable, binding] : bindings) {
    if (binding != result.buffers - 1)
      continue;
    const auto pointer = variableTypes.at(variable);
    if (!pointers.contains(pointer) || !members.contains(pointers.at(pointer)))
      return std::unexpected("invalid parameter buffer block");
    const auto array = members.at(pointers.at(pointer));
    if (!arrays.contains(array))
      return std::unexpected("parameter buffer must have fixed size");
    auto [element, length] = arrays.at(array);
    if (!integers.contains(element) || integers.at(element) != 32 || !constants.contains(length) ||
        !constants.at(length) || constants.at(length) > 262144)
      return std::unexpected("invalid parameter word layout");
    result.parameterWords = constants.at(length);
  }
  return result;
}
std::expected<std::vector<std::uint32_t>, std::string>
preserveNativeFloat64(std::span<const std::uint32_t> words, bool denormals) {
  auto interface = reflectSpirv(words);
  if (!interface || !interface->float64)
    return std::unexpected("native float64 controls require a reflected Float64 module");
  const bool addSign = !interface->preserveFloat64;
  const bool addDenorm = denormals && !interface->denormFloat64;
  if (!addSign && !addDenorm)
    return std::vector<std::uint32_t>(words.begin(), words.end());
  std::size_t capabilitiesEnd = 5;
  while (capabilitiesEnd < words.size() && (words[capabilitiesEnd] & 65535) == 17)
    capabilitiesEnd += words[capabilitiesEnd] >> 16;
  if (capabilitiesEnd == 5)
    return std::unexpected("native float64 module has no capability section");
  auto extensionsEnd = capabilitiesEnd;
  bool extension = false, signCapability = false;
  std::size_t modesEnd = 0;
  std::uint32_t entry = 0;
  constexpr char name[] = "SPV_KHR_float_controls";
  for (std::size_t offset = 5; offset < words.size();) {
    const auto count = words[offset] >> 16, opcode = words[offset] & 65535;
    if (opcode == 17 && words[offset + 1] == 4466)
      signCapability = true;
    if (opcode == 10) {
      if (count > 1 && (count - 1) * 4 >= sizeof(name) &&
          !std::memcmp(words.data() + offset + 1, name, sizeof(name)))
        extension = true;
      if (offset == extensionsEnd)
        extensionsEnd += count;
    }
    if (opcode == 15)
      entry = words[offset + 2];
    if (opcode == 16)
      modesEnd = offset + count;
    offset += count;
  }
  if (!entry || !modesEnd)
    return std::unexpected("native float64 module has no execution section");
  std::vector<std::uint32_t> result(words.begin(), words.begin() + 5);
  result.reserve(words.size() + 20);
  const auto insert = [&](std::size_t offset) {
    if (offset == capabilitiesEnd) {
      if (addSign && !signCapability)
        result.insert(result.end(), {(2u << 16) | 17, 4466});
      if (addDenorm)
        result.insert(result.end(), {(2u << 16) | 17, 4464});
    }
    if (offset == extensionsEnd && !extension) {
      constexpr std::uint32_t length = (sizeof(name) + 3) / 4;
      result.push_back(((length + 1) << 16) | 10);
      const auto begin = result.size();
      result.resize(begin + length, 0);
      std::memcpy(result.data() + begin, name, sizeof(name));
    }
    if (offset == modesEnd) {
      if (addSign)
        result.insert(result.end(), {(4u << 16) | 16, entry, 4461, 64});
      if (addDenorm)
        result.insert(result.end(), {(4u << 16) | 16, entry, 4459, 64});
    }
  };
  for (std::size_t offset = 5; offset < words.size();) {
    insert(offset);
    const auto count = words[offset] >> 16;
    result.insert(result.end(), words.begin() + offset, words.begin() + offset + count);
    offset += count;
  }
  insert(words.size());
  auto checked = reflectSpirv(result);
  if (!checked || checked->entry != interface->entry || checked->buffers != interface->buffers ||
      checked->parameterWords != interface->parameterWords || !checked->preserveFloat64 ||
      (denormals && !checked->denormFloat64))
    return std::unexpected("native float64 controls changed the reflected ABI");
  return result;
}
} // namespace kalandra::runtime::vulkan
