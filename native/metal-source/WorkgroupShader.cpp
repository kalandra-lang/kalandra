#include "../lib/ErrorFamilies.h"
#include "NumericShader.h"
#include "WorkgroupPlan.h"
#include "kalandra/GpuConversion.h"
#include "kalandra/GpuInteger.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVAttributes.h"
#include "mlir/IR/Verifier.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <set>
#include <sstream>

namespace kalandra {
namespace {
using namespace mlir;
struct Rejected {
  Diagnostic diagnostic;
};
struct DeviceValue {
  Value bits, data, slot, failed, failureSlot;
  std::vector<Value> shape;
  std::vector<DeviceValue> elements;
  std::vector<DeviceValue> errors;
  ValueType type;
  std::optional<std::int64_t> constant;
  std::optional<FunctionId> function;
};
struct Frame {
  std::map<SymbolId, DeviceValue> symbols;
  std::map<ValueId, DeviceValue> values, completed;
  std::map<std::size_t, DeviceValue> results;
  Value control, failed;
  DeviceValue error;
};
class Emitter {
  const TypedModule &input;
  const WorkgroupPlan &plan;
  const NumericShaderOptions &options;
  gpu::GPUFuncOp kernel;
  OpBuilder builder;
  Value fault, parameters, output, block, thread;
  std::vector<Value> globals, locals, dimensions;
  std::vector<DeviceValue> tensors, scalars;
  std::map<std::string, std::size_t> scratch;
  std::vector<Value> scratchStorage;
  std::string context;
  std::set<FunctionId> active;
  std::size_t steps = 0;
  std::uint32_t words;
  Frame *checkingFrame = nullptr;

