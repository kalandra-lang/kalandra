#pragma once
#include "kalandra/CliOutput.h"
namespace kalandra {
// Optional target backend, linked into the common native compiler when enabled.
int compileMetalCommand(int argc, char **argv, bool application = false, cli::Output feedback = {});
} // namespace kalandra
