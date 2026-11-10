#pragma once
#include "TaskValueTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include <algorithm>
#include <stdexcept>

namespace kalandra {
// A console operation is host control, not a numerical kernel. Both CPU tasks
// and mixed-device host supervisors use the same packet construction.
inline llvm::StructType *consoleResultType(const TaskValueTypes &types,
                                           llvm::LLVMContext &context) {
  ValueType text;
  text.builtin = BuiltinValue::String;
  auto *i32 = llvm::Type::getInt32Ty(context);
  return llvm::StructType::get(context, {i32, i32, types.scalar(text)});
}
inline llvm::Value *emitConsoleStep(llvm::IRBuilder<> &b, llvm::Module &module,
                                    const ConsoleOperation &operation, llvm::Value *argument,
                                    llvm::Value *scratch, llvm::Value *state,
                                    llvm::Value *context) {
  const bool read = operation.action == ConsoleAction::ReadLine;
  llvm::Value *done;
  if (read) {
    auto callee =
        module.getOrInsertFunction("kalandra_console_read_line_step_v1", b.getInt1Ty(),
                                   b.getPtrTy(), b.getPtrTy(), b.getInt64Ty(), b.getPtrTy());
    done = b.CreateCall(callee, {context, scratch, b.CreateLoad(b.getInt64Ty(), argument), state});
  } else {
    const unsigned stream =
        operation.action == ConsoleAction::Eprint || operation.action == ConsoleAction::Eprintln
            ? 2
            : 1;
    const bool newline =
        operation.action == ConsoleAction::Println || operation.action == ConsoleAction::Eprintln;
    auto callee = module.getOrInsertFunction("kalandra_console_write_step_v1", b.getInt1Ty(),
                                             b.getPtrTy(), b.getPtrTy(), b.getPtrTy(),
                                             b.getInt32Ty(), b.getInt1Ty(), b.getPtrTy());
    done = b.CreateCall(
        callee, {context, scratch, argument, b.getInt32(stream), b.getInt1(newline), state});
  }
  return done;
}

inline void emitConsolePacket(llvm::IRBuilder<> &b, llvm::Module &module, const TypedModule &input,
                              const TaskValueTypes &types, const ConsoleOperation &operation,
                              const ValueType &result, llvm::StructType *resultType,
                              llvm::Value *argument, llvm::Value *output, llvm::Value *scratch,
                              llvm::Value *context, llvm::BasicBlock *next,
                              llvm::BasicBlock *abort) {
  const bool read = operation.action == ConsoleAction::ReadLine;
  if (!result.errorRecord ||
      input.records.at(*result.errorRecord).standard != StandardRecord::IoError)
    throw std::invalid_argument("console operation lost its nominal IoError identity");
  auto *transport = consoleResultType(types, module.getContext());
  auto *parent = b.GetInsertBlock()->getParent();
  const auto block = [&](const char *name) {
    return llvm::BasicBlock::Create(module.getContext(), name, parent);
  };
  b.CreateStore(llvm::Constant::getNullValue(resultType), output);
  auto *invalid = block("console.invalid");
  auto *success = block("console.success");
  auto *system = block("console.system");
  auto *encoding = read ? block("console.encoding") : invalid;
  auto *tooLong = read ? block("console.line.limit") : invalid;
  auto *end = read ? block("console.end") : invalid;
  auto *kind = b.CreateLoad(b.getInt32Ty(), b.CreateStructGEP(transport, scratch, 0));
  auto *dispatch = b.CreateSwitch(kind, invalid, read ? 5 : 2);
  dispatch->addCase(b.getInt32(0), success);
  dispatch->addCase(b.getInt32(2), system);
  if (read) {
    dispatch->addCase(b.getInt32(1), end);
    dispatch->addCase(b.getInt32(3), encoding);
    dispatch->addCase(b.getInt32(4), tooLong);
  }
  const auto field = [&](llvm::StructType *type, llvm::Value *record, std::size_t id) {
    return b.CreateStructGEP(type, record, id);
  };
  const auto storeField = [&](std::size_t id, llvm::Value *record, const char *name,
                              llvm::Value *value) {
    const auto &fields = input.records.at(id).fields;
    const auto found = std::ranges::find_if(fields, [&](const auto &f) { return f.name == name; });
    if (found == fields.end())
      throw std::invalid_argument("console standard record field is missing");
    b.CreateStore(value, field(types.records()[id], record, found - fields.begin()));
  };
  ValueType string;
  string.builtin = BuiltinValue::String;
  const auto text = [&] {
    return b.CreateLoad(types.scalar(string), field(transport, scratch, 2));
  };
  auto *errorType = types.records()[*result.errorRecord];
  const auto errorPacket = [&](unsigned tag) {
    auto *packet = field(resultType, output, 2);
    b.CreateStore(b.getInt64(tag), field(errorType, packet, 0));
    return packet;
  };
  b.SetInsertPoint(success);
  if (read) {
    if (!result.record || input.records.at(*result.record).standard != StandardRecord::Line)
      throw std::invalid_argument("console read lost its nominal Line identity");
    auto *line = field(resultType, output, 1);
    b.CreateStore(b.getInt64(2), field(types.records()[*result.record], line, 0));
    storeField(*result.record, line, "text", text());
  }
  b.CreateBr(next);
  b.SetInsertPoint(system);
  auto *packet = errorPacket(1);
  storeField(*result.errorRecord, packet, "code",
             b.CreateLoad(b.getInt32Ty(), field(transport, scratch, 1)));
  storeField(*result.errorRecord, packet, "message", text());
  b.CreateBr(next);
  if (read) {
    b.SetInsertPoint(end);
    b.CreateStore(b.getInt64(1),
                  field(types.records()[*result.record], field(resultType, output, 1), 0));
    b.CreateBr(next);
    b.SetInsertPoint(encoding);
    errorPacket(2);
    b.CreateBr(next);
    b.SetInsertPoint(tooLong);
    storeField(*result.errorRecord, errorPacket(3), "limit",
               b.CreateLoad(b.getInt64Ty(), argument));
    b.CreateBr(next);
  }
  b.SetInsertPoint(invalid);
  auto fault = module.getOrInsertFunction("kalandra_fault_v1", b.getVoidTy(), b.getPtrTy(),
                                          b.getInt64Ty(), b.getInt64Ty(), b.getInt64Ty());
  b.CreateCall(fault, {context, b.getInt64(3), b.getInt64(0), b.getInt64(0)});
  b.CreateBr(abort);
}
inline void emitConsoleOperation(llvm::IRBuilder<> &b, llvm::Module &module,
                                 const TypedModule &input, const TaskValueTypes &types,
                                 const ConsoleOperation &operation, const ValueType &result,
                                 llvm::StructType *resultType, llvm::Value *argument,
                                 llvm::Value *output, llvm::Value *scratch, llvm::Value *state,
                                 llvm::Value *context, llvm::Value *task, llvm::Value *pc,
                                 std::size_t instruction, llvm::BasicBlock *next,
                                 llvm::BasicBlock *leave, llvm::BasicBlock *abort) {
  b.CreateStore(llvm::Constant::getNullValue(resultType), output);
  auto *done = emitConsoleStep(b, module, operation, argument, scratch, state, context);
  auto *parent = b.GetInsertBlock()->getParent();
  const auto block = [&](const char *name) {
    return llvm::BasicBlock::Create(module.getContext(), name, parent);
  };
  auto *finished = block("console.finished");
  auto *pending = block("console.pending");
  b.CreateCondBr(done, finished, pending);
  b.SetInsertPoint(pending);
  b.CreateStore(b.getInt32(instruction), pc);
  auto pause = module.getOrInsertFunction("kalandra_console_pause_v1", b.getVoidTy(), b.getPtrTy(),
                                          b.getPtrTy());
  b.CreateCall(pause, {task, b.CreateLoad(b.getPtrTy(), state)});
  b.CreateBr(leave);
  b.SetInsertPoint(finished);
  auto *ready = block("console.ready");
  auto poll =
      module.getOrInsertFunction("kalandra_invocation_ready_v1", b.getInt1Ty(), b.getPtrTy());
  b.CreateCondBr(b.CreateCall(poll, {context}), ready, abort);
  b.SetInsertPoint(ready);
  emitConsolePacket(b, module, input, types, operation, result, resultType, argument, output,
                    scratch, context, next, abort);
}

inline void emitConsoleSynchronousOperation(llvm::IRBuilder<> &b, llvm::Module &module,
                                            const TypedModule &input, const TaskValueTypes &types,
                                            const ConsoleOperation &operation,
                                            const ValueType &result, llvm::StructType *resultType,
                                            llvm::Value *argument, llvm::Value *output,
                                            llvm::Value *scratch, llvm::Value *context,
                                            llvm::BasicBlock *next, llvm::BasicBlock *abort) {
  b.CreateStore(llvm::Constant::getNullValue(resultType), output);
  if (operation.action == ConsoleAction::ReadLine) {
    auto callee = module.getOrInsertFunction("kalandra_console_read_line_v1", b.getVoidTy(),
                                             b.getPtrTy(), b.getPtrTy(), b.getInt64Ty());
    b.CreateCall(callee, {context, scratch, b.CreateLoad(b.getInt64Ty(), argument)});
  } else {
    const auto stream =
        operation.action == ConsoleAction::Eprint || operation.action == ConsoleAction::Eprintln
            ? 2
            : 1;
    const auto newline =
        operation.action == ConsoleAction::Println || operation.action == ConsoleAction::Eprintln;
    auto callee =
        module.getOrInsertFunction("kalandra_console_write_v1", b.getVoidTy(), b.getPtrTy(),
                                   b.getPtrTy(), b.getPtrTy(), b.getInt32Ty(), b.getInt1Ty());
    b.CreateCall(callee, {context, scratch, argument, b.getInt32(stream), b.getInt1(newline)});
  }
  auto *ready = llvm::BasicBlock::Create(module.getContext(), "console.direct.ready",
                                         b.GetInsertBlock()->getParent());
  auto poll =
      module.getOrInsertFunction("kalandra_invocation_ready_v1", b.getInt1Ty(), b.getPtrTy());
  b.CreateCondBr(b.CreateCall(poll, {context}), ready, abort);
  b.SetInsertPoint(ready);
  emitConsolePacket(b, module, input, types, operation, result, resultType, argument, output,
                    scratch, context, next, abort);
}

} // namespace kalandra