  Location loc() { return builder.getUnknownLoc(); }
  [[noreturn]] void reject(ValueId id, const char *message) {
    throw Rejected{{"G001", message, input.expressions.at(id).span}};
  }
  Value c(std::uint64_t bits, unsigned width = 64) {
    return arith::ConstantOp::create(
        builder, loc(), builder.getIntegerType(width),
        builder.getIntegerAttr(builder.getIntegerType(width), llvm::APInt(width, bits)));
  }
  Value at(std::uint64_t index) { return arith::ConstantIndexOp::create(builder, loc(), index); }
  Value cmp(arith::CmpIPredicate pred, Value a, Value b) {
    return arith::CmpIOp::create(builder, loc(), pred, a, b);
  }
  Value both(Value a, Value b) { return arith::AndIOp::create(builder, loc(), a, b); }
  Value either(Value a, Value b) { return arith::OrIOp::create(builder, loc(), a, b); }
  Value select(Value condition, Value a, Value b) {
    return arith::SelectOp::create(builder, loc(), condition, a, b);
  }
  Value negate(Value value) { return arith::XOrIOp::create(builder, loc(), value, c(1, 1)); }
  Value index(Value bits) {
    return arith::IndexCastUIOp::create(builder, loc(), builder.getIndexType(), bits);
  }
  Value widen(Value bits) {
    return bits.getType().isInteger(64)
               ? bits
               : Value(arith::ExtUIOp::create(builder, loc(), builder.getI64Type(), bits));
  }
  Value narrow(Value bits, unsigned width) {
    return width == 64 ? bits
                       : Value(arith::TruncIOp::create(builder, loc(),
                                                       builder.getIntegerType(width), bits));
  }
  Value load(Value memory, std::uint64_t offset = 0) {
    return memref::LoadOp::create(builder, loc(), memory, ValueRange{at(offset)});
  }
  void store(Value value, Value memory, std::uint64_t offset = 0) {
    memref::StoreOp::create(builder, loc(), value, memory, ValueRange{at(offset)});
  }
  Value privateSlot() {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(&kernel.getBody().front());
    auto space = spirv::StorageClassAttr::get(builder.getContext(), spirv::StorageClass::Function);
    return memref::AllocaOp::create(
        builder, loc(),
        MemRefType::get({1}, builder.getI64Type(), MemRefLayoutAttrInterface{}, space));
  }
  void checked(Value valid) {
    // Sticky fault state affects publication, never later uniform calculations
    // or participation in barriers. Checked primitives supply defined bits.
    if (checkingFrame)
      valid = either(negate(running(*checkingFrame)), valid);
    store(both(load(fault), widen(valid)), fault);
  }
  DeviceValue scalar(Value value, NumericType element = NumericType::Int64) {
    DeviceValue out;
    out.bits = value;
    out.type.element = element;
    return out;
  }
  DeviceValue literal(std::uint64_t bits, NumericType element = NumericType::Int64) {
    auto out = scalar(c(bits), element);
    if (element == NumericType::Int64)
      out.constant = std::bit_cast<std::int64_t>(bits);
    return out;
  }
  Value nonzero(Value bits) { return cmp(arith::CmpIPredicate::ne, bits, c(0)); }
  Value word64(unsigned offset, bool wide = true) {
    auto low = widen(load(parameters, offset));
    return wide ? either(low, arith::ShLIOp::create(builder, loc(),
                                                    widen(load(parameters, offset + 1)), c(32)))
                : low;
  }
  DeviceValue arithmetic(DeviceValue a, DeviceValue b, TokenKind operation, ValueId site) {
    const auto type = a.type.element;
    Value result;
    if (isInteger(type)) {
      const auto op = operation == TokenKind::Plus    ? GpuIntegerOperation::Add
                      : operation == TokenKind::Minus ? GpuIntegerOperation::Subtract
                      : operation == TokenKind::Star  ? GpuIntegerOperation::Multiply
                      : operation == TokenKind::Slash ? GpuIntegerOperation::Divide
                                                      : GpuIntegerOperation::Remainder;
      if (operation != TokenKind::Plus && operation != TokenKind::Minus &&
          operation != TokenKind::Star && operation != TokenKind::Slash &&
          operation != TokenKind::Percent)
        reject(site, "unsupported cooperative numeric operator");
      auto value = *emitCheckedGpuInteger(builder, loc(), type, op, a.bits, b.bits);
      checked(value.valid);
      result = value.bits;
    } else {
      const auto bits = describeNumeric(type)->bits;
      auto floating = bits == 32 ? Type(builder.getF32Type()) : Type(builder.getF64Type());
      Value x = arith::BitcastOp::create(builder, loc(), floating, narrow(a.bits, bits));
      Value y = arith::BitcastOp::create(builder, loc(), floating, narrow(b.bits, bits));
      if (operation == TokenKind::Plus)
        result = arith::AddFOp::create(builder, loc(), x, y);
      else if (operation == TokenKind::Minus)
        result = arith::SubFOp::create(builder, loc(), x, y);
      else if (operation == TokenKind::Star)
        result = arith::MulFOp::create(builder, loc(), x, y);
      else if (operation == TokenKind::Slash)
        result = arith::DivFOp::create(builder, loc(), x, y);
      else
        reject(site, "unsupported cooperative floating operator");
      result =
          widen(arith::BitcastOp::create(builder, loc(), builder.getIntegerType(bits), result));
    }
    return scalar(result, type);
  }
  DeviceValue comparison(DeviceValue a, DeviceValue b, ComparisonKind operation) {
    using P = arith::CmpIPredicate;
    auto x = a.bits, y = b.bits;
    auto type = a.type.element;
    auto bits = describeNumeric(type)->bits;
    Value result;
    if (isInteger(type) || a.type.builtin == BuiltinValue::Bool) {
      if (!isUnsigned(type) && bits < 64 && a.type.builtin != BuiltinValue::Bool) {
        x = arith::ExtSIOp::create(builder, loc(), builder.getI64Type(), narrow(x, bits));
        y = arith::ExtSIOp::create(builder, loc(), builder.getI64Type(), narrow(y, bits));
      }
      const auto u = isUnsigned(type);
      auto predicate = operation == ComparisonKind::Equal       ? P::eq
                       : operation == ComparisonKind::NotEqual  ? P::ne
                       : operation == ComparisonKind::Less      ? (u ? P::ult : P::slt)
                       : operation == ComparisonKind::LessEqual ? (u ? P::ule : P::sle)
                       : operation == ComparisonKind::Greater   ? (u ? P::ugt : P::sgt)
                                                                : (u ? P::uge : P::sge);
      result = cmp(predicate, x, y);
    } else {
      // IEEE ordering of bit carriers works on Metal's software f64 path too.
      auto mask = c((UINT64_C(1) << (bits - 1)) - 1), sign = c(UINT64_C(1) << (bits - 1));
      auto inf = c(bits == 32 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000));
      auto ax = both(x, mask), ay = both(y, mask);
      auto ordered = both(cmp(P::ule, ax, inf), cmp(P::ule, ay, inf));
      auto equal = either(cmp(P::eq, x, y), both(cmp(P::eq, ax, c(0)), cmp(P::eq, ay, c(0))));
      auto sx = both(x, sign), sy = both(y, sign);
      auto negative = cmp(P::ne, sx, c(0));
      auto less =
          both(negate(equal), select(cmp(P::ne, sx, sy), negative,
                                     select(negative, cmp(P::ugt, x, y), cmp(P::ult, x, y))));
      result = operation == ComparisonKind::Equal       ? both(ordered, equal)
               : operation == ComparisonKind::NotEqual  ? negate(both(ordered, equal))
               : operation == ComparisonKind::Less      ? both(ordered, less)
               : operation == ComparisonKind::LessEqual ? both(ordered, either(less, equal))
               : operation == ComparisonKind::Greater   ? both(ordered, negate(either(less, equal)))
                                                        : both(ordered, negate(less));
    }
    auto out = scalar(widen(result));
    out.type.builtin = BuiltinValue::Bool;
    return out;
  }
  DeviceValue persist(DeviceValue value, const DeviceValue *previous = nullptr) {
    if (value.bits && !value.data && value.elements.empty() && !value.function) {
      value.slot = previous && previous->slot ? previous->slot : privateSlot();
      store(value.bits, value.slot);
    }
    for (std::size_t i = 0; i < value.elements.size(); ++i)
      value.elements[i] =
          persist(value.elements[i],
                  previous && i < previous->elements.size() ? &previous->elements[i] : nullptr);
    if (value.failed) {
      value.failureSlot = previous && previous->failureSlot ? previous->failureSlot : privateSlot();
      store(value.failed, value.failureSlot);
    }
    for (std::size_t i = 0; i < value.errors.size(); ++i)
      value.errors[i] =
          persist(value.errors[i],
                  previous && i < previous->errors.size() ? &previous->errors[i] : nullptr);
    return value;
  }
  DeviceValue materialize(DeviceValue value) {
    if (value.slot)
      value.bits = load(value.slot);
    if (value.failureSlot)
      value.failed = load(value.failureSlot);
    for (auto &element : value.elements)
      element = materialize(element);
    for (auto &error : value.errors)
      error = materialize(error);
    return value;
  }
  DeviceValue zero(ValueType type, ValueId site, unsigned depth = 0) {
    if (depth > 32 || type.tensor || type.array || type.task || type.device)
      reject(site,
             "cooperative private error/control payload requires a finite inline numeric layout");
    DeviceValue value;
    value.type = type;
    if (type.builtin == BuiltinValue::Record && type.record &&
        (input.records.at(*type.record).standard == StandardRecord::WorkItem ||
         input.records.at(*type.record).standard == StandardRecord::Shared))
      value.bits = c(0);
    else if (type.builtin == BuiltinValue::Record && type.record) {
      for (const auto &field : input.records.at(*type.record).fields)
        value.elements.push_back(zero(field.type, site, depth + 1));
    } else if (type.builtin == BuiltinValue::Numeric || type.builtin == BuiltinValue::Bool ||
               type.builtin == BuiltinValue::Unit)
      value.bits = c(0);
    else
      reject(site,
             "cooperative private payload requires numeric, Bool, unit or inline record values");
    if (type.fallible) {
      if (!type.errorRecord)
        reject(site, "cooperative expected errors require a declared application-error family");
      ValueType error;
      error.builtin = BuiltinValue::Record;
      error.record = type.errorRecord;
      value.failed = c(0);
      value.errors.push_back(zero(error, site, depth + 1));
    }
    return value;
  }
  void assign(const DeviceValue &destination, const DeviceValue &source, ValueId site) {
    if (destination.type.record && source.type.record &&
        destination.type.record != source.type.record &&
        input.records.at(*destination.type.record).error &&
        input.records.at(*source.type.record).error) {
      const auto fromId = *source.type.record, toId = *destination.type.record;
      const auto &from = input.records.at(fromId), &to = input.records.at(toId);
      const auto charge = from.variants.size() + to.variants.size() + to.fields.size();
      if (charge > 300000 - std::min<std::size_t>(steps, 300000))
        reject(site, "cooperative error conversion exceeds its work budget");
      steps += charge;
      if (source.elements.size() != from.fields.size() ||
          destination.elements.size() != to.fields.size() || source.elements.empty())
        reject(site, "cooperative error conversion has an invalid record layout");
      // A union has its own tag numbering and inactive payload slots. Copy
      // only the selected original variant, leaving every other slot zero.
      assign(destination, zero(destination.type, site), site);
      std::map<std::pair<std::size_t, std::size_t>, std::size_t> variants;
      for (std::size_t v = 0; v < to.variants.size(); ++v)
        variants.emplace(errorOrigin(input, toId, v), v);
      auto matched = c(0, 1);
      for (std::size_t v = 0; v < from.variants.size(); ++v) {
        auto found = variants.find(errorOrigin(input, fromId, v));
        if (found == variants.end())
          continue;
        const auto target = found->second;
        const auto &fields = from.variants[v].fields;
        const auto &targetFields = to.variants[target].fields;
        if (fields.size() != targetFields.size())
          reject(site, "cooperative error variants disagree on payload fields");
        auto selected = cmp(arith::CmpIPredicate::eq, source.elements.front().bits, c(v + 1));
        matched = either(matched, selected);
        when(selected, [&] {
          assign(destination.elements.front(), scalar(c(target + 1)), site);
          for (std::size_t field = 0; field < fields.size(); ++field)
            assign(destination.elements.at(targetFields[field]), source.elements.at(fields[field]),
                   site);
        });
      }
      checked(matched);
      return;
    }
    if (destination.slot) {
      if (!source.bits)
        reject(site, "cooperative control value does not match its scalar layout");
      store(source.bits, destination.slot);
    }
    if (destination.elements.size() != source.elements.size())
      reject(site, "cooperative control value does not match its record layout");
    for (std::size_t i = 0; i < destination.elements.size(); ++i)
      assign(destination.elements[i], source.elements[i], site);
    if (destination.failureSlot) {
      store(source.failed ? source.failed : c(0), destination.failureSlot);
      if (source.failed) {
        if (destination.errors.size() != source.errors.size())
          reject(site, "cooperative expected-error payload layout mismatch");
        for (std::size_t i = 0; i < destination.errors.size(); ++i)
          assign(destination.errors[i], source.errors[i], site);
      }
    }
  }
  Value running(const Frame &frame) {
    auto live = cmp(arith::CmpIPredicate::eq, load(frame.control), c(0));
    return frame.failed ? both(live, cmp(arith::CmpIPredicate::eq, load(frame.failed), c(0)))
                        : live;
  }
  void errorLeaves(const DeviceValue &value, std::vector<Value> &leaves) {
    if (value.bits)
      leaves.push_back(value.bits);
    for (const auto &field : value.elements)
      errorLeaves(field, leaves);
  }
  void bind(Frame &frame, SymbolId symbol, DeviceValue value) {
    auto found = frame.symbols.find(symbol);
    frame.symbols[symbol] =
        persist(std::move(value), found == frame.symbols.end() ? nullptr : &found->second);
  }
  DeviceValue read(Frame &frame, SymbolId symbol, ValueId site) {
    if (!frame.symbols.contains(symbol))
      reject(site, "unbound cooperative shader value");
    return materialize(frame.symbols.at(symbol));
  }
  template <class Emit> void when(Value condition, Emit emit) {
    auto branch = scf::IfOp::create(builder, loc(), condition, false);
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(&branch.getThenRegion().front());
    emit();
  }
  void suite(Frame &frame, const std::vector<TypedStatement> &body) {
    for (const auto &statement : body) {
      // All lexical variable slots are allocated at entry. SSA results remain
      // inside their guarded statement; subsequent reads use those slots.
      when(running(frame), [&] {
        auto value = evaluate(frame, statement.value);
        if (statement.binding)
          bind(frame, *statement.binding, value);
        frame.completed[statement.value] = persist(std::move(value));
      });
      frame.values.clear();
    }
  }
  DeviceValue invoke(FunctionId id, const std::vector<DeviceValue> &arguments, ValueId site) {
    if (active.size() >= 256 || !active.insert(id).second)
      reject(site, "recursive cooperative shader helpers require a finite expansion");
    auto previous = context;
    context += "/" + std::to_string(site) + "@" + std::to_string(id);
    const auto &function = input.functions.at(id);
    if (arguments.size() != function.parameters.size())
      reject(site, "invalid cooperative helper capture count");
    Frame frame;
    frame.control = privateSlot();
    store(c(0), frame.control);
    if (function.resultType.fallible) {
      frame.failed = privateSlot();
      store(c(0), frame.failed);
      auto envelope = zero(function.resultType, site);
      frame.error = persist(std::move(envelope.errors.front()));
    }
    for (std::size_t i = 0; i < arguments.size(); ++i)
      bind(frame, function.parameters[i], arguments[i]);
    suite(frame, function.body);
    // A simple return expression is also the last source statement. Its result
    // must be retained, rather than executing a side-effecting expression twice.
    auto result = evaluate(frame, function.result);
    if (frame.failed) {
      result.failed = load(frame.failed);
      result.errors = {materialize(frame.error)};
    }
    context = previous;
    active.erase(id);
    return result;
  }
  DeviceValue call(Frame &caller, FunctionId function, const std::vector<DeviceValue> &arguments,
                   ValueId site) {
    // An argument may itself return, break or propagate an expected error.
    // Entering the callee is a later source effect and needs its own guard.
    auto result = persist(zero(input.expressions.at(site).type, site));
    when(running(caller), [&] { assign(result, invoke(function, arguments, site), site); });
    return materialize(std::move(result));
  }
  DeviceValue capture(const WorkgroupCapture &source, ValueId site) {
    if (source.tensor)
      return tensors.at(*source.tensor);
    std::vector<DeviceValue> stack;
    for (const auto &step : source.scalar) {
      using A = WorkgroupShapeAction;
      if (step.action == A::Constant)
        stack.push_back(literal(step.value, source.type.element));
      else if (step.action == A::Scalar)
        stack.push_back(scalars.at(step.input));
      else if (step.action == A::Extent)
        stack.push_back(scalar(tensors.at(step.input).shape.at(step.axis)));
      else {
        if (stack.empty())
          reject(site, "invalid cooperative scalar capture expression");
        auto right = stack.back();
        stack.pop_back();
        if (step.action == A::Negate)
          stack.push_back(arithmetic(literal(0), right, TokenKind::Minus, site));
        else {
          if (stack.empty())
            reject(site, "invalid cooperative binary capture expression");
          auto left = stack.back();
          stack.pop_back();
          const auto operation = step.action == A::Add        ? TokenKind::Plus
                                 : step.action == A::Subtract ? TokenKind::Minus
                                 : step.action == A::Multiply ? TokenKind::Star
                                 : step.action == A::Divide   ? TokenKind::Slash
                                                              : TokenKind::Percent;
          stack.push_back(arithmetic(left, right, operation, site));
        }
      }
    }
    if (stack.size() != 1)
      reject(site, "invalid cooperative capture stack");
    stack.back().type = source.type;
    return stack.back();
  }
  DeviceValue workgroup(Frame &frame, ValueId id, const WorkgroupOperation &operation) {
    using A = WorkgroupAction;
    DeviceValue out;
    out.type = input.expressions[id].type;
    const auto action = operation.metadata.action;
    evaluate(frame, operation.arguments[0]);
    if (action == A::GlobalIndex || action == A::LocalIndex) {
      auto axis = evaluate(frame, operation.arguments[1]);
      const auto &coordinates = action == A::GlobalIndex ? globals : locals;
      if (coordinates.empty())
        reject(id, "rank-zero workgroups have no index axes");
      checked(cmp(arith::CmpIPredicate::ult, axis.bits, c(coordinates.size())));
      out.bits = coordinates.front();
      for (std::size_t dimension = 1; dimension < coordinates.size(); ++dimension)
        out.bits = select(cmp(arith::CmpIPredicate::eq, axis.bits, c(dimension)),
                          coordinates[dimension], out.bits);
    } else if (action == A::Shared) {
      const auto key = context + ":" + std::to_string(id);
      if (!scratch.contains(key)) {
        const auto bits = describeNumeric(operation.metadata.element)->bits == 64 ? 64 : 32;
        auto space = gpu::AddressSpaceAttr::get(builder.getContext(), gpu::AddressSpace::Workgroup);
        auto type =
            MemRefType::get({static_cast<std::int64_t>(operation.metadata.count)},
                            builder.getIntegerType(bits), MemRefLayoutAttrInterface{}, space);
        scratch[key] = scratchStorage.size();
        scratchStorage.push_back(kernel.addWorkgroupAttribution(type, loc()));
      }
      out.bits = c(scratch.at(key) + 1);
    } else if (action == A::Barrier) {
      gpu::BarrierOp::create(builder, loc());
      out.bits = c(0);
    } else if (action == A::Read || action == A::Write) {
      auto source = evaluate(frame, operation.arguments[0]);
      auto position = evaluate(frame, operation.arguments[1]);
      if (!source.bits)
        reject(id, "cooperative scratch has no device storage provenance");
      auto result = privateSlot();
      store(c(0), result);
      Value matched = c(0, 1);
      auto stored = action == A::Write ? evaluate(frame, operation.arguments[2]) : DeviceValue{};
      for (std::size_t slot = 0; slot < scratchStorage.size(); ++slot) {
        auto storage = scratchStorage[slot];
        const auto type = cast<MemRefType>(storage.getType());
        const auto selected = cmp(arith::CmpIPredicate::eq, source.bits, c(slot + 1));
        matched = either(matched, selected);
        when(both(running(frame), selected), [&] {
          if (action == A::Read)
            store(widen(memref::LoadOp::create(builder, loc(), storage,
                                               ValueRange{index(position.bits)})),
                  result);
          else
            memref::StoreOp::create(builder, loc(),
                                    narrow(stored.bits, type.getElementTypeBitWidth()), storage,
                                    ValueRange{index(position.bits)});
        });
      }
      checked(matched);
      out.bits = load(result);
    } else
      reject(id, "nested cooperative dispatch is unsupported");
    return out;
  }
  DeviceValue evaluate(Frame &frame, ValueId id) {
    if (auto found = frame.completed.find(id); found != frame.completed.end())
      return materialize(found->second);
    if (auto found = frame.values.find(id); found != frame.values.end())
      return found->second;
    const auto &node = input.expressions.at(id);
    const bool privateRecord = node.type.builtin == BuiltinValue::Record && !node.type.tensor &&
                               !node.type.array && !node.type.task && !node.type.device;
    if (node.type.fallible || privateRecord) {
      auto result = persist(zero(node.type, id));
      const auto cache = frame.values;
      when(running(frame), [&] { assign(result, expression(frame, id), id); });
      frame.values = cache;
      auto value = materialize(std::move(result));
      frame.values[id] = value;
      return value;
    }
    // Scalar expressions may contain a nonlocal return/break in an operand.
    // Check the source control state before evaluating each subsequent operand,
    // so skipped helpers, stores and barriers remain skipped.
    const bool scalarResult =
        !node.type.tensor && !node.type.array &&
        (node.type.builtin == BuiltinValue::Numeric || node.type.builtin == BuiltinValue::Bool ||
         node.type.builtin == BuiltinValue::Unit);
    const bool literal = std::holds_alternative<Int64Literal>(node.operation) ||
                         std::holds_alternative<BoolLiteral>(node.operation);
    if (!scalarResult || literal)
      return expression(frame, id);
    const auto cache = frame.values;
    auto branch =
        scf::IfOp::create(builder, loc(), TypeRange{builder.getI64Type()}, running(frame), true);
    DeviceValue out;
    {
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToStart(&branch.getThenRegion().front());
      out = expression(frame, id);
      if (!out.bits)
        reject(id, "cooperative scalar expression has no bit value");
      scf::YieldOp::create(builder, loc(), ValueRange{out.bits});
      builder.setInsertionPointToStart(&branch.getElseRegion().front());
      scf::YieldOp::create(builder, loc(), ValueRange{c(0)});
    }
    frame.values = cache;
    out.bits = branch.getResult(0);
    frame.values[id] = out;
    return out;
  }
  DeviceValue expression(Frame &frame, ValueId id) {
    struct CheckingScope {
      Frame *&current;
      Frame *previous;
      ~CheckingScope() { current = previous; }
    } checking{checkingFrame, checkingFrame};
    checkingFrame = &frame;
    if (++steps > 300000)
      reject(id, "cooperative shader expansion exceeds its work budget");
    if (auto found = frame.values.find(id); found != frame.values.end())
      return found->second;
    const auto &node = input.expressions.at(id);
    DeviceValue out;
    out.type = node.type;
    if (auto value = std::get_if<SymbolRead>(&node.operation))
      out = read(frame, value->symbol, id);
    else if (auto operation = std::get_if<WorkgroupOperation>(&node.operation))
      out = workgroup(frame, id, *operation);
    else if (auto operation = std::get_if<ScalarBinary>(&node.operation)) {
      auto left = evaluate(frame, operation->left), right = evaluate(frame, operation->right);
      out = arithmetic(left, right, operation->operation, id);
    } else if (auto operation = std::get_if<ScalarCompare>(&node.operation)) {
      auto left = evaluate(frame, operation->left), right = evaluate(frame, operation->right);
      out = comparison(left, right, operation->operation);
    } else if (auto operation = std::get_if<NumericConvert>(&node.operation)) {
      auto value = evaluate(frame, operation->input);
      if (operation->bitwise)
        out.bits = value.bits;
      else {
        auto result =
            gpuNumericConversion(builder, loc(), value.bits, value.type.element, node.type.element,
                                 options.arithmetic, options.nativeFloat64Conversions);
        out.bits = result.first;
        checked(result.second);
      }
    } else if (auto operation = std::get_if<ScalarUnary>(&node.operation)) {
      out = evaluate(frame, operation->operand);
      if (operation->operation == TokenKind::Bang)
        out.bits = widen(negate(nonzero(out.bits)));
      else if (operation->operation == TokenKind::Minus) {
        if (isInteger(node.type.element))
          out = arithmetic(literal(0, node.type.element), out, TokenKind::Minus, id);
        else
          out.bits = arith::XOrIOp::create(
              builder, loc(), out.bits,
              c(UINT64_C(1) << (describeNumeric(node.type.element)->bits - 1)));
      } else if (operation->operation != TokenKind::Plus)
        reject(id, "unsupported cooperative unary operation");
    } else if (auto operation = std::get_if<CollectionQuery>(&node.operation)) {
      auto value = evaluate(frame, operation->input);
      if (operation->kind == CollectionQueryKind::Length)
        out.bits = value.shape.empty() ? c(value.elements.size()) : value.shape.front();
      else
        for (auto extent : value.shape)
          out.elements.push_back(scalar(extent));
    } else if (auto operation = std::get_if<TensorLiteral>(&node.operation)) {
      for (auto value : operation->elements)
        out.elements.push_back(evaluate(frame, value));
    } else if (auto operation = std::get_if<ArrayIndex>(&node.operation)) {
      auto value = evaluate(frame, operation->input), position = evaluate(frame, operation->index);
      auto normalized =
          select(cmp(arith::CmpIPredicate::slt, position.bits, c(0)),
                 arith::AddIOp::create(builder, loc(), position.bits, c(value.elements.size())),
                 position.bits);
      checked(cmp(arith::CmpIPredicate::ult, normalized, c(value.elements.size())));
      out.bits = c(0);
      for (std::size_t element = 0; element < value.elements.size(); ++element) {
        if (!value.elements[element].bits)
          reject(id, "cooperative private metadata requires numeric elements");
        out.bits = select(cmp(arith::CmpIPredicate::eq, normalized, c(element)),
                          value.elements[element].bits, out.bits);
      }
    } else if (auto operation = std::get_if<TensorIndex>(&node.operation)) {
      auto source = evaluate(frame, operation->input);
      if (!source.data || source.shape.size() != operation->indices.size())
        reject(id, "invalid cooperative tensor index layout");
      Value offset = c(0), valid = c(1, 1);
      for (std::size_t axis = 0; axis < operation->indices.size(); ++axis) {
        auto value = evaluate(frame, operation->indices[axis]);
        // As with ordinary tensors, negative positions count from the end.
        auto normalized = select(
            cmp(arith::CmpIPredicate::slt, value.bits, c(0)),
            arith::AddIOp::create(builder, loc(), value.bits, source.shape[axis]), value.bits);
        valid = both(valid, cmp(arith::CmpIPredicate::ult, normalized, source.shape[axis]));
        offset = arith::AddIOp::create(
            builder, loc(), arith::MulIOp::create(builder, loc(), offset, source.shape[axis]),
            normalized);
      }
      checked(valid);
      // A false predicate must not issue even a speculative out-of-bounds read,
      // including the empty-input case. Barriers remain outside this branch.
      auto branch = scf::IfOp::create(builder, loc(), TypeRange{builder.getI64Type()},
                                      both(running(frame), valid), true);
      {
        OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToStart(&branch.getThenRegion().front());
        auto loaded =
            widen(memref::LoadOp::create(builder, loc(), source.data, ValueRange{index(offset)}));
        scf::YieldOp::create(builder, loc(), ValueRange{loaded});
        builder.setInsertionPointToStart(&branch.getElseRegion().front());
        scf::YieldOp::create(builder, loc(), ValueRange{c(0)});
      }
      out.bits = branch.getResult(0);
    } else if (auto operation = std::get_if<FunctionCall>(&node.operation)) {
      std::vector<DeviceValue> arguments;
      for (auto value : operation->arguments)
        arguments.push_back(evaluate(frame, value));
      out = call(frame, operation->function, arguments, id);
    } else if (auto operation = std::get_if<FunctionReference>(&node.operation)) {
      out.function = operation->function;
      for (auto value : operation->captures)
        out.elements.push_back(evaluate(frame, value));
    } else if (auto operation = std::get_if<FunctionInvoke>(&node.operation)) {
      auto function = evaluate(frame, operation->callee);
      if (!function.function)
        reject(id, "cooperative helper has no established function provenance");
      std::vector<DeviceValue> arguments;
      for (auto value : operation->arguments)
        arguments.push_back(evaluate(frame, value));
      arguments.insert(arguments.end(), function.elements.begin(), function.elements.end());
      out = call(frame, *function.function, arguments, id);
    } else if (auto operation = std::get_if<RecordValue>(&node.operation)) {
      out = operation->base ? evaluate(frame, *operation->base) : zero(node.type, id);
      if (operation->variant) {
        if (out.elements.empty())
          reject(id, "cooperative enum has no private tag field");
        out.elements.front().bits = c(*operation->variant + 1);
      }
      for (const auto &field : operation->fields)
        out.elements.at(field.field) = evaluate(frame, field.value);
    } else if (auto operation = std::get_if<ErrorNarrow>(&node.operation)) {
      auto value = evaluate(frame, operation->input);
      auto narrowed = persist(zero(node.type, id));
      when(running(frame), [&] { assign(narrowed, value, id); });
      out = materialize(narrowed);
    } else if (auto operation = std::get_if<RecordFieldRead>(&node.operation)) {
      out = evaluate(frame, operation->input).elements.at(operation->field);
    } else if (auto operation = std::get_if<FailOperation>(&node.operation)) {
      auto error = evaluate(frame, operation->error);
      if (!frame.failed)
        reject(id, "cooperative failure is outside a declared error channel");
      when(running(frame), [&] {
        assign(frame.error, error, id);
        store(c(1), frame.failed);
      });
      out = zero(node.type, id);
    } else if (auto operation = std::get_if<TaskOperation>(&node.operation);
               operation && operation->prefix == TaskPrefix::Try && !node.type.task) {
      out = evaluate(frame, operation->operand);
      if (!out.failed || out.errors.size() != 1 || !frame.failed)
        reject(id, "cooperative try requires matching application-error storage");
      when(both(running(frame), nonzero(out.failed)), [&] {
        assign(frame.error, out.errors.front(), id);
        store(c(1), frame.failed);
      });
      out.failed = {};
      out.failureSlot = {};
      out.errors.clear();
    } else if (auto operation = std::get_if<CatchOperation>(&node.operation)) {
      auto value = evaluate(frame, operation->operand);
      if (!value.failed || value.errors.size() != 1)
        reject(id, "cooperative catch requires a checked application-error channel");
      auto result = persist(zero(node.type, id));
      auto saved = frame;
      when(nonzero(value.failed), [&] {
        frame = saved;
        bind(frame, operation->parameter, value.errors.front());
        suite(frame, operation->body);
        assign(result, evaluate(frame, operation->result), id);
      });
      frame = saved;
      when(negate(nonzero(value.failed)), [&] {
        value.failed = {};
        value.failureSlot = {};
        value.errors.clear();
        assign(result, value, id);
      });
      frame = std::move(saved);
      out = materialize(std::move(result));
    } else if (auto operation = std::get_if<GuardOperation>(&node.operation)) {
      auto condition = evaluate(frame, operation->condition);
      auto saved = frame;
      when(both(running(frame), nonzero(condition.bits)), [&] { suite(frame, operation->body); });
      frame = std::move(saved);
      out.bits = c(0);
    } else if (auto operation = std::get_if<MatchOperation>(&node.operation)) {
      auto subject = evaluate(frame, operation->subject);
      if (subject.elements.empty() || !subject.elements.front().bits)
        reject(id, "cooperative match requires a checked private enum tag");
      auto result = persist(zero(node.type, id));
      auto matched = c(0, 1);
      auto saved = frame;
      for (const auto &arm : operation->cases) {
        auto selected =
            arm.tag ? cmp(arith::CmpIPredicate::eq, subject.elements.front().bits, c(*arm.tag))
                    : c(1, 1);
        when(both(negate(matched), selected), [&] {
          frame = saved;
          suite(frame, arm.body);
          assign(result, evaluate(frame, arm.result), id);
        });
        matched = either(matched, selected);
      }
      frame = std::move(saved);
      checked(matched);
      out = materialize(std::move(result));
    } else if (auto operation = std::get_if<ConditionalOperation>(&node.operation)) {
      auto condition = evaluate(frame, operation->condition);
      auto saved = frame;
      auto result = persist(zero(node.type, id));
      auto branch = scf::IfOp::create(builder, loc(), nonzero(condition.bits), true);
      auto arm = [&](Region &region, const std::vector<TypedStatement> &body, ValueId value) {
        builder.setInsertionPointToStart(&region.front());
        frame = saved;
        suite(frame, body);
        assign(result, evaluate(frame, value), id);
      };
      {
        OpBuilder::InsertionGuard guard(builder);
        arm(branch.getThenRegion(), operation->whenTrue, operation->trueResult);
        arm(branch.getElseRegion(), operation->whenFalse, operation->falseResult);
      }
      frame = std::move(saved);
      out = materialize(std::move(result));
    } else if (auto operation = std::get_if<LoopTransferOperation>(&node.operation)) {
      auto value = operation->value ? evaluate(frame, *operation->value) : literal(0);
      if (!frame.results.contains(operation->loop))
        reject(id, "cooperative control transfer has no enclosing loop");
      when(running(frame), [&] {
        assign(frame.results.at(operation->loop), value, id);
        store(c(operation->continuing ? -std::int64_t(operation->loop + 1)
                                      : std::int64_t(operation->loop + 1)),
              frame.control);
      });
      out.bits = c(0);
    } else if (auto operation = std::get_if<WhileOperation>(&node.operation)) {
      auto result = persist(zero(node.type, id));
      frame.results[operation->identity] = result;
      suite(frame, operation->initial);
      auto saved = frame;
      auto loop = scf::WhileOp::create(builder, loc(), TypeRange{}, ValueRange{});
      {
        OpBuilder::InsertionGuard guard(builder);
        auto *before = builder.createBlock(&loop.getBefore());
        builder.setInsertionPointToStart(before);
        auto prior = load(frame.control);
        store(
            select(cmp(arith::CmpIPredicate::eq, prior, c(-std::int64_t(operation->identity + 1))),
                   c(0), prior),
            frame.control);
        frame.values.clear();
        frame.completed.clear();
        auto condition = evaluate(frame, operation->condition);
        auto code = load(frame.control);
        auto permitted =
            either(cmp(arith::CmpIPredicate::eq, code, c(0)),
                   cmp(arith::CmpIPredicate::eq, code, c(-std::int64_t(operation->identity + 1))));
        auto healthy =
            frame.failed ? cmp(arith::CmpIPredicate::eq, load(frame.failed), c(0)) : c(1, 1);
        scf::ConditionOp::create(
            builder, loc(), both(both(nonzero(condition.bits), permitted), healthy), ValueRange{});
        auto *after = builder.createBlock(&loop.getAfter());
        builder.setInsertionPointToStart(after);
        store(c(0), frame.control);
        frame.values.clear();
        frame.completed.clear();
        suite(frame, operation->iteration);
        suite(frame, operation->body);
        scf::YieldOp::create(builder, loc());
      }
      frame = saved;
      frame.values.clear();
      when(running(frame), [&] {
        suite(frame, operation->whenDone);
        if (operation->normalResult)
          assign(result, evaluate(frame, *operation->normalResult), id);
      });
      when(cmp(arith::CmpIPredicate::eq, load(frame.control), c(operation->identity + 1)),
           [&] { store(c(0), frame.control); });
      frame.values.clear();
      out = materialize(std::move(result));
    } else if (auto operation = std::get_if<IntegerRangeOperation>(&node.operation)) {
      auto current = evaluate(frame, operation->current), stop = evaluate(frame, operation->stop),
           stride = evaluate(frame, operation->step);
      auto positive = cmp(arith::CmpIPredicate::sgt, stride.bits, c(0));
      if (operation->kind == IntegerRangeOperation::Kind::Validate) {
        checked(nonzero(stride.bits));
        out = current;
      } else if (operation->kind == IntegerRangeOperation::Kind::Test) {
        out.bits = widen(select(positive, cmp(arith::CmpIPredicate::slt, current.bits, stop.bits),
                                cmp(arith::CmpIPredicate::sgt, current.bits, stop.bits)));
      } else {
        auto sum = *emitCheckedGpuInteger(builder, loc(), NumericType::Int64,
                                          GpuIntegerOperation::Add, current.bits, stride.bits);
        out.bits = select(sum.valid, sum.bits, stop.bits);
      }
    } else if (auto operation = std::get_if<ShapeCheck>(&node.operation))
      out = evaluate(frame, operation->input);
    else {
      std::optional<std::uint64_t> bits;
      std::visit(
          [&](const auto &operation) {
            using T = std::decay_t<decltype(operation)>;
            if constexpr (std::is_same_v<T, Float32Literal> || std::is_same_v<T, Float64Literal>)
              bits = operation.bits;
            else if constexpr (std::is_same_v<T, Int32Literal> ||
                               std::is_same_v<T, UInt32Literal> ||
                               std::is_same_v<T, Int64Literal> || std::is_same_v<T, UInt64Literal>)
              bits = operation.value;
            else if constexpr (std::is_same_v<T, BoolLiteral>)
              bits = operation.value;
          },
          node.operation);
      if (!bits)
        reject(id, "cooperative native shader operation is not implemented");
      const auto width = node.type.builtin == BuiltinValue::Numeric
                             ? describeNumeric(node.type.element)->bits
                             : 64;
      out.bits = c(*bits & (width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1));
      if (node.type.element == NumericType::Int64 && node.type.builtin == BuiltinValue::Numeric)
        out.constant = std::bit_cast<std::int64_t>(*bits);
    }
    out.type = node.type;
    frame.values[id] = out;
    return out;
  }

public:
  Emitter(const TypedModule &module, const WorkgroupPlan &proof,
          const NumericShaderOptions &settings, gpu::GPUFuncOp function, FunctionId root,
          OpBuilder at, unsigned parameterWords)
      : input(module), plan(proof), options(settings), kernel(function), builder(at),
        words(parameterWords) {
    parameters = kernel.getArguments().back();
    output = kernel.getArgument(kernel.getNumArguments() - 2);
    fault = privateSlot();
    store(c(1), fault);
    block = arith::IndexCastUIOp::create(builder, loc(), builder.getI64Type(),
                                         gpu::BlockIdOp::create(builder, loc(), gpu::Dimension::x));
    thread =
        arith::IndexCastUIOp::create(builder, loc(), builder.getI64Type(),
                                     gpu::ThreadIdOp::create(builder, loc(), gpu::Dimension::x));
    unsigned word = 1;
    for (std::size_t axis = 0; axis < plan.launch.groupShape.size(); ++axis) {
      dimensions.push_back(word64(word));
      word += 2;
    }
    auto rank = dimensions.size();
    globals.resize(rank);
    locals.resize(rank);
    auto groupId = block, laneId = thread;
    for (std::size_t axis = rank; axis-- > 0;) {
      auto groupWidth = c(plan.launch.groupShape[axis]);
      auto groups =
          arith::DivUIOp::create(builder, loc(),
                                 arith::AddIOp::create(builder, loc(), dimensions[axis],
                                                       c(plan.launch.groupShape[axis] - 1)),
                                 groupWidth);
      locals[axis] = arith::RemUIOp::create(builder, loc(), laneId, groupWidth);
      globals[axis] = arith::AddIOp::create(
          builder, loc(),
          arith::MulIOp::create(
              builder, loc(), arith::RemUIOp::create(builder, loc(), groupId, groups), groupWidth),
          locals[axis]);
      laneId = arith::DivUIOp::create(builder, loc(), laneId, groupWidth);
      groupId = arith::DivUIOp::create(builder, loc(), groupId, groups);
    }
    for (std::size_t i = 0; i < plan.launch.inputRanks.size(); ++i) {
      DeviceValue value;
      value.data = kernel.getArgument(i);
      for (unsigned axis = 0; axis < plan.launch.inputRanks[i]; ++axis) {
        value.shape.push_back(word64(word));
        word += 2;
      }
      tensors.push_back(std::move(value));
    }
    for (auto parameter : input.functions.at(root).parameters) {
      const auto &type = input.symbols[parameter].type;
      if (type.tensor)
        continue;
      const auto numeric = type.builtin == BuiltinValue::Bool ? NumericType::UInt8 : type.element;
      auto value = scalar(word64(word, describeNumeric(numeric)->bits == 64), numeric);
      value.type = type;
      word += describeNumeric(numeric)->bits == 64 ? 2 : 1;
      scalars.push_back(value);
    }
  }
  void run() {
    // Vulkan can round a large linear group count into a physical rectangle.
    // Skip only entire extra groups, uniformly; logical padding lanes still run.
    when(cmp(arith::CmpIPredicate::ult, block, word64(0, false)), [&] {
      std::vector<DeviceValue> arguments(1);
      arguments[0].type = input.symbols[input.functions[plan.callback].parameters[0]].type;
      arguments[0].bits = c(0);
      for (const auto &source : plan.captures) {
        auto value = capture(source, plan.dispatch);
        value.type = source.type;
        arguments.push_back(std::move(value));
      }
      auto result = invoke(plan.callback, arguments, plan.dispatch);
      if (result.failed) {
        if (!plan.launch.errorWords || result.errors.size() != 1)
          reject(plan.dispatch, "cooperative expected-error export has no matching envelope");
        std::vector<Value> leaves;
        auto error = materialize(result.errors.front());
        if (!plan.errorFamily)
          reject(plan.dispatch, "cooperative expected-error export requires a declared family");
        if (error.type.record != plan.errorFamily) {
          ValueType type;
          type.builtin = BuiltinValue::Record;
          type.record = plan.errorFamily;
          auto converted = persist(zero(type, plan.dispatch));
          when(nonzero(result.failed), [&] { assign(converted, error, plan.dispatch); });
          error = materialize(converted);
        }
        errorLeaves(error, leaves);
        if (leaves.size() != plan.launch.errorWords)
          reject(plan.dispatch,
                 "cooperative expected-error payload differs from its export layout");
        const auto begin = words - 2 - 2 * plan.launch.errorWords;
        when(nonzero(result.failed), [&] {
          // Only failing invocations contend. Atomic OR elects exactly one
          // writer without spinning or depending on invocation count/order.
          // No peer reads the payload; the host waits for the entire dispatch.
          auto previous = memref::AtomicRMWOp::create(builder, loc(), arith::AtomicRMWKind::ori,
                                                      c(1, 32), parameters, ValueRange{at(begin)});
          when(cmp(arith::CmpIPredicate::eq, previous, c(0, 32)), [&] {
            for (std::size_t i = 0; i < leaves.size(); ++i) {
              store(narrow(leaves[i], 32), parameters, begin + 1 + 2 * i);
              store(narrow(arith::ShRUIOp::create(builder, loc(), leaves[i], c(32)), 32),
                    parameters, begin + 2 + 2 * i);
            }
          });
        });
      }
      auto activeLane = c(1, 1), offset = c(0);
      for (std::size_t axis = 0; axis < dimensions.size(); ++axis) {
        activeLane =
            both(activeLane, cmp(arith::CmpIPredicate::ult, globals[axis], dimensions[axis]));
        offset = arith::AddIOp::create(
            builder, loc(), arith::MulIOp::create(builder, loc(), offset, dimensions[axis]),
            globals[axis]);
      }
      when(activeLane, [&] {
        memref::StoreOp::create(builder, loc(),
                                narrow(result.bits, describeNumeric(result.type.element)->bits),
                                output, ValueRange{index(offset)});
      });
      when(cmp(arith::CmpIPredicate::eq, load(fault), c(0)), [&] {
        memref::AtomicRMWOp::create(builder, loc(), arith::AtomicRMWKind::minu, c(0, 32),
                                    parameters, ValueRange{at(words - 1)});
      });
    });
  }
};
} // namespace

