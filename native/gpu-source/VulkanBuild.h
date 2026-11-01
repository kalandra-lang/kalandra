#pragma once
#include "kalandra/Arithmetic.h"
#include <filesystem>
#include <map>
#include <string>
void buildVulkanProgram(const std::filesystem::path &, const std::map<std::string, std::string> &,
                        const std::string &, kalandra::ArithmeticMode);
