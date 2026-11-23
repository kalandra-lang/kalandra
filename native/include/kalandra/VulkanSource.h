#pragma once
#include "kalandra/CliOutput.h"
namespace kalandra {
int compileVulkanCommand(int argc, char **argv, bool application = false, cli::Output feedback = {}, bool bundle = false);
}
