#include "kalandra/Application.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected isolated fixture directory");
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    unsigned index = 0;
    const auto checked = [&](const std::string &text, CheckOptions options = {}) {
      const auto path = root / ("case-" + std::to_string(index++) + ".k");
      {
        std::ofstream file(path);
        file << text;
      }
      auto loaded = loadModules(path.string());
      require(bool(loaded) && loaded->ok(), "application fixture did not parse");
      auto result = check(std::move(*loaded), {}, options);
      if (!result.ok()) {
        for (const auto &diagnostic : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, diagnostic);
      }
      require(result.ok(), "application fixture did not check");
      return std::move(*result.module);
    };
    auto placed = checked("from \"std/io\" import IoError as InputError\n"
                          "from \"std/process\" import ExitStatus as Status\n"
                          "fn main()->Status ! InputError:\n  return Status {code:0}\n",
                          {.placement = true});
    require(bool(validateApplicationEntry(placed)),
            "placement checking lost imported standard error or result types");
    for (const auto &source :
         {"fn main()->unit:\n  return ()\n", "fn main(args:[String])->unit:\n  return ()\n",
          "from \"std/process\" import ExitStatus\nfn main()->ExitStatus:\n  return ExitStatus "
          "{code:42}\n",
          "from \"std/process\" import ExitStatus as Status\nfn main(args:[String])->Status:\n  "
          "return Status {code:255}\n",
          "from \"std/io\" import IoError\nfn main()->unit ! IoError:\n  return ()\n"}) {
      auto module = checked(source);
      const auto count = module.functions.size();
      const auto sourceMain =
          std::ranges::find_if(module.functions, [](const auto &fn) { return fn.name == "main"; });
      const auto parameters = sourceMain->parameters.size();
      const auto fallible = sourceMain->resultType.fallible;
      const auto info = validateApplicationEntry(module);
      require(info && info->arguments == bool(parameters),
              "shared application validation rejected entry arguments");
      require(!prepareApplication(module), "valid application rejected");
      require(module.applicationEntry && module.functions.size() == count + 1,
              "application wrapper missing");
      const auto &entry = module.functions.back();
      require(entry.name == "main" && entry.parameters.empty() &&
                  entry.resultType.element == NumericType::Int32 &&
                  entry.resultType.fallible == fallible,
              "application exit ABI changed");
      unsigned arguments = 0, calls = 0, tries = 0;
      for (auto id : entry.expressions) {
        const auto &operation = module.expressions[id].operation;
        arguments += std::holds_alternative<ApplicationArguments>(operation);
        tries += std::holds_alternative<TaskOperation>(operation);
        if (const auto *call = std::get_if<FunctionCall>(&operation)) {
          ++calls;
          require(call->arguments.size() == parameters, "application argument count changed");
        }
      }
      require(arguments == parameters && calls == 1 && tries == unsigned(fallible),
              "application changed argument/call/error evaluation");
      require(!numericCapabilityError(module, NumericTarget::Cpu),
              "application wrapper failed capability validation");
      require(bool(prepareApplication(module)), "application pass accepted duplicate wrapping");
    }
    for (const auto &source :
         {"fn main()->i64:\n  return 42\n", "fn main(args:[i64])->unit:\n  return ()\n",
          "fn main(text:String)->unit:\n  return ()\n",
          "fn main(args:[String],extra:i64)->unit:\n  return ()\n",
          "type ExitStatus:\n  code:u8\nfn main()->ExitStatus:\n  return ExitStatus {code:42}\n",
          "fn helper()->unit:\n  return ()\n"}) {
      auto module = checked(source);
      const auto functions = module.functions.size(), expressions = module.expressions.size();
      const auto result = prepareApplication(module);
      require(result && result->code == "S081" && !module.applicationEntry &&
                  module.functions.size() == functions && module.expressions.size() == expressions,
              "invalid application accepted or partially rewritten");
    }
    auto module = checked("fn main()->unit:\n  return ()\n");
    SemanticLimits limits;
    limits.maxValues = module.expressions.size();
    require(bool(prepareApplication(module, limits)),
            "application ignored the compiler value bound");
    std::cout << "PASS: nominal ExitStatus, unit exits, optional args, expected errors and atomic "
                 "entry validation\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
