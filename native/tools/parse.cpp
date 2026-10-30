#include "kalandra/Syntax.h"
#include "kalandra/Version.h"
#include <expected>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>

int main(int argc, char **argv) {
  if (kalandra::printVersion(argc, argv, "kalandra-parse"))
    return 0;
  const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));
  if (arguments.size() != 2) {
    std::cerr << "usage: kalandra-parse <source.k>\n";
    return 2;
  }
  auto source = kalandra::loadSource(arguments[1]);
  if (!source) {
    std::cerr << arguments[1] << ": " << source.error() << '\n';
    return 1;
  }
  auto result = kalandra::parse(*source);
  if (!result.ok()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << kalandra::renderDiagnostic(**source, diagnostic);
    return 1;
  }
  std::cout << "Parsed " << result.module.functions.size() << " functions, "
            << result.module.expressions.size() << " expressions (syntax only)\n";
}