std::expected<NumericShader, Diagnostic> emitWorkgroupShader(const TypedModule &input,
                                                             FunctionId function,
                                                             const NumericShaderOptions &options) {
  auto plan = planWorkgroup(input, function);
  if (!plan)
    return std::unexpected(plan.error());
  const auto &root = input.functions.at(function);
  NumericShader result{root.resultType.element, {}, {}};
  result.workgroup = plan->launch;
  bool narrow8 = describeNumeric(result.element)->bits == 8,
       narrow16 = describeNumeric(result.element)->bits == 16;
  for (auto parameter : root.parameters) {
    const auto &type = input.symbols[parameter].type;
    if (type.tensor) {
      result.tensorTypes.push_back(type.element);
      result.scalarBuffers.push_back(0);
      result.reads.push_back(result.reads.size());
      narrow8 |= describeNumeric(type.element)->bits == 8;
      narrow16 |= describeNumeric(type.element)->bits == 16;
    } else
      result.scalarTypes.push_back(type.builtin == BuiltinValue::Bool ? NumericType::UInt8
                                                                      : type.element);
  }
  const auto words = workgroupParameterWords(*result.workgroup, result.scalarTypes);
  if (!words)
    return std::unexpected(Diagnostic{
        "G001", "cooperative launch parameter layout exceeds its limits", root.declaration});
  std::uint32_t local = 1;
  for (auto extent : plan->launch.groupShape)
    local *= extent;
  std::ostringstream source;
  source << "module attributes {gpu.container_module, spirv.target_env = "
            "#spirv.target_env<#spirv.vce<v1.0, [Shader, Int64";
  if (narrow8)
    source << ", Int8, StorageBuffer8BitAccess";
  if (narrow16)
    source << ", Int16, StorageBuffer16BitAccess";
  source << "], [SPV_KHR_storage_buffer_storage_class";
  if (narrow8)
    source << ", SPV_KHR_8bit_storage";
  if (narrow16)
    source << ", SPV_KHR_16bit_storage";
  source << "]>, #spirv.resource_limits<>>} { gpu.module @kernels { gpu.func @"
         << workgroupKernelName(result.element, result.tensorTypes, result.scalarTypes,
                                *result.workgroup)
         << "(";
  for (std::size_t i = 0; i < result.tensorTypes.size(); ++i)
    source << "%input" << i << ":memref<?xi" << describeNumeric(result.tensorTypes[i])->bits
           << ">,";
  source
      << "%output:memref<?xi" << describeNumeric(result.element)->bits << ">,%parameters:memref<"
      << *words
      << "xi32>) kernel attributes {spirv.entry_point_abi=#spirv.entry_point_abi<workgroup_size=["
      << local
      << ",1,1]>} {\n"
         "%placeholder=arith.constant 0:i64 loc(\"cooperative.entry\")\ngpu.return\n} } }";
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  context.loadAllAvailableDialects();
  auto module = parseSourceString<ModuleOp>(source.str(), &context);
  if (!module)
    return std::unexpected(
        Diagnostic{"G001", "cooperative shader skeleton failed validation", root.declaration});
  gpu::GPUFuncOp kernel;
  arith::ConstantOp entry;
  module->walk([&](gpu::GPUFuncOp operation) { kernel = operation; });
  module->walk([&](arith::ConstantOp operation) {
    if (auto name = dyn_cast<NameLoc>(operation.getLoc());
        name && name.getName() == "cooperative.entry")
      entry = operation;
  });
  try {
    Emitter emitter(input, *plan, options, kernel, function, OpBuilder(entry), *words);
    emitter.run();
    entry.erase();
    if (failed(verify(*module)))
      return std::unexpected(
          Diagnostic{"G001", "cooperative shader failed IR verification", root.declaration});
    llvm::raw_string_ostream output(result.mlir);
    module->print(output);
    return result;
  } catch (const Rejected &error) {
    return std::unexpected(error.diagnostic);
  }
}
} // namespace kalandra
