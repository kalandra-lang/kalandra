#include "kalandra/Semantics.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected isolated fixture directory");
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    const auto checkSource = [&](const std::string &text) {
      const auto path = root / "main.k";
      {
        std::ofstream stream(path);
        stream << text;
        require(bool(stream), "cannot write iterator contract fixture");
      }
      auto loaded = loadModules(path.string());
      require(bool(loaded), "cannot load iterator contract fixture");
      return check(std::move(*loaded));
    };
    auto canonical = checkSource(
        "from \"std/option\" import Option\nfrom \"std/option\" import Option as Maybe\n"
        "fn get[T](value:Option[T],fallback:T)->T:\n  return match value:\n"
        "    Option.None:fallback\n    Option.Some {value}:value\n"
        "fn main()->i64:\n  absent:Maybe[i64]=Maybe.None\n"
        "  present=Option.Some {value:42}\n  text=Maybe.Some {value:\"yes\"}\n"
        "  unused=get(text,\"no\")\n  return get(absent,7)+get(present,0)\n");
    for (const auto &d : canonical.diagnostics)
      std::cerr << renderDiagnostic(*canonical.source, d);
    require(canonical.ok(), "standard Option aliases lost generic nominal identity");
    auto resource = checkSource("from \"std/option\" import Option\n"
                                "from \"std/buffer\" import byteBuffer,BufferError\n"
                                "fn main()->unit ! BufferError:\n  value=try byteBuffer(0)\n"
                                "  illegal=Option.Some {value:value}\n  return ()\n");
    require(!resource.ok() && !resource.module,
            "standard Option bypassed owned-resource aggregate restrictions");
    auto exhaustive =
        checkSource("from \"std/option\" import Option\nfn get(value:Option[i64])->i64:\n"
                    "  return match value:\n    Option.Some {value}:value\n"
                    "fn main()->i64:\n  return get(Option[i64].None)\n");
    require(!exhaustive.ok() && !exhaustive.module,
            "standard Option omitted exhaustion without an exhaustiveness error");
    std::cout
        << "PASS: canonical Option aliases, specialization, ownership and exhaustion contracts\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
