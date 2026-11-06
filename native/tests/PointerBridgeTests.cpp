#include "../gpu-source/PointerBridge.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T> static T take(llvm::Expected<T> result) {
  if (!result)
    throw std::runtime_error(llvm::toString(result.takeError()));
  return std::move(*result);
}
int main() {
  try {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    auto jit = take(llvm::orc::LLJITBuilder().create());
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("pointer.bridge", *context);
    module->setDataLayout(jit->getDataLayout());
    module->setTargetTriple(jit->getTargetTriple());
    llvm::IRBuilder<> b(*context);
    auto *ptr = b.getPtrTy();
    auto *dimensions = llvm::ArrayType::get(b.getInt64Ty(), 1);
    auto *array =
        llvm::StructType::get(*context, {ptr, ptr, b.getInt64Ty(), dimensions, dimensions});
    // Narrow fields deliberately surround wide/descriptor members. Never let a
    // C++ compiler choose how to pass/return this native aggregate by value.
    auto *record =
        llvm::StructType::get(*context, {b.getInt8Ty(), b.getInt64Ty(), array, b.getInt1Ty(),
                                         b.getFloatTy(), b.getDoubleTy()});
    auto *target =
        llvm::Function::Create(llvm::FunctionType::get(record, {record, b.getInt8Ty()}, false),
                               llvm::GlobalValue::InternalLinkage, "source.record", *module);
    target->addParamAttr(1, llvm::Attribute::SExt);
    target->setMemoryEffects(llvm::MemoryEffects::none());
    b.SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", target));
    auto *value = target->getArg(0);
    auto *wide = b.CreateAdd(b.CreateExtractValue(value, 1),
                             b.CreateSExt(target->getArg(1), b.getInt64Ty()));
    auto *result = b.CreateInsertValue(value, wide, 1);
    b.CreateRet(result);
    auto *bridge = emitCpuPointerBridge(*target, "record_bridge");
    require(bridge->getReturnType()->isVoidTy() && bridge->arg_size() == 3 &&
                bridge->getArg(0)->getType()->isPointerTy() &&
                bridge->getArg(1)->getType()->isPointerTy() &&
                bridge->hasParamAttribute(2, llvm::Attribute::SExt),
            "aggregate result pointer shifted scalar attributes incorrectly");
    require(!bridge->hasParamAttribute(1, llvm::Attribute::SExt),
            "extension attribute attached to pointer");
    require(!bridge->doesNotAccessMemory(),
            "by-value target memory attributes leaked onto the pointer wrapper");
    bool duplicate = false;
    try {
      emitCpuPointerBridge(*target, "record_bridge");
    } catch (const std::invalid_argument &) {
      duplicate = true;
    }
    require(duplicate, "duplicate exported bridge admitted");
    // Also exercise the existing array/void/scalar signatures through this same
    // implementation. Strict extension attributes stay on the original slots.
    auto *scalar = llvm::Function::Create(
        llvm::FunctionType::get(b.getInt8Ty(), {array, b.getInt8Ty()}, false),
        llvm::GlobalValue::InternalLinkage, "source.scalar", *module);
    scalar->addRetAttr(llvm::Attribute::ZExt);
    scalar->addParamAttr(1, llvm::Attribute::ZExt);
    b.SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", scalar));
    b.CreateRet(scalar->getArg(1));
    auto *scalarBridge = emitCpuPointerBridge(*scalar, "scalar_bridge");
    require(scalarBridge->hasRetAttribute(llvm::Attribute::ZExt) &&
                scalarBridge->hasParamAttribute(1, llvm::Attribute::ZExt),
            "scalar attributes lost");
    auto *indirect =
        llvm::Function::Create(llvm::FunctionType::get(b.getVoidTy(), {ptr, array}, false),
                               llvm::GlobalValue::InternalLinkage, "source.array", *module);
    b.SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", indirect));
    b.CreateStore(indirect->getArg(1), indirect->getArg(0));
    b.CreateRetVoid();
    emitCpuPointerBridge(*indirect, "array_bridge");
    auto *closureType = llvm::StructType::get(*context, {ptr, ptr});
    std::array<llvm::Function *, 2> closureCodes;
    for (unsigned i = 0; i < closureCodes.size(); ++i) {
      closureCodes[i] = llvm::Function::Create(
          llvm::FunctionType::get(b.getInt64Ty(), {ptr}, false),
          llvm::GlobalValue::InternalLinkage, "closure." + std::to_string(i), *module);
      b.SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", closureCodes[i]));
      b.CreateRet(b.getInt64(i));
    }
    const CpuClosureTarget closureTargets[]{{closureCodes[0], 301, false},
                                            {closureCodes[1], 907, true}};
    emitCpuClosureBridge(*module, "closure_bridge", closureTargets);
    auto *factory = llvm::Function::Create(
        llvm::FunctionType::get(closureType, {ptr, b.getInt64Ty()}, false),
        llvm::GlobalValue::InternalLinkage, "closure.factory", *module);
    b.SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", factory));
    auto *selected = b.CreateSelect(b.CreateICmpEQ(factory->getArg(1), b.getInt64(0)),
                                    closureCodes[0], closureCodes[1]);
    auto *closureValue = b.CreateInsertValue(llvm::Constant::getNullValue(closureType), selected, 0);
    b.CreateRet(b.CreateInsertValue(closureValue, factory->getArg(0), 1));
    emitCpuPointerBridge(*factory, "closure_factory");
    closureCodes[0]->setName("renamed.projection.callback.0");
    closureCodes[1]->setName("renamed.projection.callback.1");
    bool badClosureTable = false;
    try {
      const CpuClosureTarget repeated[]{{closureCodes[0], 1, false}, {closureCodes[1], 1, true}};
      emitCpuClosureBridge(*module, "invalid_closure_bridge", repeated);
    } catch (const std::invalid_argument &) {
      badClosureTable = true;
    }
    require(badClosureTable && !module->getNamedValue("invalid_closure_bridge"),
            "invalid closure target table left a partial bridge");
    require(!llvm::verifyModule(*module, &llvm::errs()),
            "pointer bridges failed LLVM verification");
    const auto &layout = jit->getDataLayout();
    auto *fields = layout.getStructLayout(record);
    const auto bytes = layout.getTypeAllocSize(record).getFixedValue();
    const auto wideOffset = fields->getElementOffset(1);
    const auto arrayOffset = fields->getElementOffset(2);
    const auto boolOffset = fields->getElementOffset(3);
    const auto floatOffset = fields->getElementOffset(4);
    const auto doubleOffset = fields->getElementOffset(5);
    const auto arrayBytes = layout.getTypeAllocSize(array).getFixedValue();
    // All supported native targets have <=16-byte ABI alignment. The slots are
    // oversized test storage, with sizes/offsets supplied exclusively by LLVM.
    alignas(16) std::array<std::byte, 256> input{}, output{};
    require(bytes <= input.size() && layout.getABITypeAlign(record).value() <= 16,
            "test slot too small");
    std::int64_t wideValue = 1000;
    std::memcpy(input.data() + wideOffset, &wideValue, sizeof(wideValue));
    input[0] = std::byte{0x81};
    input[boolOffset] = std::byte{1};
    std::uint32_t floatBits = 0x80000000;
    std::uint64_t doubleBits = UINT64_C(0x7ff8000000001234);
    std::memcpy(input.data() + floatOffset, &floatBits, sizeof(floatBits));
    std::memcpy(input.data() + doubleOffset, &doubleBits, sizeof(doubleBits));
    // A descriptor contains a real identity pointer, offset, length and stride;
    // this bridge preserves it rather than treating it as flat element bytes.
    std::array<std::uint64_t, 3> backing{11, 22, 33};
    auto *arrayFields = layout.getStructLayout(array);
    void *pointer = backing.data();
    std::memcpy(input.data() + arrayOffset + arrayFields->getElementOffset(0), &pointer,
                sizeof(pointer));
    std::memcpy(input.data() + arrayOffset + arrayFields->getElementOffset(1), &pointer,
                sizeof(pointer));
    std::uint64_t offset = 1, length = 2, stride = 1;
    std::memcpy(input.data() + arrayOffset + arrayFields->getElementOffset(2), &offset, 8);
    std::memcpy(input.data() + arrayOffset + arrayFields->getElementOffset(3), &length, 8);
    std::memcpy(input.data() + arrayOffset + arrayFields->getElementOffset(4), &stride, 8);
    if (auto error =
            jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context))))
      throw std::runtime_error(llvm::toString(std::move(error)));
    auto run =
        take(jit->lookup("record_bridge")).toPtr<void (*)(void *, const void *, std::int8_t)>();
    auto createClosure = take(jit->lookup("closure_factory"))
                             .toPtr<void (*)(void *, const void *, std::int64_t)>();
    auto identifyClosure = take(jit->lookup("closure_bridge"))
                               .toPtr<std::uint64_t (*)(const void *, const void **)>();
    struct Carrier { const void *code, *environment; } carrier{};
    const void *environment = input.data();
    createClosure(&carrier, nullptr, 0);
    require(identifyClosure(&carrier, &environment) == 301 && !environment,
            "capture-free identity failed after CPU projection rename");
    createClosure(&carrier, input.data(), 1);
    require(identifyClosure(&carrier, &environment) == 907 && environment == input.data(),
            "capturing identity lost its borrowed environment");
    carrier.environment = nullptr;
    environment = input.data();
    require(identifyClosure(&carrier, &environment) == 0 && !environment,
            "capturing target admitted a missing environment");
    createClosure(&carrier, input.data(), 0);
    require(identifyClosure(&carrier, &environment) == 0 && !environment,
            "capture-free target admitted a foreign environment");
    carrier.code = input.data();
    environment = input.data();
    require(identifyClosure(&carrier, &environment) == 0 && !environment,
            "unknown native code identity acquired a device tag");
    run(output.data(), input.data(), -7);
    std::int64_t got;
    std::memcpy(&got, output.data() + wideOffset, sizeof(got));
    require(got == 993 && output[0] == input[0] && output[boolOffset] == input[boolOffset] &&
                std::memcmp(output.data() + arrayOffset, input.data() + arrayOffset, arrayBytes) ==
                    0,
            "executed aggregate bridge changed values, alias identity or narrow bits");
    require(std::memcmp(output.data() + floatOffset, &floatBits, sizeof(floatBits)) == 0 &&
                std::memcmp(output.data() + doubleOffset, &doubleBits, sizeof(doubleBits)) == 0,
            "aggregate bridge changed signed zero or NaN payload bits");
    floatBits = 1; // Smallest positive subnormal, with no floating-point arithmetic.
    doubleBits = UINT64_C(0x8000000000000000);
    std::memcpy(input.data() + floatOffset, &floatBits, sizeof(floatBits));
    std::memcpy(input.data() + doubleOffset, &doubleBits, sizeof(doubleBits));
    run(output.data(), input.data(), -7);
    require(std::memcmp(output.data() + floatOffset, &floatBits, sizeof(floatBits)) == 0 &&
                std::memcmp(output.data() + doubleOffset, &doubleBits, sizeof(doubleBits)) == 0,
            "aggregate bridge changed subnormal or float64 signed zero bits");
    auto scalarRun =
        take(jit->lookup("scalar_bridge")).toPtr<std::uint8_t (*)(const void *, std::uint8_t)>();
    require(scalarRun(input.data() + arrayOffset, 255) == 255, "executed scalar extension failed");
    output.fill(std::byte{0});
    auto arrayRun = take(jit->lookup("array_bridge")).toPtr<void (*)(void *, const void *)>();
    arrayRun(output.data(), input.data() + arrayOffset);
    require(std::memcmp(output.data(), input.data() + arrayOffset, arrayBytes) == 0,
            "existing indirect array result bridge changed");
    std::cout << "native pointer bridges executed: aggregate return, padded/narrow fields, array "
                 "identity, "
                 "scalar extensions, indirect results and LLVM verification passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
