#pragma once
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace kalandra::runtime::tree {
namespace fs = std::filesystem;
struct InvalidTreePath {};
inline void treeSystemError(int code) { throw std::system_error(code, std::system_category()); }

struct TreeLocation {
  fs::path anchor;
  std::vector<fs::path> components;
};

inline TreeLocation treeLocation(const fs::path &path, bool removing) {
  if (path.empty())
    throw InvalidTreePath{};
  auto leaf = path;
  while (!leaf.has_filename() && leaf.has_relative_path())
    leaf = leaf.parent_path();
  if (removing && (leaf.filename() == "." || leaf.filename() == ".."))
    throw InvalidTreePath{};
  TreeLocation loc;
#ifdef _WIN32
  // Start at a drive or share handle. Each subsequent open is relative to the
  // preceding handle and takes exactly one component.
  auto raw = path.native();
  std::replace(raw.begin(), raw.end(), L'/', L'\\');
  if (raw.starts_with(L"\\\\.\\"))
    throw InvalidTreePath{};
  if (!raw.starts_with(L"\\\\?\\")) {
    const auto n = GetFullPathNameW(raw.c_str(), 0, nullptr, nullptr);
    if (!n)
      treeSystemError(GetLastError());
    std::wstring full(n, L'\0');
    const auto used = GetFullPathNameW(raw.c_str(), n, full.data(), nullptr);
    if (!used || used >= n)
      treeSystemError(used ? ERROR_INSUFFICIENT_BUFFER : GetLastError());
    full.resize(used);
    raw = std::move(full);
  }
  std::size_t end = 0;
  const bool extended = raw.starts_with(L"\\\\?\\");
  const auto drive = extended ? 4u : 0u;
  if (raw.size() >= drive + 3 && raw[drive + 1] == L':' && raw[drive + 2] == L'\\' &&
      ((raw[drive] >= L'A' && raw[drive] <= L'Z') || (raw[drive] >= L'a' && raw[drive] <= L'z'))) {
    end = drive + 3;
  } else if ((!extended && raw.starts_with(L"\\\\")) ||
             (extended && _wcsnicmp(raw.c_str() + 4, L"UNC\\", 4) == 0)) {
    const auto server = extended ? 8u : 2u;
    const auto share = raw.find(L'\\', server);
    if (share == raw.npos || share == server || share + 1 == raw.size())
      throw InvalidTreePath{};
    end = raw.find(L'\\', share + 1);
    if (end == share + 1)
      throw InvalidTreePath{};
    if (end == raw.npos)
      end = raw.size();
  } else {
    throw InvalidTreePath{};
  }
  loc.anchor = raw.substr(0, end);
  for (std::size_t i = end; i < raw.size();) {
    if (raw[i] == L'\\') {
      ++i;
      continue;
    }
    const auto j = raw.find(L'\\', i);
    auto part = raw.substr(i, j == raw.npos ? raw.size() - i : j - i);
    if (part == L".." || part.find(L':') != part.npos)
      throw InvalidTreePath{};
    if (part != L".")
      loc.components.emplace_back(std::move(part));
    i = j == raw.npos ? raw.size() : j;
  }
#else
  loc.anchor = path.is_absolute() ? "/" : ".";
  for (const auto &part : path.relative_path())
    if (!part.empty() && part != ".")
      loc.components.push_back(part);
#endif
  if (removing && loc.components.empty())
    throw InvalidTreePath{};
  return loc;
}

} // namespace kalandra::runtime::tree
