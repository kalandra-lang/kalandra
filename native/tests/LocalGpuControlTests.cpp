#include "../gpu-source/Supervisor.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    const std::string kernel = "fn twice(x:Tensor[f32,?])->Tensor[f32,?]:\n  return x+x\n";
    const std::string host = "fn main()->Tensor[f32,3] ! TaskError:\n"
                             "  i=0\n  while i<3:\n    try parallel:\n"
                             "      gpu=devices.gpu()\n"
                             "      x:Tensor[f32,3]=tensor([1,2,3])\n"
                             "      input=try await transfer(x,gpu)\n"
                             "      result=try await spawn twice(input) on gpu\n"
                             "      output=try await transfer(result,devices.cpu())\n"
                             "      i=i+1\n      output\n"
                             "  return tensor([4,5,6])\n";
    for (auto backend : {GpuBackend::Metal, GpuBackend::Vulkan, GpuBackend::Cuda}) {
      auto emit = [&](const std::string &text) {
        auto source = std::make_shared<SourceFile>("gpu-locals.k", text);
        auto parsed = parse(source);
        require(parsed.ok(), "GPU local-control fixture did not parse");
        return emitGpuSupervisor(std::move(parsed.module), ArithmeticMode::Software, false,
                                 backend);
      };
      auto result = emit(kernel + host);
      require(bool(result), "host loop rejected: " + (result ? "" : result.error().message));
      const auto &cpp = result->at("host.cpp");
      require(cpp.find("task.releaseGroup(") != std::string::npos,
              "completed mixed groups retain iteration resources");
      require(cpp.find("task.context().poll()") != std::string::npos,
              "mixed loop lost cancellation checkpoint");
      auto mutation = emit("fn twice(x:Tensor[f32,?])->Tensor[f32,?]:\n"
                           "  scale:f32=1\n  scale=2\n  return x*scale\n" +
                           host);
      require(bool(mutation), "device scalar mutation rejected");
      auto loop = emit("fn twice(x:Tensor[f32,?])->Tensor[f32,?]:\n"
                       "  while false: 0\n  return x+x\n" +
                       host);
      require(bool(loop), "device statement loop rejected");
      auto arrayWrite = emit("fn twice(x:Tensor[f32,?])->Tensor[f32,?]:\n"
                             "  local=[1,2]\n  local[0]=3\n  return x+x\n" +
                             host);
      require(bool(arrayWrite), "device local array write with tensor result rejected");
      auto arraySort = emit("fn twice(x:Tensor[f32,?])->Tensor[f32,?]:\n"
                            "  local=sorted([2,1])\n  return x+x\n" +
                            host);
      require(bool(arraySort), "device array sorting rejected");
      for (const auto *program : {&mutation, &loop, &arrayWrite, &arraySort})
        require((*program)->at("host.cpp").find("NumericInstructionKind::DeviceControl") !=
                    std::string::npos,
                "tensor control was routed through host computation");
      auto hostArray =
          emit(kernel + std::string("fn main()->Tensor[f32,3] ! TaskError:\n"
                                    "  local:[f32]=[1,2,3]\n  local[0]=f32(3)\n"
                                    "  return try parallel:\n    gpu=devices.gpu()\n"
                                    "    x:Tensor[f32,3]=tensor(local)\n"
                                    "    input=try await transfer(x,gpu)\n"
                                    "    result=try await spawn twice(input) on gpu\n"
                                    "    try await transfer(result,devices.cpu())\n"));
      require(bool(hostArray),
              "mixed host array rejected: " + (hostArray ? "" : hostArray.error().message));
      require(hostArray->at("host.cpp").find("HostArrayScopes arrayScopes") != std::string::npos &&
                  hostArray->contains("cpu-steps.mlir"),
              "mixed host array bypassed native ownership lowering");
      const std::string scalar =
          "fn work(n:i64)->i64:\n  total=0\n  for i in range(n):\n"
          "    if i==5:\n      continue\n    if i>20:\n      break\n"
          "    total=total+i\n  return total\n"
          "fn main()->i64 ! TaskError:\n  return try parallel:\n    gpu=devices.gpu()\n"
          "    try await spawn work(100) on gpu\n";
      auto deviceLoop = emit(scalar);
      require(bool(deviceLoop), "checkpointed scalar loop rejected: " +
                                    (deviceLoop ? "" : deviceLoop.error().message));
      require(deviceLoop->at("host.cpp").find("NumericInstructionKind::ScalarControl") !=
                  std::string::npos,
              "device loop was routed through a host continuation");
      bool checkpoint = false;
      for (const auto &[name, artifact] : *deviceLoop)
        if (name.ends_with(".mlir"))
          checkpoint |= artifact.find("scf.while") != std::string::npos &&
                        artifact.find("memref.store") != std::string::npos;
      require(checkpoint, "device loop has no bounded resumable shader");
      const std::string recordEntry =
          "fn main()->i64 ! TaskError:\n  return try parallel:\n"
          "    gpu=devices.gpu()\n    try await spawn work(42) on gpu\n";
      auto arrays = emit("fn work(n:i64)->i64:\n  values=[1,2,3]\n"
                         "  alias=values\n  alias[0]=n\n  copied=copy(values)\n"
                         "  total=0\n  for value in copied:\n    total=total+value\n  return total\n" +
                         recordEntry);
      require(bool(arrays), "device-local array lowering rejected: " +
                                (arrays ? "" : arrays.error().message));
      require(arrays->at("host.cpp").find("NumericInstructionKind::DeviceControl") !=
                  std::string::npos,
              "device arrays were routed through host computation");
      auto record = emit("type Row:\n  value:i64\n  enabled:bool\n"
                         "fn work(n:i64)->i64:\n  row=Row {value:n,enabled:true}\n"
                         "  return if row.enabled:row.value\n  else:0\n" +
                         recordEntry);
      require(bool(record),
              "plain device record rejected: " + (record ? "" : record.error().message));
      require(record->at("host.cpp").find("NumericInstructionKind::ScalarControl") !=
                  std::string::npos,
              "device record was routed through host evaluation");
      auto ownedRecord = emit("type Row:\n  values:[i64]\n"
                              "fn work(n:i64)->i64:\n  row=Row {values:[n]}\n  return row.values[0]\n" +
                              recordEntry);
      require(bool(ownedRecord), "device owned record rejected");
      require(ownedRecord->at("host.cpp").find("NumericInstructionKind::DeviceControl") !=
                  std::string::npos,
              "owned record bypassed device storage");
      auto transportedRecord =
          emit("type Row:\n  value:i64\nfn work(n:i64)->Row:\n  return Row {value:n}\n"
               "fn main()->i64 ! TaskError:\n  return try parallel:\n"
               "    gpu=devices.gpu()\n    row=try await spawn work(42) on gpu\n    row.value\n");
      require(bool(transportedRecord), "device record result transport rejected");
      auto readOnly =
          emit(kernel + std::string("fn write(values:[i64])->i64:\n  values[0]=3\n  return values[0]\n") +
               host);
      require(!readOnly && readOnly.error().message.find("read-only") != std::string::npos,
              "mixed host admitted writes through a read-only parameter");
      auto shared = emit(
          kernel + std::string("fn read(values:[i64])->i64:\n  return values[0]\n"
                               "fn main()->Tensor[f32,3] ! TaskError:\n  return try parallel:\n"
                               "    local=[1,2]\n    alias=local\n"
                               "    pending=spawn read(local) on devices.cpu()\n    alias[0]=3\n"
                               "    observed=try await pending\n    gpu=devices.gpu()\n"
                               "    x:Tensor[f32,3]=tensor([1,2,3])\n"
                               "    input=try await transfer(x,gpu)\n"
                               "    result=try await spawn twice(input) on gpu\n"
                               "    try await transfer(result,devices.cpu())\n"));
      require(!shared && shared.error().message.find("may still read") != std::string::npos,
              "mixed host admitted an alias write while a task reads its backing");
    }
    std::cout << "Metal/Vulkan/CUDA: host/device loops, owned values, group release and read-only gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
