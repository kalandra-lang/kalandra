#include "../metal-source/MatmulEpilogue.h"
#include <cstdlib>
#include <iostream>
#include <source_location>
static void check(bool value, std::source_location where = std::source_location::current()) {
  if (!value) {
    std::cerr << "Epilogue check failed: " << where.line() << "\n";
    std::abort();
  }
}
static std::string map(unsigned tensors, unsigned scalars) {
  std::string text = "#include <metal_stdlib>\nusing namespace metal;\nstruct F { float _m0[1]; "
                     "};\nstruct P { uint _m0[" +
                     std::to_string(scalars + 1) + "]; };\nkernel void kalandra_elementwise_v2(";
  for (unsigned i = 0; i < tensors + 2; ++i)
    text += "device " + std::string(i == tensors + 1 ? "P" : "F") +
            "& kalandra_elementwise_v2_arg_" + std::to_string(i) + " [[buffer(" +
            std::to_string(i) + ")]], ";
  text += "uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]], uint3 gl_WorkGroupID "
          "[[threadgroup_position_in_grid]])\n{\nuint ix = (gl_WorkGroupID.x * 64u) + "
          "gl_LocalInvocationID.x;\nif (ix < kalandra_elementwise_v2_arg_" +
          std::to_string(tensors + 1) + "._m0[0u]) {\nfloat value = ";
  for (unsigned i = 0; i < tensors; ++i)
    text += (i ? "+" : "") + std::string("kalandra_elementwise_v2_arg_") + std::to_string(i) +
            "._m0[ix]";
  return text + ";\nkalandra_elementwise_v2_arg_" + std::to_string(tensors) +
         "._m0[ix] = value;\n}\n}\n";
}
int main() {
  for (unsigned count : {1u, 2u, 10u, 11u, 27u})
    for (unsigned scalars : {0u, 1u, 7u}) {
      auto fused = matmulEpilogue(map(count, scalars), count, scalars);
      check(bool(fused));
      check(fused->find("kalandra_matmul_fused_register16") != std::string::npos);
      check(fused->find("kalandra_matmul_fused_grouped16") != std::string::npos);
      check(fused->find("kalandra_matmul_fused_skinny32") != std::string::npos);
      check(fused->find("kalandra_matmul_fused_tiled") != std::string::npos);
      check(fused->find("return value;") != std::string::npos);
      check(fused->find("kalandra_matrix_value") != std::string::npos);
    }
  auto replaceAll = [](std::string &text, const std::string &from, const std::string &to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
      text.replace(at, from.size(), to);
      at += to.size();
    }
  };
  for (unsigned scalars : {0u, 1u, 7u}) {
    auto typed = map(1, scalars);
    replaceAll(typed, "kalandra_elementwise_v2", "kalandra_numeric_v1_float32");
    replaceAll(typed, "float _m0[1]", "uint _m0[1]");
    replaceAll(typed, "struct P { uint _m0[" + std::to_string(scalars + 1) + "]",
               "struct P { uint _m0[" + std::to_string(scalars + 2) + "]");
    replaceAll(typed, "float value = ", "uint value = ");
    auto fused = matmulEpilogue(typed, 1, scalars, true);
    check(bool(fused));
    check(fused->find("as_type<uint>(kalandra_matrix_value)") != std::string::npos);
    check(fused->find("return as_type<float>(value)") != std::string::npos);
    check(!matmulEpilogue(typed, 1, scalars, false));
    auto badIndex = typed;
    replaceAll(badIndex, "arg_0._m0[ix]", "arg_0._m0[ix+1]");
    check(!matmulEpilogue(badIndex, 1, scalars, true));
    auto badWire = typed;
    replaceAll(badWire, "uint _m0[1]", "float _m0[1]");
    check(!matmulEpilogue(badWire, 1, scalars, true));
    auto badControl = typed;
    replaceAll(badControl, "uint value = ", "if(true) { uint value = ");
    check(!matmulEpilogue(badControl, 1, scalars, true));
  }
  auto valid = map(2, 1);
  check(!matmulEpilogue(valid, 0, 1));
  check(!matmulEpilogue(valid, 28, 1));
  check(!matmulEpilogue(valid, 2, 0));
  check(!matmulEpilogue(valid, 1, 1));
  for (auto [from, to] :
       {std::pair{std::string("arg_0._m0[ix]"), std::string("arg_0._m0[ix+1]")},
        {"float value = ", "if(true) { float value = "},
        {"arg_2._m0[ix] = value;", "arg_2._m0[ix] = value; arg_2._m0[ix] = value;"},
        {"float _m0[1]", "uint _m0[1]"},
        {"64u", "128u"}}) {
    auto bad = valid;
    auto at = bad.find(from);
    check(at != std::string::npos);
    bad.replace(at, from.size(), to);
    check(!matmulEpilogue(bad, 2, 1));
  }
  check(!matmulEpilogue(std::string(16385, ' ') + valid, 2, 1));
  std::cout << "Epilogue lifting: canonical arities, scalar blocks, physical binding limit and "
               "fail-closed indexing/control/store/type/size checks passed\n";
}
