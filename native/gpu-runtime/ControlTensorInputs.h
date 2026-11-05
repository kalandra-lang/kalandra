#pragma once
#include "../runtime/DeviceArrayABI.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace kalandra::runtime::detail {
struct ControlTensorExportABI {
  static constexpr std::size_t Root = DeviceArrayExportABI::Root,
                               Data = DeviceArrayExportABI::Data,
                               Length = DeviceArrayExportABI::Length,
                               Extents = Length + 1, MaxRank = 8, Cells = Extents + MaxRank;
};
// Roles zero through three retain their existing meanings. A resident tensor
// role carries its fixed rank in the signature and reserves rank+1 sealed
// control words for element count and runtime extents.
struct ControlTensorInputs {
  static constexpr std::uint8_t FirstRole = 4, LastRole = 12;
  static constexpr std::optional<unsigned> rank(std::uint8_t role) {
    if (role < FirstRole || role > LastRole)
      return {};
    return role - FirstRole;
  }
  static constexpr std::uint8_t FirstResultRole = LastRole + 1, LastResultRole = FirstResultRole + 8;
  static constexpr std::optional<unsigned> resultRank(std::uint8_t role) {
    if (role < FirstResultRole || role > LastResultRole)
      return {};
    return role - FirstResultRole;
  }
  static constexpr bool validTerminal(std::uint8_t role) {
    return role == 0 || role == DeviceArrayWireABI::ExportRole || resultRank(role).has_value();
  }
  static constexpr char encode(std::uint8_t role) {
    if (auto n = rank(role))
      return static_cast<char>('0' + *n);
    if (auto n = resultRank(role))
      return static_cast<char>('A' + *n);
    switch (role) {
    case 0: return 't';
    case 1: return 's';
    case DeviceArrayWireABI::InputRole: return 'a';
    case DeviceArrayWireABI::ExportRole: return 'x';
    default: return '\0';
    }
  }
  static constexpr std::optional<std::uint8_t> decode(char role) {
    if (role >= '0' && role <= '8')
      return FirstRole + (role - '0');
    if (role >= 'A' && role <= 'I')
      return FirstResultRole + (role - 'A');
    switch (role) {
    case 't' : return 0;
    case 's': return 1;
    case 'a': return DeviceArrayWireABI::InputRole;
    case 'x': return DeviceArrayWireABI::ExportRole;
    default: return {};
    }
  }
  static constexpr std::size_t words(std::uint8_t role) {
    if (auto n = rank(role))
      return 1 + *n;
    return role == DeviceArrayWireABI::InputRole ? 1 : 0; // Existing array wire length.
  }
};

template <class Dimensions, class Bytes, class CellBytes, class Read, class Write>
bool sealControlInputs(std::span<const std::uint8_t> roles, std::size_t slots,
                              std::size_t prefix, std::uint64_t maxElements,
                              bool initialize, Dimensions dimensions, Bytes bytes,
                              CellBytes cellBytes, Read read, Write write) {
  if (roles.empty() || !maxElements || prefix > slots || !ControlTensorInputs::validTerminal(roles.back()))
    return false;
  std::size_t footer = 0;
  const auto inputs = roles.first(roles.size() - 1);
  for (auto role : inputs) {
    if (role != 1 && role != DeviceArrayWireABI::InputRole && !ControlTensorInputs::rank(role))
      return false;
    const auto words = ControlTensorInputs::words(role);
    if (words > slots - prefix - footer)
      return false;
    footer += words;
  }
  // Verify all metadata before publishing any footer, including when several
  // tensors and arrays share one command.
  const auto walk = [&](auto visit) {
    auto cell = slots - footer;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto rank = ControlTensorInputs::rank(inputs[i]);
      if (!rank) {
        if (inputs[i] != DeviceArrayWireABI::InputRole)
          continue;
        const auto size = bytes(i);
        if (size % 8 || size / 8 < DeviceArrayWireABI::HeaderWords ||
            size / 8 > DeviceArrayWireABI::MaxWords || !visit(cell++, size / 8))
          return false;
        continue;
      }
      const auto shape = dimensions(i);
      const auto width = cellBytes(i);
      if (shape.size() != *rank || !width)
        return false;
      bool empty = false;
      for (auto dim : shape) {
        if (std::cmp_less(dim, 0) || static_cast<std::uint64_t>(dim) > maxElements)
          return false;
        empty |= dim == 0;
      }
      std::uint64_t elements = empty ? 0 : 1;
      for (auto dim : shape) {
        const auto extent = static_cast<std::uint64_t>(dim);
        if (extent && elements > maxElements / extent)
          return false;
        elements *= extent;
      }
      const auto size = bytes(i);
      if (size % width || size / width != elements || !visit(cell++, elements))
        return false;
      for (auto dim : shape)
        if (!visit(cell++, static_cast<std::uint64_t>(dim)))
          return false;
    }
    return true;
  };
  if (!walk([&](std::size_t cell, std::uint64_t value) { return initialize || read(cell) == value; }))
    return false;
  return !initialize || walk([&](std::size_t cell, std::uint64_t value) {
    write(cell, value);
    return true;
  });
}
} // namespace kalandra::runtime::detail
