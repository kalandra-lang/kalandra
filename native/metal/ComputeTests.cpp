#include "../metal-proof/FloatOracle.h"
#include "Metal.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Compute assertion %d: %s\n", __LINE__, #x);                            \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)

m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  if (!core.observe(task, m::Runtime::bufferLayout(), context, slot)) {
    auto f = core.failure(task);
    std::fprintf(stderr, "Observation failed: kind=%d detail=%lld\n", int(f.kind),
                 (long long)f.code);
    std::abort();
  }
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
bool read(Task &task, TaskHandle child, m::Buffer &result) {
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  if (!task.read(child, m::Runtime::bufferLayout(), slot))
    return false;
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  result = *value;
  value->~Buffer();
  return true;
}
void clean(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group));
  CHECK(core.drain());
  CHECK(core.settled(group));
  CHECK(core.release(group));
  auto s = core.stats();
  CHECK(!s.records && !s.groups && !s.externalPending && !s.retainedBytes && !s.retainedOwners);
}
void empty(m::Runtime &r) {
  auto s = r.stats();
  CHECK(!s.bytes && !s.hostBackings && !s.deviceBackings && !s.metalBuffers && !s.stagingBuffers &&
        !s.inFlight);
}
m::Buffer host(m::Runtime &r, std::span<const std::uint32_t> bits,
               m::Format format = m::Format::Float32) {
  auto n = std::int64_t(bits.size());
  auto result = r.hostTensor(std::as_bytes(bits), {&n, 1}, format);
  CHECK(result);
  return *result;
}
m::Buffer upload(m::Runtime &r, m::Buffer buffer) {
  auto core = TaskCore::create();
  auto group = core->openRoot();
  auto t = r.upload(*core, group, buffer, *r.device());
  CHECK(core->drain());
  auto result = observe(*core, t);
  clean(*core, group);
  return result;
}
m::Buffer download(m::Runtime &r, m::Buffer buffer) {
  auto core = TaskCore::create();
  auto group = core->openRoot();
  auto t = r.download(*core, group, buffer);
  CHECK(core->drain());
  auto result = observe(*core, t);
  clean(*core, group);
  return result;
}
std::uint64_t comparisons = 0;
void check(m::Buffer result, std::span<const float_proof::Pair> pairs, unsigned operation) {
  CHECK(!result.onDevice() && result.bytes() == pairs.size() * 4);
  auto bytes = result.hostBytes();
  CHECK(bytes);
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    std::uint32_t bits;
    std::memcpy(&bits, bytes->data() + i * 4, 4);
    CHECK(float_proof::equal(bits, float_proof::oracle(pairs[i], operation)));
    ++comparisons;
  }
}
// A native CPU task performs the entire explicit transfer/compute/transfer chain.
// No controller drives a nested core or blocks a worker waiting for Metal.
struct PacketData {
  m::Runtime *runtime;
  m::Kernel kernel;
  m::Buffer a, b;
  m::BinaryOperation op;
};
struct Frame {
  PacketData packet;
  GroupHandle group;
  TaskHandle child;
  m::Buffer a, b, result;
  unsigned phase = 0;
};
bool construct(Invocation &, void *out, const Packet &p) {
  new (out) Frame{*static_cast<const PacketData *>(p.data), {}, {}, {}, {}, {}, 0};
  return true;
}
void destroy(void *p) noexcept { static_cast<Frame *>(p)->~Frame(); }
void resume(Task &task) {
  auto &f = *static_cast<Frame *>(task.frame());
  auto &r = *f.packet.runtime;
  if (!f.phase) {
    f.group = task.openGroup();
    if (f.packet.a.onDevice() && f.packet.b.onDevice()) {
      f.a = f.packet.a;
      f.b = f.packet.b;
      f.child = r.compute(task, f.group, f.packet.kernel, f.packet.op, f.a, f.b);
      f.phase = 3;
    } else {
      f.child = r.upload(task, f.group, f.packet.a, *r.device());
      f.phase = 1;
    }
  }
  if (f.phase == 1) {
    if (!task.await(f.child) || !read(task, f.child, f.a))
      return;
    f.child = r.upload(task, f.group, f.packet.b, *r.device());
    f.phase = 2;
  }
  if (f.phase == 2) {
    if (!task.await(f.child) || !read(task, f.child, f.b))
      return;
    f.child = r.compute(task, f.group, f.packet.kernel, f.packet.op, f.a, f.b);
    f.phase = 3;
  }
  if (f.phase == 3) {
    if (!task.await(f.child) || !read(task, f.child, f.result))
      return;
    CHECK(f.result.onDevice());
    f.child = r.download(task, f.group, f.result);
    f.result = {};
    f.phase = 4;
  }
  if (f.phase == 4) {
    if (!task.await(f.child) || !read(task, f.child, f.result))
      return;
    CHECK(task.close(f.group));
    f.phase = 5;
  }
  if (!task.join(f.group))
    return;
  task.complete(&f.result);
}
const TaskEntry supervisor{1,
                           0x4d4554434f01,
                           sizeof(PacketData),
                           alignof(PacketData),
                           sizeof(Frame),
                           alignof(Frame),
                           construct,
                           destroy,
                           resume,
                           &m::Runtime::bufferLayout()};

