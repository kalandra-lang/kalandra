#pragma once
#include "DeviceGraphWire.h"
#include <algorithm>
#include <expected>

namespace kalandra::runtime {
struct DeviceGraphArenaImage {
  // Cell zero is the null handle. Each allocation starts with the device
  // heap's span, layout, payload length and collector mark.
  static constexpr std::size_t BlockHeader = 4;
  std::vector<std::uint64_t> words;
  std::uint64_t root;
};

// Prepare a private arena image. A caller can upload it without exposing any
// host addresses or evaluating source operations. Validation must complete
// before the image can be bound to a device invocation.
template <class Cancelled>
std::expected<DeviceGraphArenaImage, DeviceGraphValidation>
makeDeviceGraphArena(std::span<const std::uint64_t> wire,
                     std::span<const DeviceGraphLayout> layouts,
                     std::span<const unsigned> arenaLayouts, std::size_t rootLayout,
                     std::size_t wordLimit, std::size_t work, Cancelled cancelled) {
  using A = DeviceGraphWireABI;
  using V = DeviceGraphValidation;
  constexpr auto header = DeviceGraphArenaImage::BlockHeader;
  if (layouts.size() != arenaLayouts.size())
    return std::unexpected(V::Invalid);
  const auto checked = validateDeviceGraph(wire, layouts, rootLayout, work, cancelled);
  if (checked != V::Valid)
    return std::unexpected(checked);

  wordLimit = std::min<std::size_t>(wordLimit, DeviceArrayWireABI::MaxWords);
  if (!wordLimit)
    return std::unexpected(V::Budget);
  const auto count = static_cast<std::size_t>(wire[1]);
  std::vector<std::uint64_t> handles;
  handles.reserve(count);
  std::size_t size = 1;
  for (std::size_t i = 0; i < count; ++i) {
    if (cancelled())
      return std::unexpected(V::Cancelled);
    const auto directory = A::HeaderWords + A::DirectoryWords * i;
    const auto layout = wire[directory] - 1;
    const auto payload = wire[directory + 1] * layouts[layout].cells.size();
    if (!arenaLayouts[layout])
      return std::unexpected(V::Invalid);
    if (wordLimit - size < header || payload > wordLimit - size - header)
      return std::unexpected(V::Budget);
    handles.push_back(size);
    size += header + static_cast<std::size_t>(payload);
  }

  DeviceGraphArenaImage image{{}, handles[wire[2] - 1]};
  image.words.reserve(size);
  image.words.push_back(0);
  std::size_t source = A::HeaderWords + A::DirectoryWords * count;
  for (std::size_t i = 0; i < count; ++i) {
    if (cancelled())
      return std::unexpected(V::Cancelled);
    const auto directory = A::HeaderWords + A::DirectoryWords * i;
    const auto layout = wire[directory] - 1;
    const auto elements = wire[directory + 1];
    const auto cells = layouts[layout].cells;
    const auto payload = elements * cells.size();
    image.words.insert(image.words.end(), {header + payload, arenaLayouts[layout], payload, DeviceHeapBlockABI::ReadOnly | 1});
    for (std::uint64_t element = 0; element < elements; ++element) {
      for (const auto &cell : cells) {
        if (cancelled())
          return std::unexpected(V::Cancelled);
        const auto value = wire[source++];
        // Validation established that inactive references are zero, and every
        // active nonzero reference names a typed, reachable directory entry.
        image.words.push_back(cell.target && value ? handles[value - 1] : value);
      }
    }
  }
  return image;
}
} // namespace kalandra::runtime
