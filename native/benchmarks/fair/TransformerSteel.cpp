// Full-model acceptance and changing-input qualification share the same program.
// KALANDRA_STEEL_VERIFY enables profiling/readback checks only in verifier builds.
#include "Metal.h"
#include "TransformerBlock.h"
#include <new>
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
static m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  ffn::check(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
static void clean(const TaskStats &s) {
  ffn::check(!s.groups && !s.records && !s.retainedBytes && !s.retainedOwners &&
             !s.externalPending);
}
static void retire(TaskCore &core, GroupHandle group) {
  ffn::check(core.close(group) && core.drain() && core.release(group));
  clean(core.stats());
}
// Extracted from the compiler's actual host.cpp, including all scalar captures.
#include "TransformerProgram.h"
int main(int argc, char **argv) {
  ffn::check(argc == 11);
  const unsigned rows = std::stoul(argv[2]), d = std::stoul(argv[3]), count = std::stoul(argv[4]),
                 rounds = std::stoul(argv[5]), mode = std::stoul(argv[6]),
                 dump = std::stoul(argv[7]), pattern = std::stoul(argv[8]),
                 policy = std::stoul(argv[9]);
  ffn::check(rows && d && rows <= 512 && d <= 2048 && mode <= 2 && policy <= 2);
  m::Options options;
#ifndef KALANDRA_LEGACY_MATRIX_RUNTIME
  options.fixedTreeMatmul = std::getenv("KALANDRA_FIXED_TREE") != nullptr;
#endif
#ifdef KALANDRA_STEEL_VERIFY
  options.profile = true;
  options.cachePreparedOutputs = false;
#else
  options.cachePreparedOutputs = !std::getenv("KALANDRA_NO_OUTPUT_CACHE");
#endif
  if (auto limit = std::getenv("KALANDRA_UPLOAD_CACHE_BYTES"))
    options.maxCachedUploadBytes = std::stoull(limit);
  if (auto limit = std::getenv("KALANDRA_DEVICE_CACHE_BYTES")) options.maxCachedDeviceBytes = std::stoull(limit);
  options.preencodedProgression = std::stoul(argv[10]) != 0;
  options.closedSynchronous = policy != 1;
  auto made = m::Runtime::create(options);
  ffn::check(bool(made));
  auto &runtime = **made;
  auto device = runtime.device();
  ffn::check(bool(device));
  auto core = TaskCore::create();
  auto runner = policy ? ClosedTaskRunner::create() : nullptr;
  ffn::check(bool(core) && (!policy || bool(runner)));
  Program program(runtime, argv[1]);
  const auto preparedPacks = runtime.stats().argumentPacks,
             preparedBytes = runtime.stats().argumentBytes;
  transformer::Inputs data(rows, d, pattern);
  {
    std::array<m::Buffer, 7> host, resident;
    auto upload = [&](m::Buffer buffer) {
      auto group = core->openRoot();
      auto task = runtime.upload(*core, group, buffer, *device);
      ffn::check(task && core->drain());
      auto result = observe(*core, task);
      retire(*core, group);
      return result;
    };
    auto initialize = [&] {
    for (unsigned i = 0; i < 7; ++i) {
      auto value = runtime.hostTensor(std::as_bytes(std::span(data.values[i])), data.shapes[i],
                                      m::Format::Float32);
      ffn::check(bool(value));
      host[i] = *value;
      if (!mode || (mode == 1 && i))
        resident[i] = upload(host[i]);
    }
    };
    initialize();
    const auto baseline = runtime.stats().bytes;
    auto call = [&](bool save) {
      auto input = resident;
      for (unsigned i = 0; i < 7; ++i)
        if (mode == 2 || (mode == 1 && !i))
          input[i] = upload(host[i]);
      GroupHandle group{};
      m::Buffer output;
      if (policy) {
        auto result = program.launch(runtime, *runner, input, transformer::epsilon, data.scale);
        ffn::check(bool(result));
        output = std::move(*result);
      } else {
        group = core->openRoot();
        auto task = program.launch(runtime, *core, group, input, transformer::epsilon, data.scale);
        ffn::check(task && core->drain() && core->failure(task).kind == TaskFailure::None);
        output = observe(*core, task);
      }
      ffn::check(output.shape().size() == 2 && output.shape()[0] == rows &&
                 output.shape()[1] == d && output.bytes() == rows * d * 4ull);
      if (save) {
        const auto readGroup = group ? group : core->openRoot();
        auto task = runtime.download(*core, readGroup, output);
        ffn::check(task && core->drain());
        auto value = observe(*core, task);
        auto bytes = value.hostBytes();
        ffn::check(bool(bytes));
        std::vector<float> cells(rows * d);
        ffn::check(bytes->size() == cells.size() * 4);
        std::memcpy(cells.data(), bytes->data(), bytes->size());
        ffn::dump(cells, ffn::sum(cells));
        if (!group)
          retire(*core, readGroup);
      }
      output = {};
      if (group)
        retire(*core, group);
      else
        clean(runner->stats());
      return double(rows) * d * 4;
    };
    if (dump) {
#ifdef KALANDRA_STEEL_VERIFY
      for(unsigned step=0;step<4;++step) {
        if(step) {resident={};host={};data=transformer::Inputs(rows,d,(pattern+step)%2);initialize();}
        ffn::check(!runtime.stats().inFlight);
        runtime.trimDeviceCache();
        runtime.setPreparedOutputCaching((step % 2 == 0) != (std::getenv("KALANDRA_PAIR_REVERSE") != nullptr));
        // Warm backing with opposite contents, then verify a fresh input.
        resident={};host={};data=transformer::Inputs(rows,d,(pattern+step+1)%2);initialize();
        call(false);
        resident={};host={};data=transformer::Inputs(rows,d,(pattern+step)%2);initialize();
        call(true);
        const auto s=runtime.stats();
        ffn::check(!s.inFlight && s.retainedBytes==s.bytes+(s.cachedUploadBytes + s.cachedPreparedOutputBytes) &&
          s.peakRetainedBytes<=options.maxBytes && (s.cachedUploadBytes + s.cachedPreparedOutputBytes)<=options.maxCachedDeviceBytes && (s.cachedUploadBuffers + s.cachedPreparedOutputBuffers)<=64);
      }
#else
      call(true);
#endif
    }
    else
      for (int round = -2; round < int(rounds); ++round) {
        auto before = runtime.stats();
        double checksum = 0;
        auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < count; ++i)
          checksum += call(false);
        const double ns = ffn::elapsed(start);
        auto after = runtime.stats();
        ffn::check(after.retainedBytes == after.bytes + (after.cachedUploadBytes + after.cachedPreparedOutputBytes) &&
                   after.peakRetainedBytes <= options.maxBytes &&
                   (after.cachedUploadBytes + after.cachedPreparedOutputBytes) <= options.maxCachedDeviceBytes &&
                   (after.cachedUploadBuffers + after.cachedPreparedOutputBuffers) <= 64);
        std::printf("{\"cache\":true,\"round\":%d,\"live_bytes\":%llu,\"cached_bytes\":%llu,\"retained_bytes\":%llu,\"peak_retained_bytes\":%llu,\"hits\":%llu,\"misses\":%llu}\n",round,
          (unsigned long long)after.bytes,(unsigned long long)(after.cachedUploadBytes + after.cachedPreparedOutputBytes),
          (unsigned long long)after.retainedBytes,(unsigned long long)after.peakRetainedBytes,
          (unsigned long long)(after.uploadCacheHits + after.preparedOutputCacheHits),(unsigned long long)(after.uploadCacheMisses + after.preparedOutputCacheMisses));
        ffn::check(after.bytes == baseline && !after.inFlight &&
                   after.argumentPacks == preparedPacks && after.argumentBytes == preparedBytes);
        std::printf(
            "{\"pipeline_setup_ns\":%.0f,\"preparation_ns\":%.0f,\"round\":%d,\"ns\":%.0f,"
            "\"checksum\":%.17g,\"submissions\":%llu,\"dispatches\":%llu,\"gpu_ns\":%llu,"
            "\"encoding_ns\":%llu,\"attempts\":%llu,\"resident_bytes\":%llu,\"peak_bytes\":%llu}\n",
            program.pipelineNanoseconds, program.preparationNanoseconds, round, ns, checksum,
            (unsigned long long)(after.submissions - before.submissions),
            (unsigned long long)(after.dispatches - before.dispatches),
            (unsigned long long)(after.gpuNanoseconds - before.gpuNanoseconds),
            (unsigned long long)(after.encodingNanoseconds - before.encodingNanoseconds),
            (unsigned long long)(after.attempts - before.attempts), (unsigned long long)baseline,
            (unsigned long long)after.peakBytes);
      }
  }
  const auto s = runtime.stats();
  ffn::check(!s.bytes && !s.hostBackings && !s.deviceBackings && !s.metalBuffers &&
             !s.stagingBuffers);
}
