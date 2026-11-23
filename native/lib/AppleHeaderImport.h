#pragma once
#include <expected>
#include <optional>
#include <set>
#include <string>
namespace kalandra {
std::expected<std::optional<std::string>, std::string>
importAppleFramework(const std::string &name, const std::set<std::string> &symbols);
}
