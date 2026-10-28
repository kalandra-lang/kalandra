  static id<MTLComputeCommandEncoder> traceEncoder(Frame &f, id<MTLCommandBuffer> command) {
    if (!f.packet.numericProgram) return [command computeCommandEncoder];
    auto device=f.packet.state->devices[f.packet.numericProgram->index];
    if (!f.traceBuffer) {
      if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) std::abort();
      MTLCounterSampleBufferDescriptor *desc=[MTLCounterSampleBufferDescriptor new];
      for (id<MTLCounterSet> set in device.counterSets)
        if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) desc.counterSet=set;
      if (!desc.counterSet) std::abort();
      desc.storageMode=MTLStorageModeShared;
      desc.sampleCount=f.packet.numericProgram->code.size()*2;
      NSError *error=nil;
      f.traceBuffer=[device newCounterSampleBufferWithDescriptor:desc error:&error];
      if (!f.traceBuffer) {std::fprintf(stderr,"counter buffer: %s\n",error.description.UTF8String);std::abort();}
      const auto mach0=mach_absolute_time();
      [device sampleTimestamps:&f.traceCpu0 gpuTimestamp:&f.traceGpu0];
      const auto mach1=mach_absolute_time();
      mach_timebase_info_data_t tb;mach_timebase_info(&tb);
      const long double low=static_cast<long double>(mach0)*tb.numer/tb.denom;
      const long double high=static_cast<long double>(mach1)*tb.numer/tb.denom;
      constexpr long double tolerance=10000000;
      if(f.traceCpu0>=low-tolerance && f.traceCpu0<=high+tolerance) f.traceCpuScale=1;
      else if(static_cast<long double>(f.traceCpu0)*tb.numer/tb.denom>=low-tolerance &&
              static_cast<long double>(f.traceCpu0)*tb.numer/tb.denom<=high+tolerance)
        f.traceCpuScale=double(tb.numer)/tb.denom;
      else {std::fprintf(stderr,"unrecognized CPU timestamp units\n");std::abort();}
    }
    auto pass=[MTLComputePassDescriptor computePassDescriptor];
    auto a=pass.sampleBufferAttachments[0];a.sampleBuffer=f.traceBuffer;
    a.startOfEncoderSampleIndex=f.trace.size()*2;a.endOfEncoderSampleIndex=f.trace.size()*2+1;
    auto &p=*f.packet.numericProgram;
    auto k=f.packet.kernel.state_;
    auto left=f.packet.input.shape(),right=f.packet.right.shape();
    f.trace.push_back({p.pc,int(p.code[p.pc].kind),k->transform?int(*k->transform):-1,
                      left.empty()?1:std::uint64_t(left[0]),
                      k->matrix?std::uint64_t(right[1]):(left.size()>1?std::uint64_t(left[1]):1),
                      k->matrix?std::uint64_t(left[1]):0});
    return [command computeCommandEncoderWithDescriptor:pass];
  }
  static void dumpTrace(Frame &f) {
    if (!f.traceBuffer) return;
    MTLTimestamp cpu1=0,gpu1=0;
    [f.packet.state->devices[f.packet.numericProgram->index] sampleTimestamps:&cpu1 gpuTimestamp:&gpu1];
    mach_timebase_info_data_t timebase;mach_timebase_info(&timebase);
    if(cpu1<=f.traceCpu0 || gpu1<=f.traceGpu0) std::abort();
    const double scale=double(cpu1-f.traceCpu0)*f.traceCpuScale/double(gpu1-f.traceGpu0);
    auto data=[f.traceBuffer resolveCounterRange:NSMakeRange(0,f.trace.size()*2)];
    if(!data || data.length!=f.trace.size()*2*sizeof(MTLCounterResultTimestamp))std::abort();
    auto times=static_cast<const MTLCounterResultTimestamp *>(data.bytes);
    std::fprintf(stderr,"DISPATCH_PROFILE {\"cpu0\":%llu,\"cpu1\":%llu,\"gpu0\":%llu,\"gpu1\":%llu,\"timebase_numer\":%u,\"timebase_denom\":%u,\"cpu_unit_ns\":%.17g,\"ns_per_gpu_tick\":%.17g,\"stages\":[",
      (unsigned long long)f.traceCpu0,(unsigned long long)cpu1,(unsigned long long)f.traceGpu0,(unsigned long long)gpu1,timebase.numer,timebase.denom,f.traceCpuScale,scale);
    for(std::size_t i=0;i<f.trace.size();++i){
      auto start=times[i*2].timestamp,end=times[i*2+1].timestamp;auto &t=f.trace[i];
      if(!start || start==MTLCounterErrorValue || end==MTLCounterErrorValue || end<start || start<f.traceGpu0 || end>gpu1)std::abort();
      std::fprintf(stderr,"%s{\"pc\":%zu,\"kind\":%d,\"transform\":%d,\"m\":%llu,\"n\":%llu,\"k\":%llu,\"variant\":%d,\"start\":%llu,\"end\":%llu,\"ns\":%.3f}",i?",":"",t.pc,t.kind,t.transform,(unsigned long long)t.m,(unsigned long long)t.n,(unsigned long long)t.k,t.variant,(unsigned long long)start,(unsigned long long)end,(end-start)*scale);
    }
    std::fprintf(stderr,"]}\n");
  }
