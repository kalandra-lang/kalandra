#include "kalandra/Formatter.h"
#include "kalandra/Version.h"
#include <iostream>
#include <span>
int main(int argc, char **argv) {
  if (kalandra::printVersion(argc, argv, "kalandra-fmt"))
    return 0;
  const std::span<char *> args(argv, static_cast<std::size_t>(argc));
  bool checking = false;
  std::string path;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view arg(args[i]);
    if (arg == "--check" && !checking)
      checking = true;
    else if (!arg.starts_with('-') && path.empty())
      path = arg;
    else {
      std::cerr << "usage: kalandra-fmt [--check] <source.k>\n";
      return 2;
    }
  }
  if (path.empty()) {
    std::cerr << "usage: kalandra-fmt [--check] <source.k>\n";
    return 2;
  }
  auto source = kalandra::loadSource(path);
  if (!source) {
    std::cerr << path << ": " << source.error() << '\n';
    return 1;
  }
  auto formatted = kalandra::formatSource(*source);
  if (!formatted.text) {
    for (const auto &diagnostic : formatted.diagnostics)
      std::cerr << kalandra::renderDiagnostic(**source, diagnostic);
    return 1;
  }
  if (checking)
    return *formatted.text == (*source)->text() ? 0 : 1;
  std::cout << *formatted.text;
  return std::cout ? 0 : 1;
}
