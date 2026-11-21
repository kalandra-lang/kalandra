#pragma once
#include "../tools/BuildProgress.h"
#include "kalandra/Arithmetic.h"
#include <filesystem>
#include <map>
#include <string>
void buildVulkanProgram(kalandra::cli::BuildProgress &progress, const std::filesystem::path &, const std::map<std::string, std::string> &,
                        const std::string &, kalandra::ArithmeticMode, kalandra::MatmulPolicy);
