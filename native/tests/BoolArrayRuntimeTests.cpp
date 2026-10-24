#include "../runtime/CollectionDisplay.h"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    Invocation source;
    auto *data = static_cast<unsigned char *>(kalandra_record_alloc_v1(&source, 4097));
    require(data, "source allocation failed");
    for (unsigned i = 0; i < 4097; ++i)
      data[i] = i % 2;
    cli::CollectionDescriptor value;
    value.allocated = value.aligned = data;
    value.dimensions[0] = 4097;
    value.dimensions[1] = 1;
    Invocation kept;
    std::atomic<std::uint64_t> polls = 0;
    kept.watchPolls(&polls);
    // Seventeen validation chunks plus the checked ownership retain.
    require(kalandra_bool_array_retain_v1(&kept, &source, &value) && polls == 18,
            "Bool retain did not validate/poll all lanes");
    for (unsigned mode = 0; mode < 12; ++mode) {
      auto forged = value;
      Invocation dest;
      switch (mode) {
      case 0:
        forged.offset = 1;
        break;
      case 1:
        forged.dimensions[0] = -1;
        break;
      case 2:
        forged.dimensions[0] = 4098;
        break;
      case 3:
        forged.dimensions[0] = INT64_MAX;
        break;
      case 4:
        forged.dimensions[1] = 2;
        break;
      case 5:
        forged.dimensions[1] = -1;
        break;
      case 6:
        forged.aligned = data + 1;
        break;
      case 7:
        forged.allocated = nullptr;
        break;
      case 8:
        forged.dimensions[0] = 0;
        forged.dimensions[1] = 0;
        break;
      case 9:
        data[4096] = 2;
        break;
      case 10:
        data[4096] = 255;
        break;
      case 11:
        forged.allocated = forged.aligned = reinterpret_cast<void *>(0x1000);
        break;
      }
      require(!kalandra_bool_array_retain_v1(&dest, &source, &forged) && dest.status() == 3,
              "invalid Bool storage accepted");
      dest.settle();
      require(!dest.live() && dest.allocations() == dest.releases(),
              "invalid Bool retention leaked");
      data[4096] = 0;
    }
    auto display = retainBoolForDisplay(source, value);
    require(bool(display), "Bool display retention failed");
    source.settle();
    require(kept.lease(data).has_value(), "Bool retained backing lost");
    require(display->data[4095] == std::byte{1}, "Bool display lease lost");
    kept.settle();
    require(!kept.live() && kept.allocations() == kept.releases(), "Bool owners unbalanced");
    for (unsigned limit : {0u, 1u, 4097u}) {
      auto *file = std::tmpfile();
      require(file, "tmpfile failed");
      require(printBoolArray(file, *display, limit), "Bool display failed");
      std::rewind(file);
      std::array<char, 32> out{};
      require(std::fread(out.data(), 1, limit ? 7 : 6, file) > 0, "Bool display empty");
      require(std::string_view(out.data()).starts_with(limit ? "[false" : "[..."),
              "Bool display text changed");
      std::fclose(file);
    }
    Invocation empty;
    cli::CollectionDescriptor zero;
    auto noValues = retainBoolForDisplay(empty, zero);
    require(noValues && noValues->count == 0, "canonical empty Bool rejected");
    Invocation invalid;
    data = static_cast<unsigned char *>(kalandra_record_alloc_v1(&invalid, 2));
    data[0] = 0;
    data[1] = 2;
    value.allocated = value.aligned = data;
    value.dimensions[0] = 2;
    value.dimensions[1] = 1;
    require(!retainBoolForDisplay(invalid, value) && invalid.status() == 3,
            "hidden invalid Bool display lane accepted");
    invalid.settle();
    require(!invalid.live() && invalid.allocations() == invalid.releases(),
            "invalid Bool display leaked");
    std::cout << "Bool array storage: canonical bytes, descriptor validation, polling, retained "
                 "display and cleanup passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
