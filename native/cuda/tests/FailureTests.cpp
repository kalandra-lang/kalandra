#include "Cuda.h"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime::cuda;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected artifact directory");
    auto path = std::filesystem::path(argv[1]) / "sm_80-f32-hardware.ptx";
    for (auto point :
         {FailurePoint::Allocate, FailurePoint::Upload, FailurePoint::ModuleLoad,
          FailurePoint::Launch, FailurePoint::WaitAfterCompletion, FailurePoint::Readback}) {
      auto made = Runtime::create();
      check(bool(made), "runtime");
      auto &r = **made;
      std::array<float, 3> data{1, 2, 3};
      std::array<unsigned, 2> words{3, 0};
      auto a = r.uploadBytes(std::as_bytes(std::span(data)));
      auto b = r.uploadBytes(std::as_bytes(std::span(data)));
      auto out = r.blankBytes(sizeof(data));
      auto p = r.uploadBytes(std::as_bytes(std::span(words)));
      auto k = r.loadKernel(path);
      check(a && b && out && p && k, "baseline resources");
      auto before = r.retainedBytes();
      r.injectFailure(point);
      if (point == FailurePoint::Allocate)
        check(!r.blankBytes(4), "injected allocation failure");
      else if (point == FailurePoint::Upload)
        check(!r.uploadBytes(std::as_bytes(std::span(data))), "injected upload failure");
      else if (point == FailurePoint::ModuleLoad)
        check(!r.loadKernel(std::filesystem::path(argv[1]) / "sm_80-f64-hardware.ptx"),
              "injected module failure");
      else if (point == FailurePoint::Readback) {
        std::array<std::byte, 12> target;
        target.fill(std::byte{0x5a});
        auto original = target;
        check(!r.readBytes(*a, target) && target == original, "readback failure transactional");
      } else {
        std::array bindings{*a, *b, *out, *p};
        check(!r.execute(*k, bindings, 1), "injected submission/completed wait failure");
        check(!r.execute(*k, bindings, 1), "poisoned service refuses new work");
        check(!r.loadKernel(path), "poisoned service refuses cached module admission");
        check(!r.blankBytes(0), "poisoned service refuses new empty storage");
      }
      check(r.retainedBytes() == before, "failure returns temporary quotas");
      a = Buffer{};
      b = Buffer{};
      out = Buffer{};
      p = Buffer{};
      k = Kernel{};
      check(r.retainedBytes() == 0, "owned storage reclaimed despite synthetic poison");
    }
    std::cout << "PASS: six safe failure points; transactional readback, bounded admission, "
                 "settled completion and poisoned-context cleanup\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
