#pragma once
#include "NumericShader.h"
#include "kalandra/NumericKernelName.h"
#include <sstream>
namespace kalandra {
// Arena words preserve raw lane bits. The normal map allocation provides the
// exact byte extent of the public tensor, including odd-width transfers.
inline NumericShader deviceTensorResultMap(NumericType element) {
  const auto bits = describeNumeric(element)->bits;
  const std::array input{NumericType::UInt64};
  const auto name = numericKernelName(element, input, std::span<const NumericType>{});
  std::ostringstream source;
  source << "module attributes {gpu.container_module, spirv.target_env = "
            "#spirv.target_env<#spirv.vce<v1.0, [Shader, Int64";
  if (bits == 8)
    source << ", Int8, StorageBuffer8BitAccess";
  if (bits == 16)
    source << ", Int16, StorageBuffer16BitAccess";
  source << "], [SPV_KHR_storage_buffer_storage_class";
  if (bits == 8)
    source << ", SPV_KHR_8bit_storage";
  if (bits == 16)
    source << ", SPV_KHR_16bit_storage";
  const auto output = "memref<?xi" + std::to_string(bits) + ">";
  source << "]>, #spirv.resource_limits<>>} { gpu.module @kernels { gpu.func @" << name
         << "(%in:memref<?xi64>,%out:" << output << ",%params:memref<2xi32>)"
            " kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size=[64,1,1]>} {\n"
            "%block=gpu.block_id x\n%thread=gpu.thread_id x\n%width=gpu.block_dim x\n"
            "%base=arith.muli %block,%width:index\n%i=arith.addi %base,%thread:index\n"
            "%zero=arith.constant 0:index\n%count=memref.load %params[%zero]:memref<2xi32>\n"
            "%n=arith.index_castui %count:i32 to index\n"
            "%active=arith.cmpi ult,%i,%n:index\nscf.if %active {\n"
            "%word=memref.load %in[%i]:memref<?xi64>\n";
  if (bits != 64)
    source << "%lane=arith.trunci %word:i64 to i" << bits << "\n";
  source << "memref.store " << (bits == 64 ? "%word" : "%lane") << ",%out[%i]:" << output
         << "\n}\ngpu.return\n}}}\n";
  return NumericShader{element, source.str(), {0}, {}, false, false, {NumericType::UInt64}, {},
                       {0}, false, {}, {}, true};
}
} // namespace kalandra
