#include "kalandra/Continuation.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace kalandra;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::filesystem::path &path) {
  std::ifstream file(path);
  std::string text{std::istreambuf_iterator<char>(file), {}};
  return check(parse(std::make_shared<SourceFile>(path.string(), std::move(text))));
}
int main() {
  try {
    unsigned accepted = 0, rejected = 0;
    for (const auto &file : std::filesystem::directory_iterator(KALANDRA_RESUMABLE_FIXTURES)) {
      if (file.path().extension() != ".k")
        continue;
      auto result = analyze(file.path());
      if (!result.ok())
        for (const auto &error : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, error);
      require(result.ok(), "valid resumable callable rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), plan ? "" : plan.error().c_str());
      bool dispatch = false;
      for (const auto &entry : plan->plans)
        for (const auto &op : entry->code)
          dispatch |= op.code == continuation::Code::CallableTarget;
      require(dispatch, "resumable dispatch disappeared from plan");
      require(numericCapabilityError(*result.module, NumericTarget::AppleGpu).has_value(),
              "GPU callable admitted");
      ++accepted;
    }
    for (const auto &file : std::filesystem::directory_iterator(
             std::filesystem::path(KALANDRA_RESUMABLE_FIXTURES) / "rejected")) {
      require(!analyze(file.path()).ok(), "unproven coordinating callable admitted");
      ++rejected;
    }
    require(accepted == 23 && rejected == 1, "resumable fixture catalog changed");
    std::cout
        << "Resumable callables: 23 checked/planned programs, CPU gates and 1 rejection passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
