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
using namespace kalandra::runtime::cuda;
namespace {
constexpr auto backendArtifactSuffix = ".ptx";
bool backendValidationClean(Runtime &runtime) { return runtime.healthy(); }
} // namespace
#include "../../gpu-runtime/TransformTests.inc"
