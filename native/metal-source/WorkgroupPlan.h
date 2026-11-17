#pragma once
#include "kalandra/Semantics.h"
#include "kalandra/WorkgroupAnalysis.h"
#include "kalandra/WorkgroupLaunch.h"
namespace kalandra {
struct WorkgroupCapture {
  ValueType type;
  std::optional<std::uint32_t> tensor;
  WorkgroupShapeExpression scalar;
};
struct WorkgroupPlan {
  ValueId dispatch;
  FunctionId callback;
  WorkgroupLaunch launch;
  WorkgroupAnalysis proof;
  std::vector<WorkgroupCapture> captures;
};
bool containsWorkgroupDispatch(const TypedModule &, FunctionId);
std::expected<WorkgroupPlan, Diagnostic> planWorkgroup(const TypedModule &, FunctionId);
} // namespace kalandra
