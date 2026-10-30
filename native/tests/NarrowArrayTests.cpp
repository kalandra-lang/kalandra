#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("narrow-arrays.k", text)));
}
int main() {
  try {
    for (const auto *name : {"int8", "uint8", "int16", "uint16"}) {
      const std::string type = name;
      const auto element = *resolveNumeric(type);
      require(numericCollectionStorageAvailable(element, NumericTarget::Cpu) &&
                  !numericArithmeticAvailable(element, NumericTarget::Cpu) &&
                  !numericCollectionStorageAvailable(element, NumericTarget::AppleGpu),
              "narrow array admission enabled tensor or GPU arithmetic");
      const std::string declarations = "type Row:\n  enabled:Bool\n  value:" + type +
                                       "\n  values:[" + type + "]\n  text:String\n";
      for (const auto &source :
           {"fn main()->[" + type + "]:\n  [1,2,3]\n", "fn main()->[" + type + "]:\n  []\n",
            "fn increment(x:" + type + ")->" + type + ":\n  x+1\nfn main()->[" + type +
                "]:\n  values:[" + type +
                "]=[1,2,3]\n  values->slice(0,2)->map(increment)->concat([4])\n",
            "fn main()->[" + type + "]:\n  map([1,2],fn(x):" + type + "(x))\n",
            declarations +
                "fn change(p:Row)->Row:\n  p with "
                "{value:p.value+1,values:map(p.values,fn(x):x+p.value)}\nfn main()->[" +
                type +
                "] ! TaskError:\n  try parallel:\n    work=spawn change(Row "
                "{enabled:true,value:1,values:[2,3],text:\"é\"})\n    first=try await work\n    "
                "again=try await work\n    again.values\n",
            declarations +
                "fn main()->[int]:\n  rows=[Row {enabled:true,value:1,values:[2,3],text:\"é\"}]\n  "
                "map(rows,fn(p):int(p.value))\n"}) {
        auto result = analyze(source);
        if (!result.ok())
          for (const auto &d : result.diagnostics)
            std::cerr << renderDiagnostic(*result.source, d);
        require(result.ok(), "narrow array/record source rejected: " + source);
        require(bool(continuation::plan(*result.module)), "narrow array worker planning failed");
        require(bool(numericCapabilityError(*result.module, NumericTarget::AppleGpu)),
                "narrow GPU storage enabled");
#ifndef KALANDRA_NARROW_ARRAY_FRONTEND_ONLY
        auto native = lowerNative(*result.module);
        require(bool(native), native ? "" : native.error());
#endif
      }
      for (const auto &source :
           {"fn multiply(x:Tensor[" + type + ",?])->Tensor[" + type + ",?,?]:\n  matmul(x,x)\n",
            "fn main():\n  " + type + "([1])\n",
            "fn main()->[" + type + "]:\n  values=[1,2]\n  values\n",
            "fn main():\n  values:[" + type + "]=[1,2]\n  values+values\n",
            "fn main():\n  values:[" + type + "]=[1,2]\n  values[2]\n",
            "fn main():\n  values:[" + type + "]=[1,2]\n  concat(values,[int(1)])\n"})
        require(!analyze(source).ok(), "invalid narrow array program accepted: " + source);
    }
    std::cout << "Narrow arrays/records: four exact element widths, maps, captures, workers, "
                 "strict tensor-rank/GPU and no-promotion gates passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
