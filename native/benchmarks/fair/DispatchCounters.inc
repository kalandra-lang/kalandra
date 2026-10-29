// Diagnostic-only include, inserted into a COPY of Metal.mm. Never linked into
// the production runtime or acceptance executable. One closed caller per process.
// https://developer.apple.com/documentation/metal/sampling-gpu-data-into-counter-sample-buffers
// https://developer.apple.com/documentation/metal/converting-gpu-timestamps-into-cpu-time
#include <cstdio>
#include <cstdlib>
struct KlCounterPass {
  std::uint64_t pc;
  std::string pipeline;
};
static id<MTLCounterSampleBuffer> klCounterBuffer;
static std::vector<KlCounterPass> klCounterPasses;
static MTLTimestamp klCpuStart, klGpuStart;
static std::uint64_t klCounterCall = 0;
static void klCounterCheck(bool condition) {
  if (!condition) {
    std::fputs("Dispatch counter probe invariant failed\n", stderr);
    std::abort();
  }
}
static id<MTLComputeCommandEncoder> klCounterEncoder(id<MTLCommandBuffer> command,
                                                    std::uint64_t pc) {
  auto device = command.commandQueue.device;
  if (!klCounterBuffer) {
    klCounterCheck([device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]);
    id<MTLCounterSet> timestamps = nil;
    for (id<MTLCounterSet> set in device.counterSets)
      if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
        timestamps = set;
    klCounterCheck(timestamps != nil);
    auto descriptor = [[MTLCounterSampleBufferDescriptor alloc] init];
    descriptor.counterSet = timestamps;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.sampleCount = 256;
    NSError *error = nil;
    klCounterBuffer = [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
    klCounterCheck(klCounterBuffer != nil && error == nil);
  }
  if (klCounterPasses.empty())
    [device sampleTimestamps:&klCpuStart gpuTimestamp:&klGpuStart];
  const auto index = klCounterPasses.size();
  klCounterCheck(index < 128);
  klCounterPasses.push_back({pc, {}});
  auto pass = [[MTLComputePassDescriptor alloc] init];
  pass.dispatchType = MTLDispatchTypeSerial;
  pass.sampleBufferAttachments[0].sampleBuffer = klCounterBuffer;
  pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = index * 2;
  pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = index * 2 + 1;
  return [command computeCommandEncoderWithDescriptor:pass];
}
static void klCounterLabel(id<MTLComputePipelineState> pipeline) {
  klCounterCheck(!klCounterPasses.empty());
  const auto label = pipeline.label.UTF8String;
  klCounterPasses.back().pipeline = label ? label : "unnamed";
}
static void klCounterFinish(id<MTLCommandBuffer> command) {
  klCounterCheck(!klCounterPasses.empty() && command.status == MTLCommandBufferStatusCompleted);
  MTLTimestamp cpuEnd, gpuEnd;
  [command.commandQueue.device sampleTimestamps:&cpuEnd gpuTimestamp:&gpuEnd];
  klCounterCheck(cpuEnd > klCpuStart && gpuEnd > klGpuStart);
  const double nanosecondsPerTick = double(cpuEnd - klCpuStart) / double(gpuEnd - klGpuStart);
  auto data = [klCounterBuffer resolveCounterRange:NSMakeRange(0, klCounterPasses.size() * 2)];
  klCounterCheck(data && data.length == klCounterPasses.size() * 2 * sizeof(MTLCounterResultTimestamp));
  const auto *samples = static_cast<const MTLCounterResultTimestamp *>(data.bytes);
  std::fprintf(stderr,
               "{\"dispatch_counter_call\":%llu,\"nanoseconds_per_tick\":%.12g,"
               "\"command_gpu_ns\":%.3f,\"passes\":[",
               (unsigned long long)klCounterCall++, nanosecondsPerTick,
               (command.GPUEndTime - command.GPUStartTime) * 1e9);
  for (std::size_t i = 0; i < klCounterPasses.size(); ++i) {
    const auto start = samples[i * 2].timestamp, end = samples[i * 2 + 1].timestamp;
    klCounterCheck(start && end && start != MTLCounterErrorValue && end != MTLCounterErrorValue &&
                   end >= start);
    const auto &pass = klCounterPasses[i];
    // Generated pipeline labels contain only identifiers. Refuse ambiguous JSON.
    klCounterCheck(pass.pipeline.find_first_of("\"\\\n\r") == std::string::npos);
    std::fprintf(stderr,
                 "%s{\"pc\":%llu,\"pipeline\":\"%s\",\"start\":%llu,\"end\":%llu,\"ns\":%.3f}",
                 i ? "," : "", (unsigned long long)pass.pc, pass.pipeline.c_str(),
                 (unsigned long long)start, (unsigned long long)end, (end - start) * nanosecondsPerTick);
  }
  std::fputs("]}\n", stderr);
  klCounterPasses.clear();
}
