#include "kalandra/Continuation.h"
#include "kalandra/Diagnostics.h"
#include "kalandra/PackageInstall.h"
#include "kalandra/Project.h"
#include "kalandra/Version.h"
#include <algorithm>
#include <iostream>
#include <span>
#include <string_view>

int main(int argc, char **argv) {
  if (kalandra::printVersion(argc, argv, "kalandra-check"))
    return 0;
  const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));
  bool dump = false, dumpPlan = false, json = false, selectedJson = false, fromStdin = false;
  // Select the response protocol before parsing errors, while respecting option values.
  for (std::size_t i = 1; i < arguments.size(); ++i) {
    const std::string_view arg = arguments[i];
    if ((arg == "--project" || arg == "--package") && i + 1 < arguments.size())
      ++i;
    else if (arg == "--diagnostics=json")
      json = true;
  }
  const auto failure = [&](std::string code, std::string message, int status,
                           std::string_view file = {}) {
    if (json) {
      const kalandra::Diagnostic diagnostic{std::move(code), std::move(message), {}};
      std::cout << kalandra::renderDiagnosticsJson(nullptr, {&diagnostic, 1}, {}, file);
    } else
      std::cerr << message << '\n';
    return status;
  };
  kalandra::CheckOptions options;
  const char *path = nullptr;
  std::string projectPath;
  std::optional<kalandra::NativeProject> project;
  std::vector<kalandra::ModulePackage> packages;
  const auto usage = [&] {
    return failure("CLI001",
                   "usage: kalandra-check [--diagnostics=json] [--stdin] [--placement] "
                   "[--dump-ir|--dump-plan] [--package name=path.k ...] "
                   "<source.k | --project manifest.toml>",
                   2);
  };
  for (std::size_t i = 1; i < arguments.size(); ++i) {
    const std::string_view arg = arguments[i];
    if (arg == "--diagnostics=json") {
      if (selectedJson)
        return usage();
      selectedJson = true;
    } else if (arg == "--stdin") {
      if (fromStdin)
        return usage();
      fromStdin = true;
    } else if (arg == "--project") {
      if (i + 1 >= arguments.size() || !projectPath.empty() ||
          std::string_view(arguments[i + 1]).empty())
        return usage();
      projectPath = arguments[++i];
    } else if (arg == "--package") {
      if (i + 1 >= arguments.size())
        return usage();
      auto package = kalandra::parseModulePackage(arguments[++i]);
      if (!package) {
        return failure("CLI001", package.error(), 2);
      }
      packages.push_back(std::move(*package));
    } else if (!path && arg == "--dump-ir" && !dump && !dumpPlan)
      dump = true;
    else if (!path && arg == "--dump-plan" && !dumpPlan && !dump)
      dumpPlan = true;
    else if (!path && arg == "--placement" && !options.placement)
      options.placement = true;
    else if (path || arg.starts_with("--"))
      return usage();
    else
      path = arguments[i];
  }
  if (json && (dump || dumpPlan))
    return failure("CLI001", "--diagnostics=json cannot be combined with --dump-ir or --dump-plan",
                   2);
  if (!projectPath.empty()) {
    if (path)
      return usage();
    auto loaded = kalandra::loadNativeProject(projectPath);
    if (!loaded) {
      return failure("PRJ001", loaded.error(), 1, projectPath);
    }
    project = std::move(*loaded);
    path = project->entry.c_str();
    packages.insert(packages.end(), project->packages.begin(), project->packages.end());
  }
  if (!path)
    return usage();
  std::vector<kalandra::SourceOverlay> overlays;
  if (fromStdin) {
    std::string text;
    const auto maximum = kalandra::SyntaxLimits{}.maxSourceBytes - 1;
    char buffer[8192];
    while (std::cin.read(buffer, sizeof(buffer)) || std::cin.gcount()) {
      const auto count = static_cast<std::size_t>(std::cin.gcount());
      if (count > maximum - text.size())
        return failure("IO001", "stdin source exceeds the configured size limit", 1, path);
      text.append(buffer, count);
    }
    if (!std::cin.eof())
      return failure("IO001", "cannot read stdin source", 1, path);
    overlays.push_back({path, std::move(text)});
  }
  std::optional<kalandra::InstalledNativeProject> managed;
  if (project && !project->dependencies.empty()) {
    auto loaded = kalandra::loadInstalledNativeProject(*project);
    if (!loaded)
      return failure("PRJ001", loaded.error(), 1, projectPath);
    managed = std::move(*loaded);
    managed->nodes[0].localPackages = packages;
  }
  auto syntax = managed ? kalandra::loadPackageModules(managed->nodes, {}, {}, overlays)
                        : kalandra::loadModules(path, {}, {}, packages, overlays);
  if (!syntax) {
    return failure("IO001", std::string(path) + ": " + syntax.error(), 1, path);
  }
  auto result = kalandra::check(std::move(*syntax), {}, options);
  if (!result.ok()) {
    if (json) {
      // Loader preflight failures can carry an empty synthetic source for text
      // rendering; they do not provide a trustworthy editor range.
      const bool synthetic = result.source && result.source->text().empty() &&
                             std::ranges::all_of(result.diagnostics, [](const auto &d) {
                               return d.code.starts_with("M");
                             });
      std::cout << kalandra::renderDiagnosticsJson(synthetic ? nullptr : result.source.get(),
                                                   result.diagnostics, {},
                                                   result.source ? result.source->path() : "");
    } else
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << kalandra::renderDiagnostic(*result.source, diagnostic);
    return 1;
  }
  if (json) {
    std::cout << kalandra::renderDiagnosticsJson(
        result.source.get(), {},
        kalandra::CheckSummary{result.module->functions.size(), result.module->expressions.size()});
  } else if (dumpPlan) {
    kalandra::continuation::Options planning;
    planning.preserveCalls = options.placement;
    if (options.placement) {
      auto main = std::ranges::find_if(result.module->functions,
                                       [](const auto &f) { return f.name == "main"; });
      if (main == result.module->functions.end()) {
        std::cerr << "placement continuation inspection requires main\n";
        return 1;
      }
      planning.entry = main - result.module->functions.begin();
    }
    auto planned = kalandra::continuation::plan(*result.module, planning);
    if (!planned) {
      std::cerr << planned.error() << '\n';
      return 1;
    }
    std::cout << kalandra::continuation::dump(*planned);
  } else if (dump)
    std::cout << kalandra::dumpTypedIR(*result.module);
  else
    std::cout << "Checked " << result.module->functions.size() << " functions, "
              << result.module->expressions.size() << " typed values (no code generation)\n";
}
