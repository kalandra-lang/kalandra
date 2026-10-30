#include "Metal.h"
#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "Upload assertion %d: %s\n", __LINE__, #x); std::abort(); } } while (false)
constexpr std::size_t quantum = 1024 * 1024;
m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
void clean(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group) && core.drain() && core.release(group));
  const auto s = core.stats();
  CHECK(!s.records && !s.groups && !s.retainedBytes && !s.retainedOwners && !s.externalPending);
}
void empty(m::Runtime &r) {
  const auto s = r.stats();
  CHECK(!s.bytes && !s.hostBackings && !s.deviceBackings && !s.metalBuffers &&
        !s.stagingBuffers && !s.inFlight);
}
m::Buffer host(m::Runtime &r, std::size_t count, unsigned seed = 31) {
  std::vector<std::byte> bytes(count);
  for (std::size_t i = 0; i < count; ++i) bytes[i] = std::byte((i * 83 + seed) & 255);
  const std::int64_t n = count;
  auto h = r.hostTensor(bytes, {&n, 1}, m::Format::UInt8);
  CHECK(h);
  return *h;
}
void compare(m::Runtime &r, TaskCore &core, const m::Buffer &gpu, const m::Buffer &source) {
  auto group = core.openRoot();
  auto task = r.download(core, group, gpu);
  CHECK(task && core.drain());
  auto result = observe(core, task);
  CHECK(result.bytes() == source.bytes());
  CHECK(!result.bytes() || !std::memcmp(result.hostBytes()->data(), source.hostBytes()->data(), result.bytes()));
  clean(core, group);
}
m::Buffer uploaded(m::Runtime &r, TaskCore &core, const m::Buffer &input) {
  auto group = core.openRoot();
  auto task = r.upload(core, group, input, *r.device());
  CHECK(task && core.drain() && core.failure(task).kind == TaskFailure::None);
  auto output = observe(core, task);
  clean(core, group);
  return output;
}
void cacheBudget(m::Runtime &r, std::uint64_t budget, std::uint64_t cap) {
  const auto s = r.stats();
  CHECK(s.retainedBytes == s.bytes + s.cachedUploadBytes);
  CHECK(s.retainedBytes <= budget && s.peakRetainedBytes <= budget);
  CHECK(s.cachedUploadBytes <= cap && s.cachedUploadBuffers <= 64);
}
void uploadCacheTests() {
  auto probe = m::Runtime::create(); CHECK(probe);
  if (!(*probe)->device()->hasUnifiedMemory()) return;
  // A hit transfers its existing reservation. Escaped immutable aliases keep
  // their own backing even when another invocation uploads the same shape.
  {
    m::Options options; options.maxBytes = 4096; options.maxCachedUploadBytes = 128;
    auto made = m::Runtime::create(options); CHECK(made); auto &r = **made;
    auto core = TaskCore::create();
    auto first = host(r, 17, 11), second = host(r, 17, 117);
    auto old = uploaded(r, *core, first), alias = old;
    old = {};
    CHECK(!r.stats().cachedUploadBytes);
    auto changed = uploaded(r, *core, second);
    compare(r, *core, alias, first); compare(r, *core, changed, second);
    changed = {};
    CHECK(r.stats().cachedUploadBytes == 20);
    const auto hits = r.stats().uploadCacheHits;
    changed = uploaded(r, *core, second);
    CHECK(r.stats().uploadCacheHits == hits + 1 && !r.stats().cachedUploadBytes);
    compare(r, *core, alias, first); compare(r, *core, changed, second);
    changed = {}; alias = {};
    CHECK(r.stats().cachedUploadBytes == 40 && r.stats().cachedUploadBuffers == 2);
    cacheBudget(r, 4096, 128);
    const auto live = r.stats().bytes;
    r.trimUploadCache();
    CHECK(r.stats().bytes == live && !r.stats().cachedUploadBytes && !r.stats().cachedUploadBuffers);
  }
  // Idle storage never causes an otherwise satisfiable allocation to fail.
  {
    m::Options options; options.maxBytes = 48; options.maxCachedUploadBytes = 32;
    auto made = m::Runtime::create(options); CHECK(made); auto &r = **made;
    auto core = TaskCore::create(); auto input = host(r, 16);
    auto gpu = uploaded(r, *core, input); gpu = {};
    CHECK(r.stats().bytes == 16 && r.stats().cachedUploadBytes == 16);
    auto large = host(r, 32);
    CHECK(r.stats().bytes == 48 && !r.stats().cachedUploadBytes);
    std::array<std::byte, 1> byte{}; const std::int64_t one = 1;
    auto tooMuch = r.hostTensor(byte, {&one, 1}, m::Format::UInt8);
    CHECK(!tooMuch && tooMuch.error() == m::Error::Budget);
    cacheBudget(r, 48, 32);
    large = {}; gpu = uploaded(r, *core, input); gpu = {}; input = {};
    empty(r); cacheBudget(r, 48, 32);
    r.trimUploadCache(); CHECK(!r.stats().retainedBytes);
  }
  // Both byte capacity and fixed entry capacity are enforced across shapes.
  for (std::uint64_t cap : {24u, 65536u}) {
    m::Options options; options.maxCachedUploadBytes = cap;
    auto made = m::Runtime::create(options); CHECK(made); auto &r = **made;
    auto core = TaskCore::create();
    for (unsigned i = 1; i <= 70; ++i) {
      { auto input = host(r, i * 4); auto gpu = uploaded(r, *core, input); }
      cacheBudget(r, options.maxBytes, cap);
    }
    CHECK(r.stats().cachedUploadBuffers == (cap == 65536 ? 64 : 1));
    r.trimUploadCache(); empty(r); CHECK(!r.stats().retainedBytes);
  }
  // Every logical failure site remains observable on warm hits as well as misses.
  {
    auto made = m::Runtime::create(); CHECK(made); auto &r = **made;
    auto core = TaskCore::create(); auto input = host(r, 257);
    r.failAllocationAt(0);
    { auto gpu = uploaded(r, *core, input); }
    const auto attempts = r.stats().attempts;
    for (std::uint64_t i = 1; i <= attempts; ++i) {
      r.failAllocationAt(0); { auto gpu = uploaded(r, *core, input); }
      CHECK(r.stats().cachedUploadBytes);
      r.failAllocationAt(i);
      auto group = core->openRoot(); auto task = r.upload(*core, group, input, *r.device());
      CHECK(core->drain() && core->failure(task).code == std::int64_t(m::Error::Allocation));
      clean(*core, group);
      CHECK(r.stats().bytes == 257 && !r.stats().inFlight && !r.stats().metalBuffers);
      cacheBudget(r, m::Options{}.maxBytes, m::Options{}.maxCachedUploadBytes);
    }
    r.failAllocationAt(0);
    { auto gpu = uploaded(r, *core, input); CHECK(r.stats().attempts == attempts); }
  }
  // Cancellation can leave a partly overwritten cached buffer. The next upload
  // must replace every byte before publication, including the last copy chunk.
  for (bool deadline : {false, true}) {
    auto made = m::Runtime::create(); CHECK(made); auto &r = **made;
    auto core = TaskCore::create(); auto input = host(r, quantum + 3, 17);
    { auto gpu = uploaded(r, *core, input); }
    input = host(r, quantum + 3, 111);
    const auto hits = r.stats().uploadCacheHits;
    auto group = core->openRoot(); auto task = r.upload(*core, group, input, *r.device());
    CHECK(core->step() && !core->settled(task) && r.stats().uploadCacheHits == hits + 1);
    CHECK(core->cancel(group, deadline) && core->drain());
    CHECK(core->failure(group).kind == (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
    clean(*core, group);
    input = host(r, quantum + 3, 219);
    auto gpu = uploaded(r, *core, input); compare(r, *core, gpu, input);
    cacheBudget(r, m::Options{}.maxBytes, m::Options{}.maxCachedUploadBytes);
  }
  // An admitted GPU read retains the old upload until ACK, including cancellation.
  {
    auto made = m::Runtime::create(); CHECK(made); auto &r = **made;
    auto core = TaskCore::create(), other = TaskCore::create();
    auto input = host(r, 131, 19), next = host(r, 131, 241);
    auto gpu = uploaded(r, *core, input);
    r.holdCompletions(true);
    auto group = core->openRoot(); auto task = r.download(*core, group, gpu);
    CHECK(core->step() && core->stats().externalPending == 1);
    gpu = {};
    CHECK(!r.stats().cachedUploadBytes && core->cancel(group));
    const auto hits = r.stats().uploadCacheHits;
    auto replacement = uploaded(r, *other, next);
    CHECK(r.stats().uploadCacheHits == hits && !r.stats().cachedUploadBytes);
    r.trimUploadCache();
    CHECK(r.stats().inFlight == 1);
    r.holdCompletions(false);
    CHECK(core->drain() && core->failure(task).kind == TaskFailure::Cancelled);
    clean(*core, group); compare(r, *other, replacement, next);
    cacheBudget(r, m::Options{}.maxBytes, m::Options{}.maxCachedUploadBytes);
  }
  // Cache operations, evictions and immutable captures are safe across cores.
  {
    m::Options options; options.maxBytes = 65536; options.maxCachedUploadBytes = 4096;
    auto made = m::Runtime::create(options); CHECK(made); auto &r = **made;
    std::array<std::jthread, 4> workers;
    for (unsigned t = 0; t < workers.size(); ++t) workers[t] = std::jthread([&, t] {
      auto core = TaskCore::create();
      for (unsigned i = 0; i < 16; ++i) {
        auto input = host(r, 257 + (i % 3) * 4, 31 + i + 37 * t);
        auto gpu = uploaded(r, *core, input); compare(r, *core, gpu, input);
        if (i % 5 == 0) r.trimUploadCache();
      }
    });
    for (auto &worker : workers) worker.join();
    empty(r); cacheBudget(r, options.maxBytes, options.maxCachedUploadBytes);
    r.trimUploadCache(); CHECK(!r.stats().retainedBytes);
  }
  // Competing reservations must share one live+idle budget. Keep successful
  // allocations alive at the barrier so exactly two of eight requests fit.
  {
    m::Options options; options.maxBytes = 4096; options.maxCachedUploadBytes = 1024;
    auto made = m::Runtime::create(options); CHECK(made); auto &r = **made;
    auto core = TaskCore::create();
    { auto input = host(r, 1024); auto gpu = uploaded(r, *core, input); }
    CHECK(r.stats().cachedUploadBytes == 1024);
    std::barrier sync(8);
    std::atomic<unsigned> successes{0};
    std::array<std::jthread, 8> workers;
    for (unsigned t = 0; t < workers.size(); ++t) workers[t] = std::jthread([&, t] {
      std::array<std::byte, 2048> data{};
      const std::int64_t count = data.size();
      for (unsigned round = 0; round < 32; ++round) {
        {
          auto value = r.hostTensor(data, {&count, 1}, m::Format::UInt8);
          if (value) ++successes;
          else CHECK(value.error() == m::Error::Budget);
          sync.arrive_and_wait();
          if (!t) {
            CHECK(successes == 2 && r.stats().bytes == 4096);
            cacheBudget(r, options.maxBytes, options.maxCachedUploadBytes);
          }
          sync.arrive_and_wait();
        }
        sync.arrive_and_wait();
        if (!t) successes = 0;
        sync.arrive_and_wait();
      }
    });
    for (auto &worker : workers) worker.join();
    empty(r); CHECK(!r.stats().retainedBytes && !r.stats().cachedUploadBytes);
    CHECK(r.stats().peakRetainedBytes == options.maxBytes);
  }
  for (bool disabled : {false, true}) {
    m::Options options;
    if (disabled) options.maxCachedUploadBytes = 0;
    else options.sharedUploads = false;
    auto made = m::Runtime::create(options); CHECK(made); auto &r = **made;
    auto core = TaskCore::create();
    for (unsigned i = 0; i < 3; ++i) { auto input = host(r, 17); auto gpu = uploaded(r, *core, input); }
    empty(r);
    CHECK(!r.stats().retainedBytes && !r.stats().cachedUploadBytes && !r.stats().uploadCacheHits);
  }
}

int main() {
  uploadCacheTests();
  for (bool shared : {false, true}) {
    m::Options options;
    options.sharedUploads = shared;
    auto made = m::Runtime::create(options);
    CHECK(made);
    auto &r = **made;
    const bool direct = shared && r.device()->hasUnifiedMemory();
    for (unsigned workers : {1u, 2u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      CHECK(core);
      for (std::size_t count : {std::size_t(0), std::size_t(1), std::size_t(3),
                               quantum - 1, quantum, quantum + 3, 3 * quantum + 7}) {
        auto input = host(r, count), alias = input;
        const auto before = r.stats();
        auto group = core->openRoot();
        auto task = r.upload(*core, group, input, *r.device());
        input = {};
        if (direct && count > quantum) {
          CHECK(core->step());
          CHECK(!core->settled(task) && !core->stats().externalPending);
          CHECK(r.stats().inFlight == 1 && r.stats().stagingBuffers == 0);
        }
        CHECK(core->drain());
        auto gpu = observe(*core, task);
        CHECK(gpu.onDevice() && !gpu.hostBytes() && gpu.bytes() == count);
        CHECK(r.stats().submissions - before.submissions == (count && !direct ? 1 : 0));
        clean(*core, group);
        compare(r, *core, gpu, alias);
      }
      empty(r);
      for (bool deadline : {false, true}) {
        auto input = host(r, 3 * quantum + 7);
        auto group = core->openRoot();
        auto task = r.upload(*core, group, input, *r.device());
        if (direct) {
          CHECK(core->step()); // Cancel an actual partially copied snapshot.
          CHECK(!core->settled(task) && !core->stats().externalPending);
        }
        CHECK(core->cancel(group, deadline) && core->drain());
        CHECK(core->failure(task).kind == TaskFailure::Cancelled);
        CHECK(core->failure(group).kind == (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
        clean(*core, group);
      }
      empty(r);
    }
    // Every upload allocation failure releases both backing and admission.
    {
      auto core = TaskCore::create();
      auto input = host(r, quantum + 3);
      auto group = core->openRoot();
      r.failAllocationAt(0);
      auto task = r.upload(*core, group, input, *r.device());
      CHECK(task && core->drain());
      const auto attempts = r.stats().attempts;
      clean(*core, group);
      const auto bytes = r.stats().bytes;
      for (std::uint64_t failure = 1; failure <= attempts; ++failure) {
        group = core->openRoot();
        r.failAllocationAt(failure);
        task = r.upload(*core, group, input, *r.device());
        CHECK(task && core->drain());
        CHECK(core->failure(task).code == std::int64_t(m::Error::Allocation));
        clean(*core, group);
        CHECK(r.stats().bytes == bytes && !r.stats().inFlight && !r.stats().metalBuffers);
      }
      r.failAllocationAt(0);
    }
    empty(r);
    if (!direct) continue;
    // Copy CPU-owned backing, then mutate the original after completion: the
    // upload is a snapshot, never an alias to an invocation allocation.
    {
      Invocation source;
      auto *data = static_cast<std::byte *>(source.allocate(quantum + 3));
      CHECK(data);
      std::memset(data, 0x35, quantum + 3);
      CpuTensor<NumericType::UInt8, 1> tensor{};
      tensor.allocated = tensor.aligned = reinterpret_cast<NumericValue<NumericType::UInt8> *>(data);
      tensor.sizes = {quantum + 3}; tensor.strides = {1};
      auto input = r.fromCpu(source, tensor.view()); CHECK(input);
      auto core = TaskCore::create(); auto group = core->openRoot();
      auto task = r.upload(*core, group, *input, *r.device());
      CHECK(core->step() && !core->settled(task));
      source.settle(); // The packet's lease keeps the next copy chunk alive.
      CHECK(core->drain()); auto gpu = observe(*core, task); clean(*core, group);
      std::memset(data, 0x79, quantum + 3);
      *input = {}; source.settle();
      group = core->openRoot(); task = r.download(*core, group, gpu);
      CHECK(core->drain()); auto result = observe(*core, task); clean(*core, group);
      for (auto byte : *result.hostBytes()) CHECK(byte == std::byte{0x35});
    }
    empty(r);
    {
      m::Options limited; limited.maxInFlight = 1;
      auto local = m::Runtime::create(limited); CHECK(local);
      auto input = host(**local, 3 * quantum + 7);
      auto core = TaskCore::create(); auto group = core->openRoot();
      auto first = (*local)->upload(*core, group, input, *(*local)->device());
      CHECK(core->step() && !core->settled(first));
      auto second = (*local)->upload(*core, group, input, *(*local)->device());
      CHECK(core->step() && core->step());
      CHECK(core->failure(second).code == std::int64_t(m::Error::Budget));
      CHECK(!core->stats().externalPending);
      clean(*core, group); input = {}; empty(**local);
    }
    {
      auto input = host(r, 3); auto core = TaskCore::create(); auto group = core->openRoot();
      const auto before = r.stats();
      auto task = r.upload(*core, group, input, *r.device());
      CHECK(core->cancel(group) && core->drain());
      CHECK(core->failure(task).kind == TaskFailure::Cancelled);
      CHECK(r.stats().attempts == before.attempts && r.stats().submissions == before.submissions);
      clean(*core, group);
    }
    empty(r);
    // A shared upload submits no command and must not consume a pending
    // device-completion failure. The following download still observes it.
    {
      auto input = host(r, 3); auto core = TaskCore::create(); auto group = core->openRoot();
      r.failNextCompletion(); auto task = r.upload(*core, group, input, *r.device());
      CHECK(core->drain()); auto gpu = observe(*core, task); clean(*core, group);
      group = core->openRoot(); task = r.download(*core, group, gpu);
      CHECK(core->drain() && core->failure(task).code == std::int64_t(m::Error::Device));
      clean(*core, group);
    }
    empty(r);
    // Dropping the runtime and core mid-copy needs no external acknowledgement.
    {
      auto local = m::Runtime::create(); CHECK(local);
      auto core = TaskCore::create(); auto group = core->openRoot();
      auto input = host(**local, quantum + 3);
      auto task = (*local)->upload(*core, group, input, *(*local)->device());
      input = {}; CHECK(core->step() && !core->settled(task));
      local->reset(); core.reset();
    }
    // Parallel cores retain independent immutable snapshots in one runtime.
    {
      auto input = host(r, quantum + 3);
      std::array<std::jthread, 4> threads;
      for (auto &thread : threads) thread = std::jthread([&] {
        auto core = TaskCore::create(); auto group = core->openRoot();
        auto task = r.upload(*core, group, input, *r.device()); CHECK(core->drain());
        auto gpu = observe(*core, task); clean(*core, group); compare(r, *core, gpu, input);
      });
      for (auto &thread : threads) thread.join();
    }
    empty(r);
  }
  // Exact budgets include physical padding, but no obsolete staging buffer.
  for (bool shared : {false, true}) for (std::uint64_t budget : {6u, 7u, 9u, 10u}) {
    m::Options options; options.sharedUploads = shared; options.maxBytes = budget;
    auto made = m::Runtime::create(options); CHECK(made); auto &r = **made;
    auto input = host(r, 3); auto core = TaskCore::create(); auto group = core->openRoot();
    auto task = r.upload(*core, group, input, *r.device()); CHECK(core->drain());
    const bool fits = budget >= (shared && r.device()->hasUnifiedMemory() ? 7u : 10u);
    CHECK(fits ? core->failure(task).kind == TaskFailure::None
               : core->failure(task).code == std::int64_t(m::Error::Budget));
    clean(*core, group); input = {}; empty(r);
  }
  std::puts("Owned upload snapshots, bounded copy cancellation, budgets, lifetime and fallback passed");
}
