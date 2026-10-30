#include "../runtime/Runtime.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
struct Descriptor {
  void *allocated, *aligned;
  std::int64_t offset;
  std::array<std::int64_t, 1> sizes, strides;
};
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    Invocation source, destination;
    auto *buffer = source.allocateRecord(96);
    require(buffer, "source allocation failed");
    std::memset(buffer, 0xa5, 96);
    Descriptor valid{buffer, buffer, 0, {3}, {1}};
    for (unsigned mutation = 0; mutation < 12; ++mutation) {
      auto value = valid;
      std::uint64_t bytes = 32, alignment = 8;
      switch (mutation) {
      case 0:
        value.offset = 1;
        break;
      case 1:
        value.aligned = nullptr;
        break;
      case 2:
        value.sizes[0] = -1;
        break;
      case 3:
        value.sizes[0] = 4;
        break;
      case 4:
        value.strides[0] = 2;
        break;
      case 5:
        value.sizes[0] = INT64_MAX;
        break;
      case 6:
        bytes = 0;
        break;
      case 7:
        alignment = 3;
        break;
      case 8:
        alignment = 128;
        break;
      case 9:
        bytes = 31;
        break;
      case 10:
        value.allocated = reinterpret_cast<void *>(std::uintptr_t(0x1234));
        value.aligned = value.allocated;
        break;
      case 11:
        value.sizes[0] = 0;
        break;
      }
      Invocation rejected;
      require(!kalandra_record_array_retain_v1(&rejected, &source, &value, bytes, alignment) &&
                  rejected.status() == 3 && !rejected.live(),
              "malformed record array retained");
    }
    for (unsigned n = 0; n < 2; ++n)
      require(kalandra_record_array_retain_v1(&destination, &source, &valid, 32, 8),
              "array retention failed");
    source.settle();
    auto lease = destination.lease(buffer);
    require(lease && lease->bytes().size() == 96 && lease->bytes()[95] == std::byte{0xa5},
            "array backing lost after source settlement");
    destination.releaseRecords();
    require(!destination.live() && destination.allocations() == destination.releases(),
            "array owners leaked");
    require(lease->bytes()[0] == std::byte{0xa5}, "retained array lease lost");
    Invocation emptySource, emptyDestination;
    Descriptor empty{nullptr, nullptr, 0, {0}, {0}};
    require(kalandra_record_array_retain_v1(&emptyDestination, &emptySource, &empty, 32, 8) &&
                !emptyDestination.allocations(),
            "empty array allocated");
    emptySource.settle();
    require(!kalandra_record_array_retain_v1(&emptyDestination, &emptySource, &empty, 32, 8),
            "settled empty context accepted");
    Invocation allocationFailure;
    allocationFailure.failAllocationAt(1);
    require(!allocationFailure.allocateRecord(32) && allocationFailure.status() == 2 &&
                !allocationFailure.live(),
            "record array allocation injection failed");
    std::cout << "record array runtime: descriptor validation, empty arrays, retained storage and "
                 "cleanup passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
