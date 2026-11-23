#pragma once
#include "../tools/BuildProgress.h"
#include "kalandra/Arithmetic.h"
#include "kalandra/Optimization.h"
#include <filesystem>
#include <map>
#include <string>
void buildMetalProgram(kalandra::cli::BuildProgress &progress, const std::filesystem::path &,
                       const std::map<std::string, std::string> &, const std::string &variant,
                       kalandra::Optimization level,
                       kalandra::ArithmeticMode arithmetic = kalandra::ArithmeticMode::Hardware,
                       kalandra::MatmulPolicy matmul = kalandra::MatmulPolicy::Optimized);
