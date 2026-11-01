#include "Program.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime::vulkan;
namespace {
constexpr auto backendArtifactSuffix = ".spv";
bool backendValidationClean(Runtime &runtime) { return !runtime.validationErrors(); }
} // namespace
#include "../../gpu-runtime/TransformTests.inc"
