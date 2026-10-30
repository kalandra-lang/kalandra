#include "Tasks.h"
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <thread>
using namespace kalandra::runtime;
static_assert(sizeof(Failure) == 32 && offsetof(Failure, code) == 8);
static_assert(sizeof(TaskHandle) == 16 && sizeof(GroupHandle) == 16);
static_assert(sizeof(Packet) == 32 && offsetof(Packet, source) == 24);
static_assert(sizeof(TaskEntry) == 80 && sizeof(ValueLayout) == 40);
extern "C" {
bool kalandra_task_cancelled_v1(Task *task) noexcept { return task->cancelled(); }
void *kalandra_task_frame_v1(Task *task) noexcept { return task->frame(); }
Invocation *kalandra_task_context_v1(Task *task) noexcept { return &task->context(); }
void kalandra_task_open_v1(Task *task, const GroupHandle *parent, GroupHandle *group) noexcept {
  *group = task->openGroup(parent ? *parent : GroupHandle{});
}
void kalandra_task_spawn_v1(Task *task, const GroupHandle *group, const TaskEntry *entry,
                            const Packet *packet, TaskHandle *handle) noexcept {
  *handle = task->spawn(*group, *entry, *packet);
}
bool kalandra_task_await_v1(Task *task, const TaskHandle *handle) noexcept {
  return task->await(*handle);
}
bool kalandra_task_join_v1(Task *task, const GroupHandle *group) noexcept {
  return task->join(*group);
}
bool kalandra_task_release_group_v1(Task *task, const GroupHandle *group) noexcept {
  if (task->releaseGroup(*group))
    return true;
  task->context().fault(3);
  return false;
}
bool kalandra_task_read_v1(Task *task, const TaskHandle *handle, const ValueLayout *layout,
                           void *out) noexcept {
  return task->read(*handle, *layout, out);
}
void kalandra_task_failure_v1(Task *task, const TaskHandle *handle, Failure *out) noexcept {
  *out = task->failure(*handle);
}
void kalandra_group_failure_v1(Task *task, const GroupHandle *group, Failure *out) noexcept {
  *out = task->failure(*group);
}
void kalandra_task_close_v1(Task *task, const GroupHandle *group) noexcept {
  (void)task->close(*group);
}
bool kalandra_task_stop_group_v1(Task *task, const GroupHandle *group) noexcept {
  return task->stopGroup(*group);
}
void kalandra_task_fail_group_v1(Task *task, const GroupHandle *group,
                                 const Failure *failure) noexcept {
  (void)task->failGroup(*group, *failure);
}
void kalandra_task_propagate_v1(Task *task, const Failure *failure) noexcept {
  task->fail(*failure);
}
void kalandra_task_complete_v1(Task *task, const void *value) noexcept { task->complete(value); }
void kalandra_task_fault_v1(Task *task, std::int64_t code, std::int64_t line,
                            std::int64_t column) noexcept {
  task->fault(code, line, column);
}
void kalandra_run_task_v1(const TaskEntry *entry, const Packet *packet, void *output,
                          Invocation *context, Failure *failure) noexcept {
  TaskLimits limits;
  limits.workers = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
  if (const auto *configured = std::getenv("KALANDRA_WORKERS")) {
    char *end = nullptr;
    const auto count = std::strtoul(configured, &end, 10);
    if (end == configured || *end || count < 1 || count > 64) {
      *failure = {TaskFailure::AdmissionRejected};
      return;
    }
    limits.workers = static_cast<std::uint32_t>(count);
  }
  auto core = TaskCore::create(limits);
  if (!core) {
    *failure = {TaskFailure::Fault, 2};
    return;
  }
  auto group = core->openRoot();
  auto task = core->submit(group, *entry, *packet);
  if (!task || !core->drain()) {
    *failure = {TaskFailure::Fault, 3};
    return;
  }
  *failure = core->failure(group);
  if (failure->kind == TaskFailure::None)
    *failure = core->failure(task);
  if (failure->kind == TaskFailure::None && !core->observe(task, *entry->result, *context, output))
    *failure = {TaskFailure::Fault, context->status(), context->line(), context->column()};
  core->close(group);
  if (!core->drain()) {
    *failure = {TaskFailure::Fault, 3};
    return;
  }
  if (failure->kind == TaskFailure::None)
    *failure = core->failure(group);
  core->release(group);
}
}
