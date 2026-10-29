#include "Metal.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <vector>
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Metal runtime assertion %d\n", __LINE__);                              \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
template <class P> void until(P predicate) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!predicate()) {
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::yield();
  }
}
void clean(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group));
  CHECK(core.drain());
  CHECK(core.settled(group));
  CHECK(core.release(group));
  auto s = core.stats();
  CHECK(!s.externalPending && !s.records && !s.groups && !s.retainedBytes && !s.retainedOwners);
}
m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  m::Buffer result = *value;
  value->~Buffer();
  return result;
}
m::Buffer host(m::Runtime &runtime, const std::vector<std::uint32_t> &data) {
  std::int64_t shape = static_cast<std::int64_t>(data.size());
  auto value = runtime.hostTensor(std::as_bytes(std::span(data)), {&shape, 1}, m::Format::Float32);
  CHECK(value);
  return *value;
}
void empty(m::Runtime &runtime) {
  auto s = runtime.stats();
  CHECK(!s.bytes && !s.hostBackings && !s.deviceBackings && !s.metalBuffers && !s.stagingBuffers &&
        !s.inFlight);
}
struct SupervisorPacket {
  m::Runtime *runtime;
  m::Buffer input;
  m::Device device;
};
struct SupervisorFrame {
  SupervisorPacket packet;
  GroupHandle group;
  TaskHandle child;
  m::Buffer result;
  unsigned phase = 0;
};
bool constructSupervisor(Invocation &, void *out, const Packet &packet) {
  new (out) SupervisorFrame{*static_cast<const SupervisorPacket *>(packet.data), {}, {}, {}, 0};
  return true;
}
void destroySupervisor(void *frame) noexcept {
  static_cast<SupervisorFrame *>(frame)->~SupervisorFrame();
}
bool readBuffer(Task &task, TaskHandle child, m::Buffer &output) {
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  if (!task.read(child, m::Runtime::bufferLayout(), slot))
    return false;
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  output = *value;
  value->~Buffer();
  return true;
}
void resumeSupervisor(Task &task) {
  auto &f = *static_cast<SupervisorFrame *>(task.frame());
  if (f.phase == 0) {
    f.group = task.openGroup();
    f.child = f.packet.runtime->upload(task, f.group, f.packet.input, f.packet.device);
    f.packet.input = {};
    f.phase = 1;
  }
  if (f.phase == 1) {
    if (!task.await(f.child))
      return;
    if (!readBuffer(task, f.child, f.result))
      return;
    f.child = f.packet.runtime->download(task, f.group, f.result);
    f.result = {};
    f.phase = 2;
  }
  if (f.phase == 2) {
    if (!task.await(f.child))
      return;
    if (!readBuffer(task, f.child, f.result))
      return;
    CHECK(task.close(f.group));
    f.phase = 3;
  }
  if (!task.join(f.group))
    return;
  task.complete(&f.result);
}
const TaskEntry supervisorEntry{1,
                                0x4d4554414c03,
                                sizeof(SupervisorPacket),
                                alignof(SupervisorPacket),
                                sizeof(SupervisorFrame),
                                alignof(SupervisorFrame),
                                constructSupervisor,
                                destroySupervisor,
                                resumeSupervisor,
                                &m::Runtime::bufferLayout()};
