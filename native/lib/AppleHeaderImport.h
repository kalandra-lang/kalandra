#pragma once
#include "kalandra/Syntax.h"
#include <expected>
#include <optional>
#include <set>
#include <string>
namespace kalandra {
std::expected<std::string, std::string> importAppleCHeader(const std::string &path,
                                                           const std::set<std::string> &wanted,
                                                           HeaderContracts &bounds,
                                                           bool framework = false);
std::expected<std::optional<std::string>, std::string>
importAppleFramework(const std::string &name, const std::set<std::string> &symbols,
                     HeaderContracts &bounds);
}
