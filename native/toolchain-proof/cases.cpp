#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
using Matrix = std::array<float, 6>;
struct InputCase {
  std::string name;
  Matrix a;
  Matrix b;
};
float tree(std::vector<float> values) {
  if (values.empty())
    return 0;
  while (values.size() > 1) {
    std::vector<float> next;
    for (std::size_t i = 0; i < values.size(); i += 2)
      next.push_back(i + 1 < values.size() ? values[i] + values[i + 1] : values[i]);
    values = std::move(next);
  }
  return values.front();
}
// Separate scalar implementation: the MLIR fixture neither calls this code nor
// shares its IR construction. Keep contraction disabled for this reference.
float reference(const Matrix &a, const Matrix &b) {
  std::vector<float> outputs;
  for (std::size_t row = 0; row < 2; ++row)
    for (std::size_t col = 0; col < 2; ++col) {
      std::vector<float> products;
      for (std::size_t k = 0; k < 3; ++k)
        products.push_back(a[row * 3 + k] * b[k * 2 + col]);
      outputs.push_back(tree(std::move(products)));
    }
  return tree(std::move(outputs));
}
std::expected<std::size_t, std::string> writeCases(const std::filesystem::path &directory) {
  const auto filled = [](float value) {
    Matrix matrix;
    matrix.fill(value);
    return matrix;
  };
  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::vector<InputCase> cases{
      {"canonical", {1, 2, 3, 4, 5, 6}, {1, 2, 3, 4, 5, 6}},
      {"changed", {2, 4, 6, 8, 10, 12}, {1, 2, 3, 4, 5, 6}},
      {"negative-zero", filled(-0.0f), filled(1)},
      {"subnormal", filled(std::numeric_limits<float>::denorm_min()), filled(1)},
      {"infinity", filled(inf), filled(1)},
      {"nan", filled(nan), filled(1)},
      {"product-rounding",
       {1 + std::ldexp(1.0f, -23), 1, 0, 0, 0, 0},
       {1 - std::ldexp(1.0f, -23), 0, -1, 0, 0, 0}},
      {"odd-contraction-tree", {16777216, 1, -16777216, 0, 0, 0}, {1, 0, 1, 0, 1, 0}},
      {"balanced-output-tree", {1, 0, 0, 0, 1, 0}, {33554432, 1, -33554432, 1, 0, 0}},
  };
  std::uint32_t seed = 0x4b414c41;
  const auto random = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>((static_cast<double>(seed % 20001) - 10000) / 37);
  };
  for (int i = 0; i < 40; ++i) {
    InputCase entry{"generated-" + std::to_string(i), {}, {}};
    for (auto &value : entry.a)
      value = random();
    for (auto &value : entry.b)
      value = random();
    cases.push_back(std::move(entry));
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error)
    return std::unexpected("cannot create case directory: " + error.message());
  std::ofstream manifest(directory / "cases.txt");
  if (!manifest)
    return std::unexpected("cannot create case manifest");
  for (const auto &entry : cases) {
    const auto result = reference(entry.a, entry.b);
    const auto bits = std::bit_cast<std::uint32_t>(result);
    if ((entry.name == "canonical" && result != 163) ||
        (entry.name == "changed" && result != 326) ||
        (entry.name == "negative-zero" && bits != 0x80000000u) ||
        ((entry.name == "product-rounding" || entry.name == "odd-contraction-tree" ||
          entry.name == "balanced-output-tree") &&
         bits != 0))
      return std::unexpected("invalid numerical witness: " + entry.name);
    std::ofstream binary(directory / (entry.name + ".bin"), std::ios::binary);
    binary.write(reinterpret_cast<const char *>(entry.a.data()), sizeof(Matrix));
    binary.write(reinterpret_cast<const char *>(entry.b.data()), sizeof(Matrix));
    binary.close();
    if (!binary)
      return std::unexpected("cannot write input: " + entry.name);
    manifest << entry.name << '|' << bits << '|' << std::isnan(result) << '\n';
  }
  manifest.close();
  if (!manifest)
    return std::unexpected("cannot finish case manifest");
  return cases.size();
}
} // namespace

int main(int argc, char **argv) {
  const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));
  if (arguments.size() != 2)
    return 2;
  auto result = writeCases(arguments[1]);
  if (!result) {
    std::cerr << result.error() << '\n';
    return 1;
  }
  std::cout << *result << " reference cases generated\n";
}
