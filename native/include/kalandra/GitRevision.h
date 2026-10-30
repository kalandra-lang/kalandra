#pragma once
#include <algorithm>
#include <string_view>
namespace kalandra {
inline bool exactGitCommit(std::string_view value) {
  return value.size() == 40 && std::ranges::all_of(value, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
// Restricted ref names, never rev-parse expressions, patterns or abbreviated IDs.
inline bool validGitRevision(std::string_view value) {
  if (exactGitCommit(value))
    return true;
  if (value.empty() || value.size() > 1024 || value == "HEAD")
    return false;
  if (value.starts_with("refs/")) {
    if (value.starts_with("refs/heads/"))
      value.remove_prefix(11);
    else if (value.starts_with("refs/tags/"))
      value.remove_prefix(10);
    else
      return false;
  } else if (std::ranges::all_of(value, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
             }))
    return false; // Hex-only names require an explicit refs/heads or refs/tags prefix.
  if (value.empty() || value.find("..") != std::string_view::npos)
    return false;
  while (!value.empty()) {
    const auto slash = value.find('/');
    const auto part = value.substr(0, slash);
    if (part.empty() || part.size() > 255 || part.front() == '.' || part.back() == '.' ||
        part.ends_with(".lock") || !std::ranges::all_of(part, [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-' || c == '.';
        }))
      return false;
    if (slash == std::string_view::npos)
      return true;
    value.remove_prefix(slash + 1);
  }
  return false;
}
} // namespace kalandra
