#include "kalandra/Formatter.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace {
void require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
FormatResult format(std::string text, SyntaxLimits limits = {}) {
  return formatSource(std::make_shared<const SourceFile>("format.k", std::move(text)), limits);
}
void expect(const std::string &before, const std::string &expected) {
  auto result = format(before);
  require(result.text && *result.text == expected, "unexpected formatting:\n" + before +
                                                       "\nreceived:\n" +
                                                       result.text.value_or("<rejected>"));
  auto twice = format(*result.text);
  require(twice.text == result.text, "formatting is not idempotent");
}
} // namespace
int main(int argc, char **argv) {
  try {
    expect("fn main( ):\n    value:int=1+2*3  \n    value\n\n",
           "fn main():\n  value: int = 1 + 2 * 3\n  value\n");
    expect("# Heading  \r\nfn main( ): # note  \r\n    # body  \r\n    \"😀 # string  \\\"\"  # "
           "tail  \r\n",
           "# Heading  \nfn main():  # note  \n  # body  \n  \"😀 # string  \\\"\"  # tail  \n");
    expect("fn main():\n    - 1 + ( - 2 )\n", "fn main():\n  -1 + (-2)\n");
    require(!format("fn main():\n    ! true\n").text, "unsupported negation repaired");
    expect("fn main():\n    not(true)\n", "fn main():\n  not (true)\n");
    expect("fn main():\n    1 .field\n", "fn main():\n  1 .field\n");
    expect("fn call( value : [ int ] ) -> int:\n    value [ 0 ]\n",
           "fn call(value: [int]) -> int:\n  value[0]\n");
    expect("type Point:\n    x:int\nfn main():\n    Point{ x:1 }\n",
           "type Point:\n  x: int\nfn main():\n  Point { x: 1 }\n");
    expect("fn main():\n    values=[\n        1, # first\n        2\n    ]\n    values\n",
           "fn main():\n  values = [\n    1,  # first\n    2\n  ]\n  values\n");
    expect("\r\n   \r\n", "");
    expect("# only", "# only\n");
    expect("fn main():\n  1", "fn main():\n  1\n");
    require(!format("fn main(\n").text, "invalid source formatted");
    require(!format("fn main():\n\t1\n").text, "tabs repaired instead of rejected");
    require(!format(std::string("# ") + char(0xff)).text, "invalid UTF-8 formatted");
    SyntaxLimits limits;
    limits.maxSourceBytes = 10;
    require(!format("fn main():\n  1\n", limits).text, "source size limit ignored");
    const std::string grows = "fn main():\n  1+2\n";
    limits.maxSourceBytes = grows.size();
    require(!format(grows, limits).text, "output size limit ignored");
    expect("fn main():\n    devices.gpu( 1 )\n", "fn main():\n  devices.gpu(1)\n");
    require(!format("fn main():\n  devices . gpu(1)\n").text, "selector source spelling changed");
    require(argc == 3, "missing corpus roots");
    std::size_t accepted = 0, rejected = 0;
    for (int root = 1; root < argc; ++root)
      for (const auto &entry : std::filesystem::recursive_directory_iterator(argv[root])) {
        if (!entry.is_regular_file() || entry.path().extension() != ".k")
          continue;
        auto source = loadSource(entry.path().string());
        require(source.has_value(), "cannot read corpus fixture");
        auto original = parse(*source);
        auto result = formatSource(*source);
        if (!original.ok()) {
          require(!result.text, "invalid fixture formatted: " + entry.path().string());
          ++rejected;
          continue;
        }
        require(result.text.has_value(), "valid fixture rejected: " + entry.path().string());
        auto twice = format(*result.text);
        require(twice.text == result.text, "non-idempotent fixture: " + entry.path().string());
        ++accepted;
      }
    require(accepted >= 100, "unexpectedly small corpus");
    std::cout << "Native formatter: " << accepted << " accepted fixtures; " << rejected
              << " invalid fixtures rejected\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
