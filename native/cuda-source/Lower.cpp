#include "../gpu-source/LowerConversions.h"
#include "kalandra/CudaArtifact.h"
#include "kalandra/StrictFloat.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/GPU/Pipelines/Passes.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVAttributes.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVDialect.h"
#include "mlir/IR/Verifier.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVM/NVVM/Target.h"
#include "mlir/Target/LLVMIR/Dialect/All.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include <string_view>

// Private compiler artifact ABI, not a public user-written-kernel interface.
int main(int argc, char **argv) {
  if (argc != 5) {
    llvm::errs() << "usage: kalandra-cuda-lower input.mlir output.ptx "
                    "hardware|software|hardware-with-software-fallback sm_80|sm_120\n";
    return 2;
  }
  auto policy = kalandra::parseArithmeticMode(argv[3]);
  const std::string_view chip(argv[4]);
  if (!policy || (chip != "sm_80" && chip != "sm_120"))
    return 2;
  // This process emits PTX only. Registering every host target accidentally
  // requires every LLVM backend in static Windows distributions.
  LLVMInitializeNVPTXTargetInfo();
  LLVMInitializeNVPTXTarget();
  LLVMInitializeNVPTXTargetMC();
  LLVMInitializeNVPTXAsmPrinter();
  mlir::DialectRegistry registry;
  mlir::registerAllDialects(registry);
  mlir::registerAllExtensions(registry);
  mlir::registerAllToLLVMIRTranslations(registry);
  mlir::NVVM::registerNVVMTargetInterfaceExternalModels(registry);
  mlir::MLIRContext context(registry);
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(argv[1], &context);
  if (!module || mlir::failed(mlir::verify(*module)))
    return 1;
  llvm::json::Array entries;
  kalandra::CudaArtifact envelope;
  envelope.chip = chip == "sm_80" ? 80 : 120;
  envelope.policy = *policy == kalandra::ArithmeticMode::Hardware   ? 0
                    : *policy == kalandra::ArithmeticMode::Software ? 1
                                                                    : 2;
  bool valid = true;
  module->walk([&](mlir::gpu::GPUFuncOp function) {
    if (!function.isKernel())
      return;
    auto abi = function->getAttrOfType<mlir::spirv::EntryPointABIAttr>("spirv.entry_point_abi");
    if (!abi || !abi.getWorkgroupSize() || abi.getWorkgroupSize().size() != 3) {
      function.emitError("CUDA kernel requires an explicit three-dimensional workgroup");
      valid = false;
      return;
    }
    auto group = abi.getWorkgroupSize().asArrayRef();
    if (group[0] <= 0 || group[0] > 1024 || group[1] != 1 || group[2] != 1) {
      function.emitError("unsupported CUDA workgroup shape");
      valid = false;
      return;
    }
    if (!function.getName().ends_with("_cuda_desc_v1"))
      function.setName((function.getName() + "_cuda_desc_v1").str());
    // Compiler parameter word zero is an unsigned cell count. Matmul's first
    // three words are unsigned extents. NVPTX uses 64-bit indices, unlike the
    // SPIR-V path's 32-bit indices: sign extension would corrupt >=2^31 sizes.
    llvm::SmallVector<mlir::arith::IndexCastOp> sizeCasts;
    function.walk([&](mlir::arith::IndexCastOp cast) {
      if (!cast.getIn().getType().isInteger(32) || !cast.getOut().getType().isIndex())
        return;
      auto load = cast.getIn().getDefiningOp<mlir::memref::LoadOp>();
      if (!load || load.getMemref() != function.getArguments().back() ||
          load.getIndices().size() != 1)
        return;
      auto index = load.getIndices()[0].getDefiningOp<mlir::arith::ConstantIndexOp>();
      if (index &&
          (index.value() == 0 || (function.getName().starts_with("kalandra_numeric_v1_matmul_") &&
                                  index.value() >= 0 && index.value() < 3)))
        sizeCasts.push_back(cast);
    });
    for (auto cast : sizeCasts) {
      mlir::OpBuilder builder(cast);
      auto replacement = mlir::arith::IndexCastUIOp::create(builder, cast.getLoc(),
                                                            cast.getOut().getType(), cast.getIn());
      cast.replaceAllUsesWith(replacement.getOut());
      cast.erase();
    }
    // The shared fixed-tree matmul uses function-private scratch. Its SPIR-V
    // spelling is an address-space annotation, not shared/device memory. LLVM
    // owns placement of CUDA thread-local allocas; do not erase other spaces.
    function.walk([&](mlir::memref::AllocaOp allocation) {
      auto type = allocation.getType();
      auto space = mlir::dyn_cast_or_null<mlir::spirv::StorageClassAttr>(type.getMemorySpace());
      if (space && space.getValue() == mlir::spirv::StorageClass::Function)
        allocation.getResult().setType(
            mlir::MemRefType::get(type.getShape(), type.getElementType(), type.getLayout(), {}));
    });
    envelope.entry = function.getName().str();
    envelope.threads = group[0];
    llvm::json::Array buffers;
    for (auto argument : function.getArgumentTypes()) {
      auto memory = mlir::dyn_cast<mlir::MemRefType>(argument);
      if (!memory || memory.getRank() != 1 || !memory.getLayout().isIdentity() ||
          bool(memory.getMemorySpace())) {
        function.emitError("CUDA descriptor ABI requires one-dimensional identity-layout buffers");
        valid = false;
        return;
      }
      auto element = memory.getElementType();
      if (!element.isIntOrFloat() ||
          (element.getIntOrFloatBitWidth() != 8 && element.getIntOrFloatBitWidth() != 16 &&
           element.getIntOrFloatBitWidth() != 32 && element.getIntOrFloatBitWidth() != 64)) {
        function.emitError("unsupported CUDA buffer element type");
        valid = false;
        return;
      }
      envelope.buffers.push_back(
          {element.getIntOrFloatBitWidth(), memory.isDynamicDim(0) ? -1 : memory.getDimSize(0)});
      buffers.emplace_back(
          llvm::json::Object{{"elementBits", element.getIntOrFloatBitWidth()},
                             {"extent", memory.isDynamicDim(0) ? -1 : memory.getDimSize(0)}});
    }
    if (buffers.empty() || buffers.size() > 64) {
      function.emitError("CUDA descriptor buffer count exceeds the ABI limit");
      valid = false;
      return;
    }
    entries.emplace_back(llvm::json::Object{
        {"name", function.getName()}, {"workgroupX", group[0]}, {"buffers", std::move(buffers)}});
    function.setKnownBlockSizeAttr(abi.getWorkgroupSize());
    function->removeAttr("spirv.entry_point_abi");
  });
  if (!valid || entries.size() != 1)
    return 1;
  (*module)->removeAttr("spirv.target_env");
  if (*policy != kalandra::ArithmeticMode::Hardware &&
      (mlir::failed(lowerGpuFloat64Conversions(*module)) ||
       mlir::failed(kalandra::lowerStrictGpuFloat64(
           *module, *policy == kalandra::ArithmeticMode::HardwareWithSoftwareFallback))))
    return 1;
  module->walk([&](mlir::Operation *operation) {
    const auto name = operation->getName().getStringRef();
    if (!(name.starts_with("arith.") || name.starts_with("math.")) || name == "arith.constant" ||
        name == "arith.bitcast" || name == "arith.select")
      return;
    for (auto type : operation->getOperandTypes())
      envelope.nativeFloat64 |= type.isF64();
    for (auto type : operation->getResultTypes())
      envelope.nativeFloat64 |= type.isF64();
  });
  if (envelope.policy && envelope.nativeFloat64) {
    llvm::errs() << "CUDA software artifact retains native float64 arithmetic\n";
    return 1;
  }
  mlir::gpu::GPUToNVVMPipelineOptions options;
  options.cubinChip = std::string(chip);
  options.cubinFeatures = "+ptx87";
  options.cubinFormat = "assembly";
  options.optLevel = 2;
  mlir::PassManager manager(&context);
  mlir::gpu::buildLowerToNVVMPassPipeline(manager, options);
  if (mlir::failed(manager.run(*module)))
    return 1;
  std::string ptx;
  unsigned objects = 0;
  module->walk([&](mlir::gpu::BinaryOp binary) {
    for (auto attribute : binary.getObjects()) {
      auto object = mlir::dyn_cast<mlir::gpu::ObjectAttr>(attribute);
      if (!object || object.getFormat() != mlir::gpu::CompilationTarget::Assembly) {
        valid = false;
        return;
      }
      ptx = object.getObject().getValue().str();
      ++objects;
    }
  });
  if (!valid || objects != 1 || ptx.empty())
    return 1;
  // CUDA Driver JIT supplies hardware instruction lowering. No libdevice or
  // other vendor numerical implementation is linked implicitly.
  if (ptx.find(".extern .func") != std::string::npos) {
    llvm::errs() << "CUDA artifact contains an unresolved device function\n";
    return 1;
  }
  envelope.ptx = std::move(ptx);
  ptx = kalandra::encodeCudaArtifact(envelope);
  if (!kalandra::decodeCudaArtifact(ptx))
    return 1;
  const auto hash = llvm::SHA256::hash(
      llvm::ArrayRef(reinterpret_cast<const std::uint8_t *>(ptx.data()), ptx.size()));
  const auto digest = llvm::toHex(llvm::ArrayRef(hash), true);
  std::error_code error;
  llvm::raw_fd_ostream output(argv[2], error, llvm::sys::fs::OF_Text);
  if (error) {
    llvm::errs() << error.message();
    return 1;
  }
  output << ptx;
  llvm::raw_fd_ostream metadata(std::string(argv[2]) + ".json", error, llvm::sys::fs::OF_Text);
  if (error) {
    llvm::errs() << error.message();
    return 1;
  }
  metadata << llvm::formatv("{0:2}\n",
                            llvm::json::Value(llvm::json::Object{{"format", 1},
                                                                 {"abi", "memref1d-descriptor-v1"},
                                                                 {"chip", std::string(chip)},
                                                                 {"ptxSHA256", digest},
                                                                 {"ptxFeatures", "+ptx87"},
                                                                 {"arithmetic", argv[3]},
                                                                 {"entries", std::move(entries)}}));
  output.flush();
  metadata.flush();
  return output.has_error() || metadata.has_error();
}
