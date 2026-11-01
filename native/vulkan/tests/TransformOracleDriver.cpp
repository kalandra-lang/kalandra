#include "Program.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime::vulkan;
namespace {
constexpr auto backendArtifactSuffix = ".spv";
bool backendValidationClean(Runtime &runtime) { return !runtime.validationErrors(); }
} // namespace
#include "../../gpu-runtime/TransformOracleDriver.inc"
