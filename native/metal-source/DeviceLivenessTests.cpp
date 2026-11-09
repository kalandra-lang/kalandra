#include "DeviceLiveness.h"
#include "DeviceCallSchedule.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::continuation;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
auto roots(const Plan &plan) {
  const std::array<Slot, 3> tracked{0, 1, 2};
  auto result = deviceLiveRoots(plan, tracked);
  if (!result)
    throw std::runtime_error(result.error());
  return *result;
}
// Independent path oracle: a root is live if some path reads its old value
// before overwriting it. This does not use the production fixed-point solver.
void randomGraphs() {
  std::uint32_t seed = 0x279;
  const auto random = [&] {
    seed = seed * 1664525u + 1013904223u;
    return seed;
  };
  for (unsigned trial = 0; trial < 60; ++trial) {
    Plan p;
    p.slots.resize(4);
    for (unsigned i = 0; i < 20; ++i) {
      const auto output = random() % 4, arg = random() % 4;
      switch (random() % 5) {
      case 0:
        p.code.push_back({Code::Kernel, output, {}});
        break;
      case 1:
        p.code.push_back({Code::Copy, output, {arg}});
        break;
      case 2:
        p.code.push_back({Code::Branch, 0, {arg}, 0, 0, random() % 20});
        break;
      case 3:
        p.code.push_back({Code::Jump, 0, {}, 0, 0, random() % 20});
        break;
      default:
        p.code.push_back({Code::Field, output, {arg}});
        break;
      }
    }
    p.code.push_back({Code::Return, 0, {random() % 4}});
    const auto actual = roots(p);
    for (unsigned first = 0; first < p.code.size(); ++first)
      for (Slot root = 0; root < 3; ++root) {
        std::vector<unsigned> pending{first};
        std::vector<bool> visited(p.code.size());
        bool live = false;
        while (!pending.empty()) {
          const auto pc = pending.back();
          pending.pop_back();
          if (visited[pc])
            continue;
          visited[pc] = true;
          const auto &op = p.code[pc];
          if (std::find(op.args.begin(), op.args.end(), root) != op.args.end()) {
            live = true;
            break;
          }
          if ((op.code == Code::Kernel || op.code == Code::Copy || op.code == Code::Field) &&
              op.output == root)
            continue;
          if (op.code == Code::Branch || op.code == Code::Jump)
            pending.push_back(op.jump);
          if (op.code != Code::Jump && op.code != Code::Return && pc + 1 < p.code.size())
            pending.push_back(pc + 1);
        }
        check(live == (std::find(actual[first].begin(), actual[first].end(), root) !=
                       actual[first].end()),
              "device root fixed point differs from independent path oracle");
      }
  }
}
} // namespace
int main() {
  try {
    Plan p;
    p.slots.resize(4);
    p.code = {{Code::Kernel, 0, {}},
              {Code::Copy, 1, {0}},
              {Code::Kernel, 0, {}},
              {Code::Field, 3, {1}},
              {Code::Return, 0, {3}}};
    auto r = roots(p);
    check(r[0].empty() && r[1] == std::vector<Slot>{0} && r[2] == std::vector<Slot>{1} &&
              r[3] == std::vector<Slot>{1} && r[4].empty(),
          "overwritten array/dead frame cells retained or live alias lost");
    // The overwritten slot must stay rooted when allocation reads its old value.
    p.code[2].args = {0};
    r = roots(p);
    check(r[2] == (std::vector<Slot>{0, 1}), "allocation operand was not rooted");
    // Both branch arms and a backedge require a real fixed-point computation.
    p.code = {
        {Code::Kernel, 0, {}}, {Code::Kernel, 1, {}},           {Code::Branch, 0, {3}, 0, 0, 5},
        {Code::Copy, 2, {0}},  {Code::Jump, 0, {}, 0, 0, 6},    {Code::Copy, 2, {1}},
        {Code::Field, 3, {2}}, {Code::Branch, 0, {3}, 0, 0, 2}, {Code::Return, 0, {3}}};
    r = roots(p);
    check(r[2] == (std::vector<Slot>{0, 1}) && r[6] == (std::vector<Slot>{0, 1, 2}),
          "branch union or backedge root fixed point lost live values");
    p.code[7].jump = p.code.size();
    check(!deviceLiveRoots(p, std::array<Slot, 1>{0}), "invalid CFG target admitted");
    p.code[7].jump = 2;
    check(!deviceLiveRoots(p, std::array<Slot, 2>{0, 0}), "duplicate root admitted");
    check(!deviceLiveRoots(p, std::array<Slot, 1>{4}), "out-of-range root admitted");
    check(!deviceLiveRoots(p, std::array<Slot, 1>{0}, 1), "memory budget ignored");
    check(!deviceLiveRoots(p, std::array<Slot, 1>{0}, 4096, 1), "work budget ignored");
    p.code = {{Code::CollectInit, 0, {}},
              {Code::Kernel, 1, {}},
              {Code::CollectAppend, 0, {3}},
              {Code::CollectFinish, 0, {}},
              {Code::Return, 0, {0}}};
    r = roots(p);
    check(r[0].empty() && r[1] == std::vector<Slot>{0} && r[2] == std::vector<Slot>{0} &&
              r[3] == std::vector<Slot>{0},
          "collector's implicit live handle was not rooted during allocation");
    p.code[0].code = Code::Spawn;
    check(!deviceLiveRoots(p, std::array<Slot, 1>{0}), "unmodelled effect silently admitted");
    p.code = {
        {Code::MapInit, 0, {1, 3}}, {Code::MapCheck, 3, {1, 3}},  {Code::Branch, 0, {3}, 0, 0, 8},
        {Code::MapLoad, 3, {1, 3}}, {Code::Kernel, 2, {}},        {Code::MapStore, 0, {0, 3, 3}},
        {Code::MapAdvance, 3, {3}}, {Code::Jump, 0, {}, 0, 0, 1}, {Code::Return, 0, {0}}};
    r = roots(p);
    check(r[0] == std::vector<Slot>{1} && r[4] == (std::vector<Slot>{0, 1}) &&
              r[5] == (std::vector<Slot>{0, 1}) && r[8] == std::vector<Slot>{0},
          "map destination/input lost across allocating callback or loop backedge");
    p.code = {{Code::Catch, 1, {0, 2}, 2, 0, 1},
              {Code::Return, 0, {2}},
              {Code::Return, 0, {1}}};
    check(roots(p)[0] == std::vector<Slot>{0},
          "catch rooted a stale success or handler payload");
    p.code[1].args.push_back(1);
    check(roots(p)[0] == (std::vector<Slot>{0, 1}),
          "catch killed a value preserved along the handler edge");
    p.code = {{Code::Try, 1, {0}, 2, SIZE_MAX, 2},
              {Code::Return, 0, {1}},
              {Code::Return, 0, {2}}};
    check(roots(p)[0] == std::vector<Slot>{0}, "try rooted a stale outcome");
    p.code[0].code = Code::Fail;
    p.code[1].args.push_back(1);
    check(roots(p)[0] == std::vector<Slot>{0}, "fail retained an unreachable success edge");
    p.code[0].target = p.slots.size();
    check(!deviceLiveRoots(p, std::array<Slot, 1>{0}), "invalid error destination admitted");
    TypedModule input;
    ModulePlan module(input);
    auto entry = std::make_unique<Plan>();
    entry->slots.resize(1);
    entry->code = {{Code::Return, 0, {0}}};
    module.plans.push_back(std::move(entry));
    auto callee = std::make_unique<Plan>();
    callee->slots.resize(4);
    callee->result = 2;
    callee->code = {{Code::Try, 1, {0}, 2, SIZE_MAX, 3, 0, {}, {3}},
                    {Code::Catch, 1, {0, 3}, 3, 0, 2},
                    {Code::Fail, 1, {3}, 2, SIZE_MAX, 3},
                    {Code::Return, 0, {2}}};
    module.plans.push_back(std::move(callee));
    auto scheduled = scheduleDeviceCalls(module, 0, 20, 20);
    check(scheduled && scheduled->plan.code[1].target == 3 &&
              scheduled->plan.code[1].jump == 4 &&
              scheduled->plan.code[1].collections == std::vector<Slot>{4} &&
              scheduled->plan.code[2].target == 4 && scheduled->plan.code[2].jump == 3 &&
              scheduled->plan.code[3].target == 3 && scheduled->plan.code[3].jump == 4,
          "device schedule mixed up error destination slots and branch PCs");
    module.plans[1]->code[1] = {Code::Jump, 0, {}, 0, 0, 3, 0, {}, {3}};
    scheduled = scheduleDeviceCalls(module, 0, 20, 20);
    check(scheduled && scheduled->plan.code[2].jump == 4 &&
              scheduled->plan.code[2].collections == std::vector<Slot>{4},
          "device schedule lost an early return's abandoned collection");
    module.plans[1]->code[1].collections = {4};
    check(!scheduleDeviceCalls(module, 0, 20, 20), "invalid abandoned collection admitted");
    module.plans[1]->code[1].collections = {3};
    module.plans[1]->code[0].group = 0;
    check(!scheduleDeviceCalls(module, 0, 20, 20), "host group propagation admitted to device frame");
    randomGraphs();
    std::cout << "PASS device root maps: aliases, allocation operands, dead slots, CFG fixed "
                 "points and budgets\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
