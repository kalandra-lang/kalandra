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
    const std::string kernel = "fn twice(x:Tensor[float32,?])->Tensor[float32,?]:\n  x+x\n";
    const std::string host = "fn main()->Tensor[float32,3] ! TaskError:\n"
                             "  i=0\n  while i<3:\n    try parallel:\n"
                             "      gpu=devices.gpu()\n"
                             "      x:Tensor[float32,3]=tensor([1,2,3])\n"
                             "      input=try await transfer(x,gpu)\n"
                             "      result=try await spawn twice(input) on gpu\n"
                             "      output=try await transfer(result,devices.cpu())\n"
                             "      i=i+1\n      output\n"
                             "  tensor([4,5,6])\n";
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
      auto mutation = emit("fn twice(x:Tensor[float32,?])->Tensor[float32,?]:\n"
                           "  scale:float32=1\n  scale=2\n  x*scale\n" +
                           host);
      require(!mutation && mutation.error().message.find("scalar helper") != std::string::npos,
              "GPU kernel admitted mutable scalar state");
      auto loop = emit("fn twice(x:Tensor[float32,?])->Tensor[float32,?]:\n"
                       "  while false: 0\n  x+x\n" +
                       host);
      require(!loop && loop.error().message.find("scalar helper") != std::string::npos,
              "GPU kernel admitted statement loops");
      auto arrayWrite = emit("fn twice(x:Tensor[float32,?])->Tensor[float32,?]:\n"
                             "  local=[1,2]\n  local[0]=3\n  x+x\n" +
                             host);
      require(!arrayWrite && arrayWrite.error().message.find("CPU execution") != std::string::npos,
              "GPU kernel admitted array element assignment");
      auto arraySort = emit("fn twice(x:Tensor[float32,?])->Tensor[float32,?]:\n"
                            "  local=sorted([2,1])\n  x+x\n" +
                            host);
      require(!arraySort && arraySort.error().message.find("CPU") != std::string::npos,
              "GPU kernel admitted CPU array sorting");
      auto hostArray =
          emit(kernel + std::string("fn main()->Tensor[float32,3] ! TaskError:\n"
                                    "  local:[float32]=[1,2,3]\n  local[0]=float32(3)\n"
                                    "  try parallel:\n    gpu=devices.gpu()\n"
                                    "    x:Tensor[float32,3]=tensor(local)\n"
                                    "    input=try await transfer(x,gpu)\n"
                                    "    result=try await spawn twice(input) on gpu\n"
                                    "    try await transfer(result,devices.cpu())\n"));
      require(bool(hostArray),
              "mixed host array rejected: " + (hostArray ? "" : hostArray.error().message));
      require(hostArray->at("host.cpp").find("HostArrayScopes arrayScopes") != std::string::npos &&
                  hostArray->contains("cpu-steps.mlir"),
              "mixed host array bypassed native ownership lowering");
      const std::string scalar =
          "fn work(n:int)->int:\n  total=0\n  for i in range(n):\n"
          "    if i==5:\n      continue\n    if i>20:\n      break\n"
          "    total=total+i\n  total\n"
          "fn main()->int ! TaskError:\n  try parallel:\n    gpu=devices.gpu()\n"
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
          "fn main()->int ! TaskError:\n  try parallel:\n"
          "    gpu=devices.gpu()\n    try await spawn work(42) on gpu\n";
      auto record = emit("type Row:\n  value:int\n  enabled:Bool\n"
                         "fn work(n:int)->int:\n  row=Row {value:n,enabled:true}\n"
                         "  if row.enabled:row.value\n  else:0\n" +
                         recordEntry);
      require(bool(record),
              "plain device record rejected: " + (record ? "" : record.error().message));
      require(record->at("host.cpp").find("NumericInstructionKind::ScalarControl") !=
                  std::string::npos,
              "device record was routed through host evaluation");
      auto ownedRecord = emit("type Row:\n  values:[int]\n"
                              "fn work(n:int)->int:\n  row=Row {values:[n]}\n  row.values[0]\n" +
                              recordEntry);
      require(!ownedRecord, "device record admitted host array pointers without storage");
      auto transportedRecord =
          emit("type Row:\n  value:int\nfn work(n:int)->Row:\n  Row {value:n}\n"
               "fn main()->int ! TaskError:\n  try parallel:\n"
               "    gpu=devices.gpu()\n    row=try await spawn work(42) on gpu\n    row.value\n");
      require(!transportedRecord && transportedRecord.error().message.find("aggregate transport") !=
                                        std::string::npos,
              "host bridge record-result gate: " + (transportedRecord
                                                        ? std::string("unexpectedly admitted")
                                                        : transportedRecord.error().message));
      auto readOnly =
          emit(kernel + std::string("fn write(values:[int])->int:\n  values[0]=3\n  values[0]\n") +
               host);
      require(!readOnly && readOnly.error().message.find("read-only") != std::string::npos,
              "mixed host admitted writes through a read-only parameter");
      auto shared = emit(
          kernel + std::string("fn read(values:[int])->int:\n  values[0]\n"
                               "fn main()->Tensor[float32,3] ! TaskError:\n  try parallel:\n"
                               "    local=[1,2]\n    alias=local\n"
                               "    pending=spawn read(local) on devices.cpu()\n    alias[0]=3\n"
                               "    observed=try await pending\n    gpu=devices.gpu()\n"
                               "    x:Tensor[float32,3]=tensor([1,2,3])\n"
                               "    input=try await transfer(x,gpu)\n"
                               "    result=try await spawn twice(input) on gpu\n"
                               "    try await transfer(result,devices.cpu())\n"));
      require(!shared && shared.error().message.find("may still read") != std::string::npos,
              "mixed host admitted an alias write while a task reads its backing");
    }
    std::cout << "Metal/Vulkan/CUDA: host loops, group release and kernel rejection gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
