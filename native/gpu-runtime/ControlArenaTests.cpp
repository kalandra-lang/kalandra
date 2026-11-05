#include "ControlArena.h"
#include "ControlExport.h"
#include "ControlTensorExport.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
namespace {
enum class Error { Invalid, Budget, Device };
struct Ledger {
  std::atomic<std::uint64_t> bytes{0};
};
struct Storage {
  Ledger &ledger;
  std::vector<std::byte> data;
  Storage(Ledger &ledger, std::size_t bytes) : ledger(ledger), data(bytes) {
    ledger.bytes += bytes;
  }
  ~Storage() { ledger.bytes -= data.size(); }
};
struct Buffer {
  std::shared_ptr<Storage> storage;
  explicit operator bool() const { return bool(storage); }
  std::uint64_t bytes() const { return storage ? storage->data.size() : 0; }
};
using Arena = kalandra::runtime::detail::ControlArena<Buffer, Error>;
static_assert(!std::is_copy_constructible_v<Arena> && !std::is_copy_assignable_v<Arena>);
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Driver {
  Ledger ledger;
  std::uint64_t limit = 4 * 1024 * 1024, copyCount = 0, failCopy = 0;
  std::expected<Buffer, Error> allocate(std::uint64_t bytes) {
    if (bytes > limit || ledger.bytes > limit - bytes)
      return std::unexpected(Error::Budget);
    return Buffer{std::make_shared<Storage>(ledger, bytes)};
  }
  std::expected<void, Error> copy(const Buffer &from, const Buffer &to, std::uint64_t offset,
                                  std::uint64_t count) {
    check(from.storage != to.storage, "growth aliases its source allocation");
    check(count && count <= Arena::CopyBytes && offset <= from.bytes() && offset <= to.bytes() &&
              count <= from.bytes() - offset && count <= to.bytes() - offset,
          "growth exceeded copy quantum or backing bounds");
    if (++copyCount == failCopy)
      return std::unexpected(Error::Device);
    std::memcpy(to.storage->data.data() + offset, from.storage->data.data() + offset, count);
    return {};
  }
  std::expected<bool, Error> step(Arena &arena) {
    return arena.advance([&](auto bytes) { return allocate(bytes); },
                         [&](const auto &a, const auto &b, auto offset, auto bytes) {
                           return copy(a, b, offset, bytes);
                         });
  }
};
void lifecycle() {
  Driver driver;
  {
    Arena arena;
    check(!arena.request(0) && !driver.step(arena), "inactive arena admitted invalid growth");
    const auto words = 2 * Arena::CopyBytes / 8 + 3;
    check(bool(arena.request(words)), "initial request");
    check(driver.step(arena).value_or(false), "initial allocation");
    auto &original = arena.buffer().storage->data;
    for (std::size_t i = 0; i < original.size(); ++i)
      original[i] = std::byte((i * 17 + 23) % 251);
    const auto snapshot = original;
    check(!arena.request(words) && !arena.request(UINT64_MAX) &&
              !arena.request(Arena::MaxWords + 1),
          "invalid requests changed capacity");
    check(bool(arena.request(words * 3)), "growth request");
    check(!arena.request(words * 4), "concurrent growth request replaced pending work");
    check(driver.step(arena) == false && arena.words() == words, "first copy retired old backing");
    Arena moved(std::move(arena));
    check(!arena.growing() && !arena.buffer(), "moved-from arena retains mutable backing");
    check(driver.step(moved) == false && moved.words() == words,
          "second copy published a partial arena");
    check(driver.step(moved) == true && moved.words() == words * 3 && driver.copyCount == 3,
          "copy quantum or commit boundary");
    const auto &result = moved.buffer().storage->data;
    check(std::equal(snapshot.begin(), snapshot.end(), result.begin()),
          "growth changed opaque bytes");
    check(std::all_of(result.begin() + snapshot.size(), result.end(),
                      [](auto value) { return value == std::byte{}; }),
          "new backing was not blank");
    check(driver.ledger.bytes == result.size(), "old backing retained after complete copy");
    const auto before = moved.buffer().storage;
    driver.limit = result.size();
    check(bool(moved.request(words * 4)), "budget probe request");
    const auto failed = driver.step(moved);
    check(!failed && failed.error() == Error::Budget && moved.buffer().storage == before,
          "denied allocation corrupted old backing or lost its failure kind");
  }
  check(driver.ledger.bytes == 0, "arena completion/failure retained storage");
}
void interruptedCopy() {
  Driver driver;
  {
    Arena arena;
    const auto words = 2 * Arena::CopyBytes / 8;
    check(bool(arena.request(words)) && driver.step(arena).value_or(false),
          "copy probe allocation");
    arena.buffer().storage->data.front() = std::byte{42};
    check(bool(arena.request(words * 2)), "copy probe growth");
    driver.failCopy = 2;
    check(driver.step(arena) == false, "copy probe did not suspend");
    auto failed = driver.step(arena);
    check(!failed && failed.error() == Error::Device && arena.words() == words &&
              arena.buffer().storage->data.front() == std::byte{42},
          "copy failure published partial state");
    check(driver.ledger.bytes == words * 8 * 3, "unsettled copy lost a storage owner");
    // Destruction after the callback settles models cancellation/fault cleanup.
  }
  check(driver.ledger.bytes == 0, "interrupted growth retained either allocation");
}
void asynchronousPages() {
  Driver driver;
  {
    Arena arena(false);
    check(bool(arena.request(Arena::CopyBytes / 8 + 1)), "asynchronous initial request");
    auto allocate = [&](auto bytes) {
      auto buffer = driver.allocate(bytes);
      if (buffer)
        std::fill(buffer->storage->data.begin(), buffer->storage->data.end(), std::byte{99});
      return buffer;
    };
    unsigned clears = 0;
    while (arena.growing()) {
      auto page = arena.beginPage(allocate);
      check(bool(page) && bool(*page), "missing asynchronous initialization page");
      check((**page).clear && !(**page).source && (**page).bytes <= Arena::CopyBytes,
            "fresh arena page is not bounded initialization");
      check(!arena.beginPage(allocate) && !arena.buffer(), "unacknowledged page was republished");
      const auto &p = **page;
      std::fill_n(p.target.storage->data.begin() + p.offset, p.bytes, std::byte{});
      check(bool(arena.acknowledge()), "acknowledge initialization page");
      ++clears;
    }
    check(clears == 2 &&
              std::all_of(arena.buffer().storage->data.begin(), arena.buffer().storage->data.end(),
                          [](auto byte) { return byte == std::byte{}; }),
          "asynchronous initialization published uninitialized bytes");
    arena.buffer().storage->data.front() = std::byte{42};
    check(bool(arena.request(arena.words() * 2)), "asynchronous growth request");
    unsigned copies = 0;
    while (arena.growing()) {
      auto page = arena.beginPage(allocate);
      check(bool(page) && bool(*page), "missing asynchronous growth page");
      const auto &p = **page;
      if (p.clear)
        std::fill_n(p.target.storage->data.begin() + p.offset, p.bytes, std::byte{});
      else {
        check(bool(driver.copy(p.source, p.target, p.offset, p.bytes)), "asynchronous prefix copy");
        ++copies;
      }
      check(bool(arena.acknowledge()), "acknowledge growth page");
    }
    check(copies == 2 && arena.buffer().storage->data.front() == std::byte{42},
          "asynchronous growth failed to preserve its prefix");
    check(!arena.acknowledge(), "duplicate acknowledgement admitted");
  }
  check(driver.ledger.bytes == 0, "asynchronous growth retained allocations");
}
} // namespace
void captureLengths() {
  using namespace kalandra::runtime::detail;
  const std::array<std::uint8_t, 4> roles{2, 1, 2, 0};
  std::array<std::uint64_t, 4> sizes{32, 1, 72, 256};
  std::array<std::uint64_t, 8> frame{};
  auto operation = [&](bool initialize) {
    return controlInputLengths(
        roles, frame.size(), initialize,
        [&](std::size_t i) {
          check(i == 0 || i == 2, "scalar treated as array wire");
          return sizes[i];
        },
        [&](std::size_t i) { return frame[i]; },
        [&](std::size_t i, std::uint64_t value) { frame[i] = value; });
  };
  check(operation(true) && frame[6] == 4 && frame[7] == 9 && operation(false),
        "capture sizes were not sealed in the frame tail");
  frame[6] = 5;
  check(!operation(false), "modified GPU capture metadata was accepted");
  frame[6] = 4;
  sizes[2] = 80;
  check(!operation(false), "changed capture backing size was accepted");
  sizes[2] = 72;
  for (auto malformed :
       {UINT64_C(0), UINT64_C(24), UINT64_C(33), UINT64_C(8) * (UINT64_C(1) + UINT32_MAX)}) {
    auto before = frame;
    sizes[2] = malformed;
    check(!operation(true) && frame == before,
          "malformed later input published partial capture metadata");
  }
  check(!validControlInputRoles(roles, 5), "metadata overlaps control header");
  for (auto malformed : {std::array<std::uint8_t, 4>{2, 0, 2, 0}, {2, 1, 2, 1}, {3, 1, 2, 0}})
    check(!validControlInputRoles(malformed, 8), "unknown/domain role accepted as capture");
}
void exportRoles() {
  using namespace kalandra::runtime;
  using namespace kalandra::runtime::detail;
  std::array<std::uint8_t, 3> roles{2, 1, 3};
  check(!validControlInputRoles(roles, DeviceArrayExportABI::Cells), "export footer overlaps descriptor");
  check(validControlInputRoles(roles, DeviceArrayExportABI::Cells + 1), "valid export footer rejected");
  roles[0] = 3;
  check(!validControlInputRoles(roles, 64), "export role accepted as input capture");
}
void residentTensorMetadata() {
  using kalandra::runtime::DeviceArrayWireABI;
  using namespace kalandra::runtime::detail;
  using Roles = ControlTensorInputs;
  const std::array<std::uint8_t, 4> roles{
      Roles::FirstRole, DeviceArrayWireABI::InputRole, Roles::FirstRole + 2, Roles::FirstResultRole + 2};
  std::array<std::vector<std::int64_t>, 3> shapes{{{}, {}, {2, 3}}};
  std::array<std::uint64_t, 3> sizes{4, 32, 12};
  const std::array<std::uint64_t, 3> widths{4, 0, 2};
  std::array<std::uint64_t, 12> frame{};
  const auto seal = [&](bool initialize, std::size_t prefix = 7) {
    return sealControlInputs(roles, frame.size(), prefix, 64, initialize,
        [&](std::size_t i) -> std::span<const std::int64_t> { return shapes.at(i); },
        [&](std::size_t i) { return sizes.at(i); },
        [&](std::size_t i) { return widths.at(i); },
        [&](std::size_t i) { return frame.at(i); },
        [&](std::size_t i, std::uint64_t value) { frame.at(i) = value; });
  };
  check(seal(true) && seal(false) &&
            std::ranges::equal(std::span(frame).last(5), std::array{1, 4, 6, 2, 3}),
        "mixed tensor and array metadata was not sealed in signature order");
  check(!seal(true, 8), "tensor footer overlapped the protected prefix");
  ++frame.back();
  check(!seal(false), "changed GPU tensor extent was accepted");
  frame.fill(77);
  const auto original = frame;
  for (auto invalid : {std::vector<std::int64_t>{-1, 3}, {9, 8}, {65, 0}, {6}}) {
    shapes[2] = invalid;
    check(!seal(true) && frame == original, "invalid tensor shape changed the footer");
  }
  shapes[2] = {0, 64};
  sizes[2] = 2;
  check(!seal(true) && frame == original, "empty tensor with nonempty storage was accepted");
  sizes[2] = 0;
  check(seal(true) && seal(false) && frame[9] == 0 && frame[11] == 64,
        "empty tensor lost its runtime extents");
}
void tensorResultDescriptor() {
  using namespace kalandra::runtime::detail;
  using A = ControlTensorExportABI;
  std::array<std::uint64_t, A::Cells> frame{};
  const auto range = [&](unsigned rank) {
    return controlTensorExportRange([&](auto i) { return frame.at(i); }, 32, rank);
  };
  frame[A::Root] = 4;
  frame[A::Data] = 8;
  frame[A::Length] = 1;
  check(range(0).has_value(), "rank-zero tensor result rejected");
  frame[A::Length] = 0;
  check(!range(0), "rank-zero tensor result lost its scalar element");
  frame[A::Data] = 10;
  frame[A::Extents + 1] = 7;
  check(range(2).has_value(), "empty tensor result rejected");
  frame[A::Extents] = 2;
  frame[A::Length] = 14;
  const auto valid = range(2);
  check(valid && valid->offsetWords == 10 && valid->length == 14 && valid->extents[1] == 7,
        "tensor result range changed its shape or payload");
  frame[A::Extents + 2] = 1;
  check(!range(2), "tensor descriptor admitted nonzero trailing metadata");
  frame[A::Extents + 2] = 0;
  frame[A::Data] = 19;
  check(!range(2), "tensor descriptor admitted out-of-arena payload");
}
void resultDescriptor() {
  using namespace kalandra::runtime;
  using namespace kalandra::runtime::detail;
  using A = ControlArrayExportABI;
  std::array<std::uint64_t, A::Cells> frame{};
  auto checkRange = [&] {
    return controlArrayExportRange([&](auto i) { return frame[i]; }, 32, 2);
  };
  frame[A::Layout] = 4;
  check(bool(checkRange()), "canonical empty result rejected");
  frame[A::Root] = 5;
  frame[A::Data] = 9;
  frame[A::Length] = 23;
  auto range = checkRange();
  check(range && range->offsetWords == 9 && range->metadata.length == 23,
        "exact terminal arena range rejected");
  frame[A::Length] = 24;
  check(!checkRange(), "terminal range exceeded actual backing");
  frame[A::Root] = DeviceArrayWireABI::ExternalHandle | 2;
  frame[A::Data] = 0;
  frame[A::Origin] = 1;
  frame[A::Length] = 1000;
  check(bool(checkRange()), "captured alias treated as local arena address");
  frame[A::Origin] = 3;
  check(!checkRange(), "unknown origin accepted");
  frame[A::Origin] = 1;
  frame[A::Data] = 1;
  check(!checkRange(), "captured result imported an arena address");
  frame[A::Data] = 0;
  frame[A::Root] = DeviceArrayWireABI::ExternalHandle | 3;
  check(!checkRange(), "unknown capture handle accepted");
  frame[A::Root] = 0;
  frame[A::Origin] = 0;
  frame[A::Length] = 1;
  check(!checkRange(), "null root contained payload");
  frame[A::Length] = 0;
  frame[A::Layout] = 12;
  check(!checkRange(), "unknown result type accepted");
}
void resultExport() {
  using Export = kalandra::runtime::detail::ControlExport<Buffer, Error>;
  Driver driver;
  {
    auto source = driver.allocate(4 * Export::CopyBytes).value();
    auto &bytes = source.storage->data;
    for (std::size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = std::byte((i * 13 + 9) % 251);
    constexpr std::uint64_t offset = 3, words = 2 * Export::CopyBytes / 8 + 5;
    Export value;
    check(!value.start(source, 0, 0) && !value.start(source, UINT64_MAX, 4) &&
              !value.start(source, source.bytes() / 8 - 1, 4),
          "invalid export range accepted");
    check(bool(value.start(source, offset, words)), "start export");
    check(!value.start(source, offset, words), "replaced pending export");
    std::weak_ptr<Storage> weak = source.storage;
    source = {};
    unsigned pages = 0;
    while (!value.complete()) {
      auto page = value.beginPage([&](auto size) { return driver.allocate(size); });
      check(bool(page) && page->bytes && page->bytes <= Export::CopyBytes, "unbounded export page");
      check(page->sourceOffset == offset * 8 + page->targetOffset,
            "export offset translation changed");
      check(!value.beginPage([&](auto size) { return driver.allocate(size); }) &&
                !value.complete() && !value.result(),
            "unacknowledged page published/reused");
      std::memcpy(page->target.storage->data.data() + page->targetOffset,
                  page->source.storage->data.data() + page->sourceOffset, page->bytes);
      Export moved(std::move(value));
      check(!value.active() && !value.complete(), "moved export retained pending state");
      auto ack = moved.acknowledge();
      check(bool(ack), "export fence acknowledgement");
      value = std::move(moved);
      ++pages;
    }
    check(pages == 3 && weak.expired(),
          "export retired source early or retained it after final fence");
    check(value.result()->bytes() == words * 8, "export result size changed");
    const auto &out = value.result()->storage->data;
    for (std::size_t i = 0; i < out.size(); ++i)
      check(out[i] == std::byte(((i + offset * 8) * 13 + 9) % 251),
            "export copied wrong arena range");
  }
  check(!driver.ledger.bytes, "export retained owners after result release");
  {
    auto source = driver.allocate(1024).value();
    Export value;
    check(bool(value.start(source, 0, 4)), "failed export setup");
    driver.limit = 1024;
    check(!value.beginPage([&](auto size) { return driver.allocate(size); }),
          "export bypassed allocation budget");
    check(!value.complete(), "failed export published destination");
  }
  check(!driver.ledger.bytes, "failed export leaked source");
  driver.limit = 4 * 1024 * 1024;
  {
    auto source = driver.allocate(1024).value();
    Export value;
    check(bool(value.start(source, 0, 4)), "cancel export setup");
    auto page = value.beginPage([&](auto size) { return driver.allocate(size); });
    check(bool(page), "cancel export admission");
    source = {};
    value = {};
    check(driver.ledger.bytes == 1024 + 32, "in-flight page lost retained owners");
    page = {};
  }
  check(!driver.ledger.bytes, "cancelled export leaked retained page");
}
int main() {
  try {
    exportRoles();
    resultDescriptor();
    resultExport();
    captureLengths();
    residentTensorMetadata();
    tensorResultDescriptor();
    lifecycle();
    interruptedCopy();
    asynchronousPages();
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; ++i)
      threads.emplace_back(lifecycle);
    for (auto &thread : threads)
      thread.join();
    std::cout
        << "PASS host arena admission: paged copies, isolation, move ownership, failure cleanup\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