template <class Predicate> void until(Predicate predicate) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!predicate()) {
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::yield();
  }
}
int main(int argc, char **argv) {
  CHECK(argc == 3);
  auto made = m::Runtime::create();
  CHECK(made);
  auto r = std::move(*made);
  auto loaded = r->loadBinary32Kernel(argv[1], *r->device());
  CHECK(loaded);
  auto kernel = *loaded;
  CHECK(!r->loadBinary32Kernel("", *r->device()));
  CHECK(!r->loadBinary32Kernel("/not/a/kalandra/metallib", *r->device()));
  CHECK(!r->loadBinary32Kernel(argv[2], *r->device())); // Same entry, incompatible fixed-size ABI.
  auto other = m::Runtime::create();
  CHECK(other);
  CHECK(!r->loadBinary32Kernel(argv[1], *(*other)->device()));
  for (unsigned fail : {1u, 2u}) {
    r->failAllocationAt(fail);
    CHECK(!r->loadBinary32Kernel(argv[1], *r->device()));
  }
  r->failAllocationAt(0);
  auto corpus = float_proof::corpus();
  // Guarded partial workgroups, beyond the old fixed fixture, and the complete IEEE corpus.
  for (unsigned workers : {1u, 2u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    limits.ready = 1;
    auto core = TaskCore::create(limits);
    CHECK(core);
    for (std::size_t n : {0u, 1u, 2u, 63u, 64u, 65u, 4095u, 4096u, 4097u, 8193u}) {
      std::vector<std::uint32_t> a, b;
      for (std::size_t i = 0; i < n; ++i) {
        auto p = corpus[(i * 233 + 17) % corpus.size()];
        a.push_back(p.a);
        b.push_back(p.b);
      }
      std::vector<float_proof::Pair> pairs;
      for (std::size_t i = 0; i < n; ++i)
        pairs.push_back({a[i], b[i]});
      for (unsigned op = 0; op < 3; ++op) {
        auto group = core->openRoot();
        auto before = r->stats().submissions;
        PacketData packet{r.get(), kernel, host(*r, a), host(*r, b), m::BinaryOperation(op)};
        auto t =
            core->submit(group, supervisor, {supervisor.packetLayout, &packet, sizeof(packet)});
        CHECK(core->drain());
        auto result = observe(*core, t);
        check(result, pairs, op);
        auto again = observe(*core, t);
        CHECK(again.hostBytes()->data() == result.hostBytes()->data());
        CHECK(r->stats().submissions - before == (n ? (r->device()->hasUnifiedMemory() ? 2 : 4) : 0));
        clean(*core, group); // Result remains valid after producing group is gone.
        check(result, pairs, op);
      }
    }
  }
  for (std::size_t offset = 0; offset < corpus.size(); offset += 8193) {
    auto pairs =
        std::span(corpus).subspan(offset, std::min<std::size_t>(8193, corpus.size() - offset));
    std::vector<std::uint32_t> a, b;
    for (auto p : pairs) {
      a.push_back(p.a);
      b.push_back(p.b);
    }
    auto da = upload(*r, host(*r, a)), db = upload(*r, host(*r, b));
    auto core = TaskCore::create();
    for (unsigned op = 0; op < 3; ++op) {
      auto group = core->openRoot();
      auto t = r->compute(*core, group, kernel, m::BinaryOperation(op), da, db);
      CHECK(core->drain());
      auto device = observe(*core, t);
      CHECK(device.onDevice() && !device.hostBytes());
      CHECK(device.bytes() == da.bytes());
      clean(*core, group);
      check(download(*r, device), pairs, op);
    }
    // Strict computation must preserve both input buffers bit for bit.
    auto ha = download(*r, da), hb = download(*r, db);
    CHECK(std::memcmp(ha.hostBytes()->data(), a.data(), a.size() * 4) == 0);
    CHECK(std::memcmp(hb.hostBytes()->data(), b.data(), b.size() * 4) == 0);
  }
  empty(*r);
  const std::array<std::uint32_t, 3> values{0x80000001, 0x3f800001, 0x7f800000};
  {
    auto a = upload(*r, host(*r, values)), b = upload(*r, host(*r, values));
    // Every compute allocation/encoder checkpoint is transactional.
    r->failAllocationAt(0);
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto t = r->compute(*core, group, kernel, m::BinaryOperation::Add, a, b);
    CHECK(core->drain());
    CHECK(core->failure(t).kind == TaskFailure::None);
    auto attempts = r->stats().attempts;
    clean(*core, group);
    auto baseline = r->stats().bytes;
    for (std::uint64_t fail = 1; fail <= attempts; ++fail) {
      r->failAllocationAt(fail);
      group = core->openRoot();
      auto before = r->stats().submissions;
      t = r->compute(*core, group, kernel, m::BinaryOperation::Add, a, b);
      CHECK(core->drain());
      CHECK(core->failure(t).code == std::int64_t(m::Error::Allocation));
      CHECK(r->stats().submissions == before);
      clean(*core, group);
      CHECK(r->stats().bytes == baseline);
    }
    r->failAllocationAt(0);
    // Validation before allocation or submission, including host/foreign storage.
    for (unsigned bad = 0; bad < 8; ++bad) {
      auto left = a, right = b;
      auto k = kernel;
      auto op = m::BinaryOperation::Add;
      if (bad == 0)
        left = {};
      if (bad == 1)
        k = {};
      if (bad == 2)
        op = m::BinaryOperation(99);
      if (bad == 3)
        right = host(*r, values);
      if (bad == 4)
        right = upload(*r, host(*r, std::span(values).first(2)));
      if (bad == 5)
        right = upload(*r, host(*r, values, m::Format::UInt32));
      if (bad == 6)
        right = upload(**other, host(**other, values));
      if (bad == 7)
        k = *(*other)->loadBinary32Kernel(argv[1], *(*other)->device());
      auto before = r->stats().submissions;
      group = core->openRoot();
      t = r->compute(*core, group, k, op, left, right);
      CHECK(core->drain());
      CHECK(core->failure(t).kind == TaskFailure::Fault);
      CHECK(before == r->stats().submissions);
      clean(*core, group);
    }
    // Failed acknowledgement cannot publish a result.
    r->failNextCompletion();
    group = core->openRoot();
    t = r->compute(*core, group, kernel, m::BinaryOperation::Multiply, a, b);
    CHECK(core->drain());
    CHECK(core->failure(t).code == std::int64_t(m::Error::Device));
    clean(*core, group);
    for (bool deadline : {false, true}) {
      group = core->openRoot();
      auto before = r->stats().submissions;
      r->holdCompletions(true);
      t = r->compute(*core, group, kernel, m::BinaryOperation::Add, a, b);
      CHECK(core->step());
      CHECK(core->stats().externalPending == 1);
      CHECK(r->stats().submissions == before + 1);
      CHECK(core->cancel(group, deadline));
      core->step();
      CHECK(!core->settled(group));
      CHECK(r->stats().bytes > baseline);
      r->holdCompletions(false);
      CHECK(core->drain());
      CHECK(core->failure(group).kind ==
            (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
      CHECK(core->failure(t).kind == TaskFailure::Cancelled);
      Invocation cancelledContext;
      alignas(m::Buffer) std::byte unpublished[sizeof(m::Buffer)];
      CHECK(!core->observe(t, m::Runtime::bufferLayout(), cancelledContext, unpublished));
      clean(*core, group);
      CHECK(r->stats().bytes == baseline);
    }
    group = core->openRoot();
    CHECK(core->cancel(group));
    auto before = r->stats().submissions;
    t = r->compute(*core, group, kernel, m::BinaryOperation::Add, a, b);
    CHECK(core->drain());
    CHECK(core->failure(t).kind != TaskFailure::None);
    CHECK(r->stats().submissions == before);
    clean(*core, group);
    // Cancelling a CPU parent must retain its pending compute child through acknowledgement.
    group = core->openRoot();
    PacketData packet{r.get(), kernel, a, b, m::BinaryOperation::Add};
    r->holdCompletions(true);
    t = core->submit(group, supervisor, {supervisor.packetLayout, &packet, sizeof(packet)});
    until([&] {
      core->step();
      return core->stats().externalPending == 1;
    });
    CHECK(core->cancel(group));
    core->step();
    CHECK(!core->settled(group));
    r->holdCompletions(false);
    CHECK(core->drain());
    CHECK(core->failure(t).kind == TaskFailure::Cancelled);
    clean(*core, group);
    CHECK(r->stats().bytes == baseline);
    // Destruction also waits for compute acknowledgement and releases unpublished output.
    auto pending = TaskCore::create();
    auto pendingGroup = pending->openRoot();
    r->holdCompletions(true);
    r->compute(*pending, pendingGroup, kernel, m::BinaryOperation::Add, a, b);
    CHECK(pending->step());
    CHECK(pending->stats().externalPending == 1);
    std::atomic<bool> destroying{false}, destroyed{false};
    std::thread drop([&] {
      destroying = true;
      pending.reset();
      destroyed = true;
    });
    until([&] { return destroying.load(); });
    CHECK(!destroyed);
    CHECK(r->stats().bytes > baseline);
    r->holdCompletions(false);
    drop.join();
    CHECK(destroyed);
    CHECK(r->stats().bytes == baseline);
    // Concurrent host admissions; nested continuation tests above use a one-slot queue.
    TaskLimits limits;
    limits.workers = 4;
    auto multi = TaskCore::create(limits);
    group = multi->openRoot();
    std::vector<TaskHandle> tasks;
    for (unsigned i = 0; i < 16; ++i)
      tasks.push_back(r->compute(*multi, group, kernel, m::BinaryOperation::Add, a, b));
    CHECK(multi->drain());
    for (auto task : tasks)
      CHECK(observe(*multi, task).onDevice());
    clean(*multi, group);
  }
  empty(*r);
  empty(**other);
  // Parameter storage participates in the byte budget; compute shares transfer command limits.
  for (bool commandBudget : {false, true}) {
    m::Options options;
    if (commandBudget)
      options.maxInFlight = 1;
    else
      options.maxBytes = 12;
    auto local = m::Runtime::create(options);
    CHECK(local);
    auto k = (*local)->loadBinary32Kernel(argv[1], *(*local)->device());
    CHECK(k);
    auto a = upload(**local, host(**local, std::span(values).first(1)));
    auto core = TaskCore::create();
    auto group = core->openRoot();
    if (commandBudget) {
      (*local)->holdCompletions(true);
      (*local)->compute(*core, group, *k, m::BinaryOperation::Add, a, a);
      CHECK(core->step());
      CHECK(core->stats().externalPending == 1);
    }
    auto before = (*local)->stats().submissions;
    auto t = (*local)->compute(*core, group, *k, m::BinaryOperation::Add, a, a);
    CHECK(core->step());
    CHECK(core->failure(t).code == std::int64_t(m::Error::Budget));
    CHECK((*local)->stats().submissions == before);
    (*local)->holdCompletions(false);
    clean(*core, group);
    a = {};
    empty(**local);
  }
  // Submitted work retains inputs, pipeline, and registry after all facade handles are dropped.
  {
    auto local = m::Runtime::create();
    CHECK(local);
    auto k = (*local)->loadBinary32Kernel(argv[1], *(*local)->device());
    CHECK(k);
    auto a = upload(**local, host(**local, values));
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto t = (*local)->compute(*core, group, *k, m::BinaryOperation::Add, a, a);
    a = {};
    k = {};
    local = {};
    CHECK(core->drain());
    CHECK(observe(*core, t).onDevice());
    clean(*core, group);
  }
  std::printf("Managed Metal compute passed: %llu APFloat comparisons; nested tasks, tails, "
              "immutable inputs, cancellation, failures, and cleanup\n",
              (unsigned long long)comparisons);
}
