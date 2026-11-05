#pragma once
#include "kalandra/Continuation.h"
#include <expected>
#include <optional>
#include <span>

namespace kalandra {
struct DeviceCallFrame {
  enum Field : std::size_t { Parent, ReturnPC, ReturnSlot, SuspendedAt, HeaderWords };
  std::vector<std::size_t> offsets;
  std::size_t words;
};

// Frame coordinates are relative to a single allocation. The caller's frame
// stays linked while the callee is active. Its return PC selects the live roots.
template <class Width>
std::expected<DeviceCallFrame, std::string>
planDeviceCallFrame(const continuation::Plan &plan, std::size_t capacity, Width width) {
  if (capacity < DeviceCallFrame::HeaderWords)
    return std::unexpected("GPU call frame exceeds the device word budget");
  DeviceCallFrame frame{{}, DeviceCallFrame::HeaderWords};
  if (plan.slots.size() > capacity - frame.words)
    return std::unexpected("GPU call frame exceeds the device word budget");
  frame.offsets.reserve(plan.slots.size());
  for (const auto &type : plan.slots) {
    const std::optional<std::size_t> words = width(type);
    if (!words || !*words)
      return std::unexpected("GPU call frame has an unsupported slot type");
    if (*words > capacity - frame.words)
      return std::unexpected("GPU call frame exceeds the device word budget");
    frame.offsets.push_back(frame.words);
    frame.words += *words;
  }
  return frame;
}
struct DeviceCallDispatch {
  // Zero is reserved for entry initialization. Each function owns a disjoint
  // interval of global PCs; local branches stay inside that interval.
  std::vector<std::size_t> entries;
  std::size_t nextPC = 1;
};

inline std::expected<DeviceCallDispatch, std::string>
planDeviceCallDispatch(const continuation::ModulePlan &module, std::size_t firstReservedPC) {
  using C = continuation::Code;
  DeviceCallDispatch dispatch{std::vector<std::size_t>(module.plans.size())};
  for (std::size_t fn = 0; fn < module.plans.size(); ++fn) {
    if (!module.plans[fn] || module.plans[fn]->code.empty())
      continue;
    const auto count = module.plans[fn]->code.size();
    if (dispatch.nextPC >= firstReservedPC || count > firstReservedPC - dispatch.nextPC)
      return std::unexpected("GPU call dispatch exceeds the device PC budget");
    dispatch.entries[fn] = dispatch.nextPC;
    dispatch.nextPC += count;
    for (std::size_t pc = 0; pc < count; ++pc) {
      const auto &op = module.plans[fn]->code[pc];
      if (op.code != C::SynchronousCall)
        continue;
      if (pc + 1 == count || op.target >= module.plans.size() ||
          !module.plans[op.target] || module.plans[op.target]->code.empty() ||
          op.args.size() != module.plans[op.target]->parameters.size() ||
          op.output >= module.plans[fn]->slots.size())
        return std::unexpected("GPU call dispatch has an invalid continuation or callee");
    }
  }
  return dispatch;
}
} // namespace kalandra
