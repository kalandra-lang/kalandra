#pragma once
#include "../tools/BuildProgress.h"
#include "kalandra/Arithmetic.h"
#include "kalandra/Optimization.h"
#include <filesystem>
#include <map>
#include <string>
void buildCudaProgram(kalandra::cli::BuildProgress &progress, const std::filesystem::path &,
                      const std::map<std::string, std::string> &, const std::string &,
                      kalandra::Optimization level, kalandra::ArithmeticMode,
                      kalandra::MatmulPolicy, bool bundle = false);
