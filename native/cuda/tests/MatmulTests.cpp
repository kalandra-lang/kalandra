#include "Program.h"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime::cuda;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, Error> value, const char *message) {
  if (!value)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(value.error())));
  return std::move(*value);
}
void runCase(Runtime &runtime, const Kernel &kernel, unsigned m, unsigned n, unsigned k,
             bool native) {
  std::vector<float> a(m * k), b(k * n), expected(m * n);
  for (unsigned i = 0; i < m; ++i)
    for (unsigned t = 0; t < k; ++t)
      a[i * k + t] = float(int((i * 17 + t * 13) % 19) - 9) / 16;
  for (unsigned t = 0; t < k; ++t)
    for (unsigned j = 0; j < n; ++j)
      b[t * n + j] = float(int((t * 7 + j * 11) % 23) - 11) / 16;
  for (unsigned i = 0; i < m; ++i)
    for (unsigned j = 0; j < n; ++j) {
      int sum = 0;
      for (unsigned t = 0; t < k; ++t)
        sum += int(a[i * k + t] * 16) * int(b[t * n + j] * 16);
      expected[i * n + j] = float(sum) / 256;
    }
  TensorValue left{take(runtime.upload(a), "left"), NumericType::Float32},
      right{take(runtime.upload(b), "right"), NumericType::Float32};
  left.rank = right.rank = 2;
  left.dimensions = {m, k};
  right.dimensions = {k, n};
  ProgramKernel schema;
  schema.kernel = kernel;
  schema.type = NumericType::Float32;
  schema.tensorTypes = {NumericType::Float32, NumericType::Float32};
  const std::array<ProgramInstruction, 1> code{
      {{NumericProgramKind::Matmul, schema, {0, 1}, {}, {}, 2}}};
  auto plan = take(Program::prepare(runtime, 2, code, 2, NumericType::Float32, false), "prepare");
  auto invocation = take(plan.start(runtime, std::array{left, right}, {}), "start");
  auto before = runtime.dispatchStats();
  unsigned stages = 0;
  while (!invocation.finished()) {
    auto dispatch = take(invocation.next(runtime), "next");
    if (!dispatch)
      continue;
    auto result = take(runtime.execute(dispatch->kernel, dispatch->inputs, dispatch->parameters,
                                       dispatch->outputBytes, dispatch->groups),
                       "dispatch");
    check(bool(invocation.accept(runtime, std::move(result))), "accept");
    ++stages;
  }
  auto result = take(invocation.result(), "result");
  check(result.rank == 2 && result.dimensions[0] == m && result.dimensions[1] == n,
        "matrix result geometry");
  check(take(runtime.download(result.data), "output") == expected,
        "independent exact matrix reference");
  check(take(runtime.download(left.data), "left immutable") == a &&
            take(runtime.download(right.data), "right immutable") == b,
        "immutable operands");
  auto after = runtime.dispatchStats();
  check(stages == 1, "row-major CUDA product needs one GPU stage");
  check(after.nativeMatmuls - before.nativeMatmuls == unsigned(native) &&
            after.fixedMatmuls - before.fixedMatmuls == unsigned(!native),
        "actual native/fixed dispatch route");
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected native kernel with fixed-tree companion");
    auto runtime = take(Runtime::create(), "runtime");
    auto kernel = take(runtime->loadMatmulKernel(argv[1]), "native matrix");
    check(kernel.tiledMatmul() && !kernel.rightTransposedMatmul(), "native row-major contract");
    runCase(*runtime, kernel, 128, 256, 32, true);
    runCase(*runtime, kernel, 129, 128, 16, false);
    runCase(*runtime, kernel, 128, 128, 17, false);
    runCase(*runtime, kernel, 3, 5, 0, false);
    runCase(*runtime, kernel, 0, 5, 3, false);
    check(!runtime->retainedBytes(), "matrix storage retired");
    auto limited = take(Runtime::create({.enableTiledMatmul = false}), "capability ceiling");
    auto fixed = take(limited->loadMatmulKernel(argv[1]), "fixed GPU companion");
    check(!fixed.tiledMatmul(), "ceiling selects native-arithmetic fixed kernel");
    runCase(*limited, fixed, 128, 128, 16, false);
    check(!limited->retainedBytes(), "companion cleanup");
    std::cout
        << "PASS: imported full-float32 row-major matmul; exact results, one stage, tails/odd "
           "K/empty/K0 GPU companions, capability ceiling and actual dispatch counters\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
