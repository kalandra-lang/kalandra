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
static CheckResult analyze(std::string text) {
  return check(parse(std::make_shared<SourceFile>("parallel-map.k", std::move(text))));
}
static std::string read(const std::filesystem::path &path) {
  std::ifstream stream(path);
  return {std::istreambuf_iterator<char>(stream), {}};
}
int main() {
  try {
    unsigned accepted = 0, rejected = 0;
    for (const auto &file : std::filesystem::directory_iterator(KALANDRA_PARALLEL_FIXTURES)) {
      if (file.path().extension() != ".k")
        continue;
      auto checked = analyze(read(file.path()));
      if (!checked.ok())
        for (const auto &d : checked.diagnostics)
          std::cerr << renderDiagnostic(*checked.source, d);
      require(checked.ok(), "parallel map fixture rejected");
      auto planned = continuation::plan(*checked.module);
      require(bool(planned), "parallel map planning failed");
      bool seen = false;
      for (const auto &plan : planned->plans)
        for (const auto &op : plan->code)
          seen |= op.code == continuation::Code::ReleaseGroup;
      require(seen, "parallel map lost bounded group reclamation");
      ++accepted;
    }
    for (const auto &file : std::filesystem::directory_iterator(
             std::filesystem::path(KALANDRA_PARALLEL_FIXTURES) / "rejected")) {
      auto checked = analyze(read(file.path()));
      require(!checked.ok(), "invalid parallel map admitted");
      ++rejected;
    }
    require(accepted == 21 && rejected == 3, "parallel map catalog changed");
    auto checked = analyze(read(std::filesystem::path(KALANDRA_PARALLEL_FIXTURES) / "errors.k"));
    require(checked.ok(), "parallel error mutation fixture rejected");
    for (unsigned mode = 0; mode < 4; ++mode) {
      auto module = *checked.module;
      bool changed = false;
      for (auto &value : module.expressions)
        if (auto *map = std::get_if<ArrayMap>(&value.operation); map && map->parallel) {
          if (mode == 0)
            map->parallel = false;
          if (mode == 1)
            value.type.fallible = false;
          if (mode == 2)
            value.type.errorRecord.reset();
          if (mode == 3)
            map->function = module.functions.size();
          changed = true;
          break;
        }
      require(changed && numericCapabilityError(module, NumericTarget::Cpu).has_value(),
              "forged parallel map type/channel admitted");
    }
    std::cout << "Parallel map: 21 accepted, 3 rejected, 4 forged IR cases passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
