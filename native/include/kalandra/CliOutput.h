#pragma once
#include <algorithm>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>

namespace kalandra::cli {
inline std::string printable(std::string_view value) {
  std::string text(value);
  for (char &c : text)
    if (static_cast<unsigned char>(c) < 32 || c == 127)
      c = '?';
  return text;
}

struct Output {
  bool silent = false;
  bool debug = false;

  // Leave option values (including filenames spelled "--silent") to their parser.
  static Output take(int &argc, char **argv, std::initializer_list<std::string_view> valued = {}) {
    Output result;
    int next = 1;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (arg == "--silent")
        result.silent = true;
      else if (arg == "--debug")
        result.debug = true;
      else {
        argv[next++] = argv[i];
        if (std::find(valued.begin(), valued.end(), arg) != valued.end() && i + 1 < argc)
          argv[next++] = argv[++i];
      }
    }
    argc = next;
    argv[argc] = nullptr;
    return result;
  }
  void trace(std::string_view operation, std::string_view subject) const {
    if (debug)
      std::cerr << "[debug] " << operation << " " << printable(subject) << '\n';
  }
};
} // namespace kalandra::cli
