#include "../include/kalandra/NumericType.h"
#include "../runtime/Runtime.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Descriptor {
  void *allocated, *aligned;
  std::int64_t offset;
  std::array<std::int64_t, 2> sizes, strides;
};
int main() {
  try {
    for (auto element :
         {NumericType::Int8, NumericType::UInt8, NumericType::Int16, NumericType::UInt16,
          NumericType::Int32, NumericType::UInt32, NumericType::Int64, NumericType::UInt64,
          NumericType::Float32, NumericType::Float64}) {
      const auto id = static_cast<unsigned>(element);
      Invocation source, destination;
      const auto bytes = describeNumeric(element)->bytes() * 6;
      auto *storage = source.allocateRecord(bytes);
      require(storage, "allocation failed");
      std::memset(storage, 0xa5, bytes);
      Descriptor original{storage, storage, 0, {2, 3}, {3, 1}};
      for (unsigned mutation = 0; mutation < 13; ++mutation) {
        auto bad = original;
        auto rank = std::uint64_t(2), type = std::uint64_t(id);
        switch (mutation) {
        case 0:
          bad.offset = 1;
          break;
        case 1:
          bad.aligned = nullptr;
          break;
        case 2:
          bad.allocated = nullptr;
          break;
        case 3:
          bad.sizes[0] = -1;
          break;
        case 4:
          bad.sizes[1] = 4;
          bad.strides[0] = 4;
          break;
        case 5:
          bad.strides[0] = 2;
          break;
        case 6:
          bad.strides[1] = -1;
          break;
        case 7:
          bad.sizes = {INT64_MAX, INT64_MAX};
          bad.strides = {INT64_MAX, 1};
          break;
        case 8:
          rank = 129;
          break;
        case 9:
          type = 100;
          break;
        case 10:
          type = 10;
          break;
        case 11:
          bad.sizes[0] = 0;
          break;
        case 12:
          bad.allocated = reinterpret_cast<void *>(std::uintptr_t(0x1234));
          bad.aligned = bad.allocated;
          break;
        }
        Invocation rejected;
        require(!kalandra_record_retain_v1(&rejected, &source, &bad, rank, type) &&
                    rejected.status() == 3 && !rejected.live(),
                "forged record descriptor retained");
      }
      for (unsigned n = 0; n < 2; ++n)
        require(kalandra_record_retain_v1(&destination, &source, &original, 2, id),
                "valid retain failed");
      source.settle();
      auto lease = destination.lease(storage);
      require(lease && lease->bytes().size() == bytes && lease->bytes()[0] == std::byte{0xa5},
              "record data did not survive source");
      // Cleanup releases only record storage, including duplicate retained owners.
      auto *ordinary = destination.allocate(16);
      require(ordinary, "ordinary allocation failed");
      destination.releaseRecords();
      require(destination.live() == 1 && !destination.status() &&
                  destination.lease(ordinary).has_value(),
              "record cleanup removed ordinary buffers");
      destination.release(ordinary);
      require(!destination.live() && destination.allocations() == destination.releases(),
              "owners leaked");
      require(lease->bytes()[bytes - 1] == std::byte{0xa5}, "lease lost storage after cleanup");
      Invocation emptySource, emptyDestination;
      Descriptor empty{nullptr, nullptr, 0, {2, 0}, {0, 0}};
      require(kalandra_record_retain_v1(&emptyDestination, &emptySource, &empty, 2, id) &&
                  !emptyDestination.allocations(),
              "empty storage allocated");
      emptySource.settle();
      require(!kalandra_record_retain_v1(&emptyDestination, &emptySource, &empty, 2, id),
              "settled empty source accepted");
      Invocation scalarSource, scalarDestination;
      auto *scalar = scalarSource.allocateRecord(describeNumeric(element)->bytes());
      struct {
        void *allocated, *aligned;
        std::int64_t offset;
      } rankZero{scalar, scalar, 0};
      require(kalandra_record_retain_v1(&scalarDestination, &scalarSource, &rankZero, 0, id),
              "rank zero rejected");
      for (unsigned attempt = 1; attempt <= 2; ++attempt) {
        Invocation failure;
        failure.failAllocationAt(attempt);
        bool failed = false;
        for (unsigned n = 0; n < 2; ++n)
          if (!kalandra_record_retain_v1(&failure, &scalarSource, &rankZero, 0, id)) {
            failed = true;
            break;
          }
        require(failed && failure.status() == 2, "retention failure injection missed");
        failure.settle();
        require(!failure.live() && failure.allocations() == failure.releases(),
                "partial retain leaked");
      }
    }
    std::cout << "record collection storage validation, leases, empty/rank-zero values and failure "
                 "cleanup passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
