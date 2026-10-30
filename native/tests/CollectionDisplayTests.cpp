#include "../runtime/CollectionDisplay.h"
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace kalandra;
using namespace kalandra::runtime;
using cli::CollectionDescriptor;
static unsigned cases = 0;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static std::string render(const DisplayCollection &value, std::span<const std::int64_t> shape,
                          NumericType type, bool array, std::uint64_t limit = 100) {
  FILE *stream = std::tmpfile();
  require(stream != nullptr, "cannot open display test stream");
  require(printCollection(stream, value, shape, type, array, limit),
          "collection formatting failed");
  std::rewind(stream);
  std::string text;
  std::array<char, 256> bytes;
  while (auto count = std::fread(bytes.data(), 1, bytes.size(), stream))
    text.append(bytes.data(), count);
  require(!std::ferror(stream), "cannot read display test stream");
  std::fclose(stream);
  return text;
}
template <class T> static void numeric(NumericType type) {
  Invocation context;
  auto *data = static_cast<T *>(context.allocate(8 * sizeof(T)));
  require(data != nullptr, "display allocation failed");
  const std::array<T, 8> source{T(0), T(0), T(0), T(1), T(2), T(3), T(4), T(0)};
  std::memcpy(data, source.data(), sizeof(source));
  CollectionDescriptor descriptor;
  descriptor.allocated = data;
  descriptor.aligned = data + 1;
  descriptor.offset = 2;
  descriptor.dimensions[0] = descriptor.dimensions[1] = 2;
  descriptor.dimensions[2] = 2;
  descriptor.dimensions[3] = 1;
  auto matrix = retainForDisplay(context, descriptor, 2, type, false);
  require(bool(matrix) && matrix->count == 4 && matrix->data.size() == 4 * sizeof(T),
          "valid interior matrix output rejected");
  for (unsigned fault = 0; fault < 12; ++fault) {
    auto invalid = descriptor;
    std::size_t rank = 2;
    auto element = type;
    bool array = false;
    if (fault == 0)
      invalid.allocated = nullptr;
    if (fault == 1)
      invalid.aligned = reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(data) - 1);
    if (fault == 2)
      invalid.aligned = reinterpret_cast<std::byte *>(data) + 1;
    if (fault == 3)
      invalid.offset = -1;
    if (fault == 4)
      invalid.offset = INT64_MAX;
    if (fault == 5)
      invalid.dimensions[0] = -1;
    if (fault == 6)
      invalid.dimensions[0] = INT64_MAX;
    if (fault == 7)
      invalid.dimensions[3] = 0;
    if (fault == 8)
      rank = cli::maximumCollectionRank + 1;
    if (fault == 9)
      element = static_cast<NumericType>(255);
    if (fault == 10)
      array = true;
    if (fault == 11) {
      invalid.dimensions[0] = 8;
      invalid.dimensions[1] = 1;
      invalid.dimensions[2] = 1;
    }
    require(!retainForDisplay(context, invalid, rank, element, array),
            "malformed display storage accepted");
    ++cases;
  }
  descriptor.dimensions[0] = 4;
  descriptor.dimensions[1] = 1;
  auto array = retainForDisplay(context, descriptor, 1, type, true);
  require(bool(array), "array result rejected");
  const auto allocations = context.allocations();
  context.settle();
  require(context.live() == 0 && context.allocations() == context.releases() && allocations == 1,
          "display retained context owners or copied payload");
  require(!retainForDisplay(context, descriptor, 1, type, true), "settled context admitted");
  require(render(*array, {descriptor.dimensions.data(), 1}, type, true) == "[1, 2, 3, 4]\n",
          "array values changed");
  require(render(*array, {descriptor.dimensions.data(), 1}, type, true, 0) ==
              "[...] (4 elements)\n",
          "zero display limit changed");
  const std::array<std::int64_t, 2> shape{2, 2};
  require(render(*matrix, shape, type, false) == "tensor([1, 2, 3, 4], [2, 2])\n",
          "matrix shape/value display changed");
  require(render(*matrix, shape, type, false, 2) == "tensor([1, 2, ...], [2, 2]) (4 elements)\n",
          "bounded matrix display changed");
  require(std::memcmp(array->data.data(), source.data() + 3, 4 * sizeof(T)) == 0,
          "printing changed stored element bits");
  cases += 7;
}
template <class T, class Bits>
static void floating(NumericType type, std::array<Bits, 5> bits, const char *expected) {
  Invocation context;
  auto *data = context.allocate(sizeof(bits));
  require(data != nullptr, "FP display allocation failed");
  std::memcpy(data, bits.data(), sizeof(bits));
  CollectionDescriptor descriptor;
  descriptor.allocated = descriptor.aligned = data;
  descriptor.dimensions[0] = 5;
  descriptor.dimensions[1] = 1;
  auto retained = retainForDisplay(context, descriptor, 1, type, true);
  require(bool(retained), "FP display result rejected");
  context.settle();
  require(render(*retained, {descriptor.dimensions.data(), 1}, type, true) == expected,
          "floating edge formatting changed");
  require(std::memcmp(retained->data.data(), bits.data(), sizeof(bits)) == 0,
          "FP formatting mutated source bits");
  (void)sizeof(T);
  ++cases;
}
template <class T> static void narrow(NumericType type) {
  Invocation context;
  const T low = std::numeric_limits<T>::min(), high = std::numeric_limits<T>::max();
  std::array<T, 6> cells{0, 0, low, 0, high, 0};
  auto *data = static_cast<T *>(context.allocate(sizeof(cells)));
  require(data != nullptr, "narrow display allocation failed");
  std::memcpy(data, cells.data(), sizeof(cells));
  CollectionDescriptor descriptor;
  descriptor.allocated = data;
  descriptor.aligned = data + 1;
  descriptor.offset = 1;
  descriptor.dimensions[0] = 3;
  descriptor.dimensions[1] = 1;
  auto result = retainForDisplay(context, descriptor, 1, type, true);
  require(bool(result) && result->data.size() == 3 * sizeof(T), "narrow interior view rejected");
  auto tensor = retainForDisplay(context, descriptor, 1, type, false);
  require(bool(tensor), "narrow tensor display rejected");
  require(render(*tensor, {descriptor.dimensions.data(), 1}, type, false) ==
              "tensor([" + std::to_string(int(low)) + ", 0, " + std::to_string(int(high)) +
                  "], [3])\n",
          "narrow tensor display lost signs, widths or dimensions");
  for (unsigned mode = 0; mode < 5; ++mode) {
    auto bad = descriptor;
    if (mode == 0)
      bad.offset = -1;
    if (mode == 1)
      bad.offset = INT64_MAX;
    if (mode == 2)
      bad.dimensions[0] = 100;
    if (mode == 3)
      bad.dimensions[1] = 2;
    if (mode == 4)
      bad.allocated = nullptr;
    require(!retainForDisplay(context, bad, 1, type, true),
            "invalid narrow display range admitted");
    ++cases;
  }
  context.settle();
  const auto prefix = "[" + std::to_string(int(low));
  require(render(*result, {descriptor.dimensions.data(), 1}, type, true) ==
              prefix + ", 0, " + std::to_string(int(high)) + "]\n",
          "narrow display printed characters, wrong signs or lost retained storage");
  require(render(*result, {descriptor.dimensions.data(), 1}, type, true, 1) ==
              prefix + ", ...] (3 elements)\n",
          "narrow display limit changed");
  descriptor.dimensions[0] = 0;
  Invocation empty;
  auto none = retainForDisplay(empty, descriptor, 1, type, true);
  require(bool(none) && render(*none, {descriptor.dimensions.data(), 1}, type, true) == "[]\n",
          "empty narrow array not canonical");
  require(!context.live() && context.allocations() == context.releases(), "narrow display leaked");
  ++cases;
}
int main() {
  try {
    narrow<std::int8_t>(NumericType::Int8);
    narrow<std::uint8_t>(NumericType::UInt8);
    narrow<std::int16_t>(NumericType::Int16);
    narrow<std::uint16_t>(NumericType::UInt16);
    numeric<std::int32_t>(NumericType::Int32);
    numeric<std::uint32_t>(NumericType::UInt32);
    numeric<std::int64_t>(NumericType::Int64);
    numeric<std::uint64_t>(NumericType::UInt64);
    numeric<float>(NumericType::Float32);
    numeric<double>(NumericType::Float64);
    floating<float, std::uint32_t>(NumericType::Float32,
                                   {0x80000000, 1, 0x7f7fffff, 0x7f800000, 0x7fc12345},
                                   "[-0, 1.40129846e-45, 3.40282347e+38, inf, nan]\n");
    floating<double, std::uint64_t>(
        NumericType::Float64,
        {0x8000000000000000ULL, 1, 0x7fefffffffffffffULL, 0x7ff0000000000000ULL,
         0x7ff812345678abcdULL},
        "[-0, 4.9406564584124654e-324, 1.7976931348623157e+308, inf, nan]\n");
    Invocation empty;
    CollectionDescriptor descriptor;
    descriptor.dimensions[0] = 2;
    descriptor.dimensions[1] = 0;
    auto value = retainForDisplay(empty, descriptor, 2, NumericType::Int64, false);
    require(bool(value) && value->count == 0 && !empty.allocations(), "empty display allocated");
    empty.settle();
    require(render(*value, {descriptor.dimensions.data(), 2}, NumericType::Int64, false) ==
                "tensor([], [2, 0])\n",
            "empty matrix shape lost");
    ++cases;
    for (auto rank : {std::size_t(0), std::size_t(8), std::size_t(9), cli::maximumCollectionRank}) {
      Invocation context;
      std::int64_t cell = 7;
      auto *data = context.allocate(sizeof(cell));
      std::memcpy(data, &cell, sizeof(cell));
      CollectionDescriptor scalar;
      scalar.allocated = scalar.aligned = data;
      scalar.dimensions.fill(1);
      auto result = retainForDisplay(context, scalar, rank, NumericType::Int64, false);
      require(bool(result) && result->count == 1, "valid display rank rejected");
      context.settle();
      std::string shape;
      for (std::size_t axis = 0; axis < rank; ++axis)
        shape += axis ? ", 1" : "1";
      require(render(*result, {scalar.dimensions.data(), rank}, NumericType::Int64, false) ==
                  "tensor([7], [" + shape + "])\n",
              "high/rank-zero shape lost");
      ++cases;
    }
    Invocation failed;
    failed.failAllocationAt(1);
    require(!failed.allocate(4), "injected display allocation did not fail");
    require(!retainForDisplay(failed, descriptor, 2, NumericType::Int64, false),
            "faulted invocation admitted for display");
    failed.settle();
    require(!failed.live(), "failed display allocation leaked");
    ++cases;
    std::cout
        << "collection display: " << cases
        << " validation/format/lifetime cases, ten array/tensor numeric types, exact FP edges, "
           "interior views, full-range guards, retained printing, limits and ranks 0/8/9/128 "
           "passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
