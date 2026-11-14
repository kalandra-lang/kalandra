#include "../lib/IteratorFusion.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("expected fixture directory");
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    const auto verify = [&](std::string text, bool expected, bool native = false) {
      const auto path = root / "main.k";
      {
        std::ofstream file(path);
        file << text;
      }
      auto loaded = loadModules(path.string());
      if (!loaded)
        throw std::runtime_error(loaded.error());
      auto checked = check(std::move(*loaded));
      for (const auto &diagnostic : checked.diagnostics)
        std::cerr << renderDiagnostic(*checked.source, diagnostic);
      if (!checked.ok())
        throw std::runtime_error("fusion fixture failed source checking");
      auto &module = *checked.module;
      const auto entry =
          std::ranges::find_if(module.functions, [](const auto &fn) { return fn.name == "main"; });
      if (entry == module.functions.end())
        throw std::runtime_error("missing fixture main");
      const auto id = static_cast<FunctionId>(entry - module.functions.begin());
      const auto before = module.functions[id].body.front().value;
      fuseLocalIterators(module);
      const auto &fn = module.functions[id];
      const auto hasCursor = std::ranges::any_of(
          fn.expressions, [&](auto id) { return isIterator(module, module.expressions[id].type); });
      if (hasCursor == expected || (!expected && before != fn.body.front().value))
        throw std::runtime_error("incorrect local iterator fusion admission");
      if (native) {
        auto planned = continuation::plan(module, {.valueScopes = true});
        if (!planned || std::ranges::any_of(planned->plans[id]->code, [](const auto &op) {
              return op.code == continuation::Code::Jump || op.code == continuation::Code::Branch;
            }))
          throw std::runtime_error("ordinary fused traversal retained per-item continuation steps");
      }
    };
    verify("fn main()->i64:\n  values=for i in range(10):\n    yield i*i\n"
           "  total=0\n  for value in values:\n    total=total+value\n  return total\n",
           true, true);
    verify("from \"std/iter\" import iter,range,filter,map as mapped\n"
           "fn main()->i64:\n  values=mapped(filter(iter(range(10)),"
           "fn(x:i64)->bool:return x%2==0),fn(x:i64)->i64:return x*x)\n"
           "  total=0\n  for value in values:\n    total=total+value\n  return total\n",
           true, true);
    verify("from \"std/iter\" import Iterator\n"
           "fn values()->Iterator[i64,never]:\n  return for i in range(10):\n    yield i\n"
           "fn main()->i64:\n  total=0\n  for value in values():\n"
           "    total=total+value\n  return total\n",
           true, true);
    verify("fn main()->i64:\n  values=for i in range(10):\n    yield i\n"
           "  total=0\n  for value in values:\n    if value==5:\n      break\n"
           "    if value==2:\n      continue\n    total=total+value\n  return total\n",
           true, true);
    verify("from \"std/iter\" import take\nfn main()->i64:\n"
           "  values=for i in range(10):\n    yield i\n"
           "  limited=take(values,2)\n  total=0\n"
           "  for value in limited:\n    total=total+value\n  return total\n",
           true, true);
    verify("from \"std/iter\" import iter,map as mapped\nfn main()->i64:\n"
           "  data=[1,2,3]\n  values=mapped(iter(data),fn(x:i64)->i64:return x*2)\n"
           "  total=0\n  for value in values:\n    total=total+value\n  return total\n",
           true, true);
    verify("from \"std/iter\" import iter\nfn main()->i64:\n"
           "  values=iter([1,2,3])\n  total=0\n"
           "  for value in values:\n    total=total+value\n  return total\n",
           false);
    verify("from \"std/io\" import println,IoError\nfrom \"std/fmt\" import format\n"
           "fn main()->i64 ! IoError:\n  values=for i in range(10):\n    yield i\n"
           "  total=0\n  for value in values:\n    total=total+value\n"
           "  try println(format(\"{}\",total))\n  return total\n",
           true);
    verify("from \"std/iter\" import next,close\nfrom \"std/option\" import Option\n"
           "fn main()->i64:\n  values=for i in range(10):\n    yield i\n"
           "  item=next(values)\n  total=0\n  for value in values:\n"
           "    total=total+value\n  return total\n",
           false);
    verify("fn main()->i64:\n  values=for i in range(10):\n    yield [i]\n"
           "  total=0\n  for value in values:\n    total=total+value[0]\n"
           "  return total\n",
           false);
    verify("from \"std/io\" import println\nfn main()->i64:\n"
           "  values=for i in range(10):\n"
           "    ignored=println(\"advance\") catch fn(error):return ()\n    yield i\n"
           "  total=0\n  for value in values:\n    total=total+value\n  return total\n",
           false);
    std::cout << "PASS: local fusion and conservative fallback admission\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
