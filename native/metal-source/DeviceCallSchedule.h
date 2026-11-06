#pragma once
#include "DeviceCallFrame.h"

namespace kalandra {
struct DeviceCallSchedule {
  continuation::Plan plan;
  DeviceCallDispatch dispatch;
  std::vector<FunctionId> owners;
  std::vector<std::vector<continuation::Slot>> parameters;
  std::vector<continuation::Slot> slotBases;
};

// Reuse the existing device instruction lowering with disjoint instruction
// and logical slot IDs. The emitter chooses physical addresses per activation.
inline std::expected<DeviceCallSchedule, std::string>
scheduleDeviceCalls(const continuation::ModulePlan &module, FunctionId entry,
                     std::size_t reservedPC, std::size_t maxSlots) {
  using C = continuation::Code;
  auto dispatch = planDeviceCallDispatch(module, reservedPC);
  if (!dispatch)
    return std::unexpected(dispatch.error());
  if (entry >= dispatch->entries.size() || !dispatch->entries[entry])
    return std::unexpected("missing GPU call schedule entry");
  DeviceCallSchedule schedule{{}, std::move(*dispatch), {}, {}, {}};
  schedule.parameters.resize(module.plans.size());
  schedule.slotBases.resize(module.plans.size());
  for (FunctionId fn = 0; fn < module.plans.size(); ++fn) {
    if (!schedule.dispatch.entries[fn])
      continue;
    const auto &source = *module.plans[fn];
    if (source.slots.empty() || source.slots.size() > maxSlots - schedule.plan.slots.size() ||
        source.result >= source.slots.size())
      return std::unexpected("GPU call schedule exceeds the logical slot budget");
    const auto base = schedule.plan.slots.size();
    const auto pc = schedule.dispatch.entries[fn] - 1;
    schedule.slotBases[fn] = base;
    schedule.plan.slots.insert(schedule.plan.slots.end(), source.slots.begin(), source.slots.end());
    auto &parameters = schedule.parameters[fn];
    for (auto parameter : source.parameters) {
      if (parameter >= source.slots.size())
        return std::unexpected("GPU call schedule has an invalid parameter slot");
      parameters.push_back(base + parameter);
    }
    if (fn == entry) {
      schedule.plan.parameters = parameters;
      schedule.plan.result = base + source.result;
    }
    for (auto op : source.code) {
      const bool propagates = op.code == C::Try || op.code == C::Fail;
      if (op.output >= source.slots.size() || op.placement ||
          (!propagates && !op.collections.empty()))
        return std::unexpected("GPU call schedule has an unsupported instruction operand");
      op.output += base;
      for (auto &arg : op.args) {
        if (arg >= source.slots.size())
          return std::unexpected("GPU call schedule has an invalid input slot");
        arg += base;
      }
      if (propagates) {
        if (op.target >= source.slots.size() || op.group != SIZE_MAX)
          return std::unexpected("GPU error propagation must stay within its device function");
        op.target += base;
        for (auto &collection : op.collections) {
          if (collection >= source.slots.size())
            return std::unexpected("GPU error propagation has an invalid collection slot");
          collection += base;
        }
      }
      if (op.code == C::Catch) {
        if (op.target >= source.code.size())
          return std::unexpected("GPU catch success branch leaves its function");
        op.target += pc;
      }
      if (op.code == C::Branch || op.code == C::Jump || (op.code == C::Case && op.jump != SIZE_MAX) ||
          propagates || op.code == C::Catch) {
        if (op.jump >= source.code.size())
          return std::unexpected("GPU call schedule branch leaves its function");
        op.jump += pc;
      }
      schedule.owners.push_back(fn);
      schedule.plan.code.push_back(std::move(op));
    }
  }
  return schedule;
}
} // namespace kalandra
