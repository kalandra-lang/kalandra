#include "kalandra/Semantics.h"
#include <iostream>
#include <set>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("record-collections.k", text)));
}
int main() {
  try {
    for (auto element : {"int32", "uint32", "int64", "uint64", "float32", "float64"}) {
      const auto type = std::string(element);
      auto result = analyze("type Envelope:\n  data:Data\ntype Data:\n  values:[" + type +
                            "]\n  matrix:Tensor[" + type +
                            ",?,?]\nfn make()->Envelope:\n  Envelope {data:Data "
                            "{values:[1,2],matrix:tensor([[1,2],[3,4]])}}\nfn main()->" +
                            type +
                            ":\n  p=make()\n  q=p with {data:p.data with {values:[3]}}\n  "
                            "p.data.values[0]+q.data.values[0]\n");
      require(result.ok(), "collection declaration/initializer rejected: " + type);
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU record field rejected");
      require(numericCapabilityError(*result.module, NumericTarget::AppleGpu).has_value(),
              "GPU record field enabled");
      for (unsigned mutation = 0; mutation < 7; ++mutation) {
        auto forged = *result.module;
        auto &field = forged.records[1].fields[0].type;
        switch (mutation) {
        case 0:
          field.device = 0;
          break;
        case 1:
          field.task = true;
          break;
        case 2:
          field.fallible = true;
          break;
        case 3:
          field.element = static_cast<NumericType>(255);
          break;
        case 4:
          field.shape.clear();
          break;
        case 5:
          field.builtin = BuiltinValue::Record;
          field.record = 0;
          break;
        case 6:
          field.shape.resize(129);
          break;
        }
        require(numericCapabilityError(forged, NumericTarget::Cpu).has_value(),
                "forged collection record admitted");
      }
    }
    for (auto text :
         {"type Bad:\n  field:Tensor[Bool,2]\nfn main():\n  0\n",
          "type Box:\n  field:[int]\nfn main():\n  Box {field:tensor([1,2])}.field\n",
          "type Box:\n  field:Tensor[int,2]\nfn main():\n  Box {field:[1,2]}.field\n",
          "type Box:\n  field:Tensor[int,2]\nfn main():\n  Box {field:tensor([1,2,3])}.field\n",
          "type Box:\n  field:[int]\nfn main():\n  Box {field:[float32(1)]}.field\n"})
      require(!analyze(text).ok(), std::string("invalid collection field program admitted: ") + text);
    require(analyze("type Box:\n  field:[Box]\nfn main()->int:\n  value=Box {field:[]}\n  length(value.field)\n").ok(),
            "recursive collection field program rejected");
    auto shapes =
        analyze("type Box:\n  field:[int]\nfn make(values:[int])->Box:\n  Box {field:values}\nfn "
                "main():\n  a=make([1])\n  b=make([2,3])\n  length(a.field)+length(b.field)\n");
    require(shapes.ok(), "dynamic field program rejected");
    std::set<std::uint64_t> identities;
    for (const auto &expression : shapes.module->expressions)
      if (std::holds_alternative<RecordFieldRead>(expression.operation)) {
        require(!expression.type.shape[0].constant, "field kept constructor-specific shape");
        require(identities.insert(expression.type.shape[0].identity).second,
                "independent record reads shared shape identity");
      }
    require(identities.size() == 2, "shape read fixtures missing");
    std::cout << "collection record typing, shape freshness and malformed IR checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
