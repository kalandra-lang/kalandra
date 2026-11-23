#pragma once
#include "kalandra/HeaderImport.h"
#include <expected>
#include <filesystem>
#include <set>

namespace kalandra {
std::expected<std::string, std::string>
importPortableCHeader(const std::string &path, const std::string &driver,
                      const std::optional<std::filesystem::path> &resources,
                      const std::set<std::string> &symbols, HeaderContracts &bounds, const HeaderTarget *target = nullptr);
}
