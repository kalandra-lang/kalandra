#pragma once
#include "kalandra/Arithmetic.h"
#include <filesystem>
#include <map>
#include <string>
void buildMetalProgram(const std::filesystem::path &, const std::map<std::string, std::string> &,
                       const std::string &variant,
                       kalandra::ArithmeticMode arithmetic = kalandra::ArithmeticMode::Hardware,
                       kalandra::MatmulPolicy matmul = kalandra::MatmulPolicy::Optimized);