// Keep the full externally acknowledged transfer suite on the private fallback.
// Owned shared snapshots have their own bounded-copy lifecycle suite.
m::Options privateUploads() { m::Options options; options.sharedUploads = false; return options; }
int main() {
  auto made = m::Runtime::create(privateUploads());
  CHECK(made);
  auto runtime = std::move(*made);
  CHECK(runtime->deviceCount() > 0);
  auto chosen = runtime->device();
  CHECK(chosen);
  CHECK(chosen->identity() != 0 && !chosen->name().empty());
  CHECK(runtime->device(0)->identity() == chosen->identity());
  CHECK(!runtime->device(UINT32_MAX));
  m::Options invalid;
  invalid.defaultDevice = UINT32_MAX;
  CHECK(!m::Runtime::create(invalid));
  invalid = {};
  invalid.maxBytes = 0;
  CHECK(!m::Runtime::create(invalid));
  // Shape/byte checks precede resource allocation; zero extents suppress unused overflow.
  {
    std::array<std::int64_t, 2> overflow{INT64_MAX, INT64_MAX}, negative{-1, 0}, zero{0, INT64_MAX};
    CHECK(!runtime->hostTensor({}, overflow, m::Format::Float32));
    CHECK(!runtime->hostTensor({}, negative, m::Format::Float32));
    CHECK(runtime->hostTensor({}, zero, m::Format::Float32));
    std::int64_t shape = 2;
    CHECK(!runtime->hostTensor({}, {&shape, 1}, m::Format::Float32));
    std::array<std::int64_t, 9> rank{};
    CHECK(!runtime->hostTensor({}, rank, m::Format::UInt8));
    CHECK(!runtime->hostTensor({}, {}, static_cast<m::Format>(99)));
  }
  empty(*runtime);
  constexpr std::array<std::uint32_t, 12> bits = {0,          0x80000000, 1,          0x80000001,
                                                  0x007fffff, 0x00800000, 0x7f800000, 0xff800000,
                                                  0x7fc12345, 0x7f812345, 0x7f7fffff, 0xffffffff};
  for (unsigned workers : {1u, 2u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    auto core = TaskCore::create(limits);
    CHECK(core);
    for (unsigned n : {0u, 1u, 3u, 63u, 64u, 65u, 1025u, 8193u}) {
      std::vector<std::uint32_t> data(n);
      for (unsigned i = 0; i < n; ++i)
        data[i] = bits[i % bits.size()];
      auto original = data;
      auto input = host(*runtime, data);
      auto alias = input;
      auto group = core->openRoot();
      auto task = runtime->upload(*core, group, input, *chosen);
      input = {};
      CHECK(core->drain());
      auto device = observe(*core, task);
      CHECK(device.onDevice());
      CHECK(device.bytes() == data.size() * 4);
      CHECK(!device.hostBytes());
      clean(*core, group);
      // A result and aliased source survive their producing task group.
      CHECK(data.empty() ||
            std::memcmp(alias.hostBytes()->data(), data.data(), data.size() * 4) == 0);
      group = core->openRoot();
      task = runtime->download(*core, group, device);
      device = {};
      CHECK(core->drain());
      auto output = observe(*core, task);
      clean(*core, group);
      CHECK(!output.onDevice() && output.format() == m::Format::Float32);
      CHECK(output.shape().size() == 1 && output.shape()[0] == n);
      CHECK(output.hostBytes()->size() == data.size() * 4);
      if (n)
        CHECK(std::memcmp(output.hostBytes()->data(), data.data(), data.size() * 4) == 0);
      CHECK(data == original);
      output = {};
      alias = {};
      empty(*runtime);
    }
    // A CPU supervisor spawns, awaits and joins transfer children under a narrow ready queue.
    {
      TaskLimits nestedLimits;
      nestedLimits.workers = workers;
      nestedLimits.ready = 1;
      auto nested = TaskCore::create(nestedLimits);
      auto group = nested->openRoot();
      SupervisorPacket packet{runtime.get(), host(*runtime, {1, 0x80000000, 0x7f812345}), *chosen};
      auto parent = nested->submit(group, supervisorEntry,
                                   {supervisorEntry.packetLayout, &packet, sizeof(packet)});
      packet.input = {};
      CHECK(nested->drain());
      auto result = observe(*nested, parent);
      clean(*nested, group);
      CHECK(result.hostBytes()->size() == 12);
      result = {};
      empty(*runtime);
    }
    // Cancelling a CPU supervisor also waits for its pending device child before group cleanup.
    {
      TaskLimits nestedLimits;
      nestedLimits.workers = workers;
      nestedLimits.ready = 1;
      auto nested = TaskCore::create(nestedLimits);
      auto group = nested->openRoot();
      SupervisorPacket packet{runtime.get(), host(*runtime, {1, 2, 3}), *chosen};
      auto parent = nested->submit(group, supervisorEntry,
                                   {supervisorEntry.packetLayout, &packet, sizeof(packet)});
      packet.input = {};
      runtime->holdCompletions(true);
      CHECK(nested->step());
      CHECK(nested->step());
      CHECK(nested->stats().externalPending == 1);
      CHECK(nested->cancel(group));
      CHECK(!nested->drain(0));
      CHECK(!nested->settled(parent));
      runtime->holdCompletions(false);
      CHECK(nested->drain());
      clean(*nested, group);
      empty(*runtime);
    }
    // Pre-submit cancellation never submits a GPU command.
    {
      auto input = host(*runtime, {1, 2, 3});
      auto group = core->openRoot();
      auto before = runtime->stats().submissions;
      auto task = runtime->upload(*core, group, input, *chosen);
      CHECK(core->cancel(group));
      CHECK(core->drain());
      CHECK(core->failure(task).kind == TaskFailure::Cancelled);
      clean(*core, group);
      CHECK(runtime->stats().submissions == before);
    }
    empty(*runtime);
    // Keep command completion pending deterministically; cancellation must not publish/free early.
    for (bool deadline : {false, true}) {
      auto input = host(*runtime, {1, 2, 3});
      auto group = core->openRoot();
      runtime->holdCompletions(true);
      auto task = runtime->upload(*core, group, input, *chosen);
      CHECK(core->step());
      input = {};
      CHECK(runtime->stats().inFlight == 1 && core->stats().externalPending == 1);
      CHECK(core->cancel(group, deadline));
      CHECK(!core->drain(0));
      CHECK(!core->settled(task));
      CHECK(runtime->stats().metalBuffers > 0);
      runtime->holdCompletions(false);
      CHECK(core->drain());
      CHECK(core->failure(group).kind ==
            (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
      clean(*core, group);
      empty(*runtime);
    }
    // Failed completion is not a valid result; a fresh group remains usable.
    {
      auto input = host(*runtime, {1, 2, 3});
      auto group = core->openRoot();
      runtime->failNextCompletion();
      auto task = runtime->upload(*core, group, input, *chosen);
      CHECK(core->drain());
      CHECK(core->failure(task).code == std::int64_t(m::Error::Device));
      clean(*core, group);
    }
    empty(*runtime);
  }
  // Inject every logical allocation/encoder/admission point for upload and download.
  for (bool upload : {false, true}) {
    auto core = TaskCore::create();
    auto source = host(*runtime, {1, 2, 3, 4});
    if (!upload) {
      auto group = core->openRoot();
      auto task = runtime->upload(*core, group, source, *chosen);
      CHECK(core->drain());
      source = observe(*core, task);
      clean(*core, group);
    }
    auto baseline = runtime->stats();
    runtime->failAllocationAt(0);
    auto group = core->openRoot();
    auto task = upload ? runtime->upload(*core, group, source, *chosen)
                       : runtime->download(*core, group, source);
    CHECK(core->drain());
    auto value = observe(*core, task);
    auto attempts = runtime->stats().attempts;
    clean(*core, group);
    value = {};
    CHECK(attempts > 0);
    for (std::uint64_t fail = 1; fail <= attempts; ++fail) {
      runtime->failAllocationAt(fail);
      group = core->openRoot();
      task = upload ? runtime->upload(*core, group, source, *chosen)
                    : runtime->download(*core, group, source);
      CHECK(core->drain());
      CHECK(core->failure(task).code == std::int64_t(m::Error::Allocation));
      clean(*core, group);
      auto now = runtime->stats();
      CHECK(now.bytes == baseline.bytes && now.hostBackings == baseline.hostBackings &&
            now.deviceBackings == baseline.deviceBackings && !now.stagingBuffers && !now.inFlight);
    }
    runtime->failAllocationAt(0);
    source = {};
    empty(*runtime);
  }
  // Independent commands, cached outcome copies, and same-residency retained aliases.
  {
    TaskLimits limits;
    limits.workers = 4;
    auto core = TaskCore::create(limits);
    auto input = host(*runtime, {1, 2, 3, 4});
    auto group = core->openRoot();
    std::array<TaskHandle, 16> tasks;
    for (auto &task : tasks)
      task = runtime->upload(*core, group, input, *chosen);
    CHECK(core->drain());
    std::vector<m::Buffer> outputs;
    for (auto task : tasks) {
      outputs.push_back(observe(*core, task));
      outputs.push_back(observe(*core, task));
    }
    clean(*core, group);
    auto prior = runtime->stats().submissions;
    group = core->openRoot();
    auto same = runtime->upload(*core, group, outputs[0], *chosen);
    CHECK(core->drain());
    auto alias = observe(*core, same);
    clean(*core, group);
    CHECK(runtime->stats().submissions == prior);
    outputs.clear();
    alias = {};
    input = {};
    empty(*runtime);
  }
  // Scalar/ranked metadata and odd-sized byte transfers are independent of arithmetic support.
  for (auto format : {m::Format::UInt8, m::Format::UInt32}) {
    std::array<std::int64_t, 2> shape{1, 3};
    std::vector<std::byte> data(format == m::Format::UInt8 ? 3 : 12, std::byte{0xa5});
    auto input = runtime->hostTensor(data, shape, format);
    CHECK(input);
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto task = runtime->upload(*core, group, *input, *chosen);
    CHECK(core->drain());
    auto device = observe(*core, task);
    clean(*core, group);
    group = core->openRoot();
    task = runtime->download(*core, group, device);
    CHECK(core->drain());
    auto result = observe(*core, task);
    clean(*core, group);
    CHECK(result.format() == format && result.shape().size() == 2 && result.shape()[1] == 3);
    CHECK(std::memcmp(result.hostBytes()->data(), data.data(), data.size()) == 0);
  }
  empty(*runtime);
  // Every host backing allocation failure leaves no partially published storage.
  for (std::uint64_t fail : {1u, 2u}) {
    runtime->failAllocationAt(fail);
    std::array<std::uint32_t, 1> data{1};
    std::int64_t shape = 1;
    CHECK(!runtime->hostTensor(std::as_bytes(std::span(data)), {&shape, 1}, m::Format::UInt32));
    empty(*runtime);
  }
  runtime->failAllocationAt(0);
  // Shutdown cannot invalidate a pending callback; transfer packets own runtime state.
  {
    auto local = m::Runtime::create(privateUploads());
    CHECK(local);
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto input = host(**local, {1, 2, 3});
    auto task = (*local)->upload(*core, group, input, *(*local)->device());
    input = {};
    local->reset();
    CHECK(core->drain());
    auto retained = observe(*core, task);
    clean(*core, group);
    CHECK(retained.onDevice());
    auto other = m::Runtime::create(privateUploads());
    CHECK(other);
    group = core->openRoot();
    task = (*other)->download(*core, group, retained);
    CHECK(core->drain());
    CHECK(core->failure(task).code == std::int64_t(m::Error::Residency));
    clean(*core, group);
  }
  // Destroying a core with a committed transfer waits for the callback's acknowledgement.
  {
    auto local = m::Runtime::create(privateUploads());
    CHECK(local);
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto input = host(**local, {1, 2, 3});
    (*local)->holdCompletions(true);
    (*local)->upload(*core, group, input, *(*local)->device());
    CHECK(core->step());
    input = {};
    std::atomic<bool> dropping = false, done = false;
    std::jthread drop([&] {
      dropping = true;
      core.reset();
      done = true;
    });
    until([&] { return dropping.load(); });
    CHECK((*local)->stats().inFlight == 1);
    (*local)->holdCompletions(false);
    drop.join();
    CHECK(done);
    empty(**local);
  }
  // Byte and in-flight budgets reject transactionally, leaving the existing source intact.
  {
    m::Options options = privateUploads();
    options.maxBytes = 32;
    auto limited = m::Runtime::create(options);
    CHECK(limited);
    auto input = host(**limited, {1, 2, 3, 4});
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto task = (*limited)->upload(*core, group, input, *(*limited)->device());
    CHECK(core->drain());
    CHECK(core->failure(task).code == std::int64_t(m::Error::Budget));
    clean(*core, group);
    input = {};
    empty(**limited);
  }
  {
    m::Options options = privateUploads();
    options.maxInFlight = 1;
    auto limited = m::Runtime::create(options);
    CHECK(limited);
    auto input = host(**limited, {1, 2, 3});
    auto core = TaskCore::create();
    auto group = core->openRoot();
    (*limited)->holdCompletions(true);
    auto first = (*limited)->upload(*core, group, input, *(*limited)->device());
    CHECK(core->step());
    auto second = (*limited)->upload(*core, group, input, *(*limited)->device());
    CHECK(core->step());
    CHECK(core->failure(second).code == std::int64_t(m::Error::Budget));
    CHECK(!core->settled(first));
    (*limited)->holdCompletions(false);
    CHECK(core->drain());
    clean(*core, group);
    input = {};
    empty(**limited);
  }
  // Wrong-runtime device handles and forged-empty bridge values are rejected.
  {
    auto other = m::Runtime::create(privateUploads());
    CHECK(other);
    auto core = TaskCore::create();
    auto input = host(*runtime, {1});
    for (bool invalidInput : {false, true}) {
      auto group = core->openRoot();
      auto task =
          runtime->upload(*core, group, invalidInput ? m::Buffer{} : input, *(*other)->device());
      CHECK(core->drain());
      CHECK(core->failure(task).kind == TaskFailure::Fault);
      clean(*core, group);
    }
  }
  empty(*runtime);
  std::puts("Metal device, immutable transfers, external completion, budgets, and failure cleanup "
            "passed");
}
