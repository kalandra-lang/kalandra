#pragma once
#include "kalandra/Semantics.h"
#include <sstream>

namespace kalandra::cli {
// Semantics validates public signatures before either the header or the
// machine-code writer sees them. These spellings describe the native C ABI.
inline std::string cScalar(const ValueType &type) {
  if (type.builtin == BuiltinValue::Unit)
    return "void";
  if (type.builtin == BuiltinValue::Bool)
    return "bool";
  if (type.element == NumericType::Float32)
    return "float";
  if (type.element == NumericType::Float64)
    return "double";
  return std::string(describeNumeric(type.element)->name) + "_t";
}

inline std::expected<std::string, std::string> cHeader(const TypedModule &module) {
  std::ostringstream out;
  out << "#pragma once\n#include <stdbool.h>\n#include <stdint.h>\n\n"
         "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
  bool any = false;
  for (const auto &function : module.functions) {
    if (function.cExportName.empty())
      continue;
    any = true;
    out << cScalar(function.resultType) << ' ' << function.cExportName << '(';
    if (function.parameters.empty())
      out << "void";
    for (std::size_t n = 0; n < function.parameters.size(); ++n) {
      const auto &param = module.symbols[function.parameters[n]];
      if (n)
        out << ", ";
      out << cScalar(param.type) << " arg_" << param.name;
    }
    out << ");\n";
  }
  if (!any)
    return std::unexpected("no export \"C\" functions in this module graph");
  out << "\n#ifdef __cplusplus\n}\n#endif\n";
  return out.str();
}
} // namespace kalandra::cli
