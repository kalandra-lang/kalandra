  // Experimental copied-runtime policy: only an already admitted closed region.
  static void closeIndependentEncoder(Frame &f) {
    if (f.independentEncoder) [f.independentEncoder endEncoding];
    f.independentEncoder = nil;
    f.independentEnd = SIZE_MAX;
  }
  static void planIndependentGroup(Frame &f) {
    static const bool enabled = [] {const auto *v=std::getenv("KALANDRA_QKV_CONCURRENT");return v && std::string_view(v)=="1";}();
    if (!enabled || f.independentEnd != SIZE_MAX || !f.progression ||
        !f.progression->closedSynchronous || !f.packet.numericProgram) return;
    const auto &p=*f.packet.numericProgram;
    using K=NumericInstructionKind;
    if (p.pc>=p.code.size()) return;
    const auto &first=p.code[p.pc];
    if(first.kind!=K::Matmul || first.inputs.size()!=2) return;
    const auto &a=p.values[first.inputs[0]],&b=p.values[first.inputs[1]];
    if(!a || !b || a.shape().size()!=2 || b.shape().size()!=2 ||
       a.shape()[1]!=b.shape()[0]) return;
    const auto rows=std::uint32_t(a.shape()[0]),inner=std::uint32_t(a.shape()[1]),cols=std::uint32_t(b.shape()[1]);
    if(!useRegisterMatmul(rows,inner,cols) || useSkinnyMatmul(rows,inner,cols) || useGroupedMatmul(rows,inner,cols)) return;
    std::array<std::uint32_t,3> outputs{};
    auto at=p.pc;
    for(unsigned i=0;i<3;++i) {
      if(at>=p.code.size()) return;
      const auto &op=p.code[at];const auto &k=op.kernel.state_;
      if(op.kind!=K::Matmul || op.inputs.size()!=2 || op.inputs[0]!=first.inputs[0] ||
         !k || !k->matrix || k->numericType!=NumericType::Float32 || !k->registerMatrix ||
         !k->tiledMatrix || k->indirect() || k->readCount!=2 || k->index!=p.index ||
         k->registry!=p.state || op.kernel.state_!=first.kernel.state_ ||
         p.values[op.output]) return;
      for(auto id:op.inputs) {
        const auto &v=p.values[id];
        if(!v || !v.onDevice() || v.format()!=Format::Float32 || !v.bytes() ||
           v.storage_->state!=p.state || v.storage_->deviceIndex!=p.index) return;
        for(unsigned j=0;j<i;++j) if(id==outputs[j]) return;
      }
      const auto &right=p.values[op.inputs[1]];
      if(right.shape().size()!=2 || right.shape()[0]!=b.shape()[0] || right.shape()[1]!=b.shape()[1]) return;
      for(unsigned j=0;j<i;++j) if(op.output==outputs[j]) return;
      outputs[i]=op.output;
      if(i==2) { f.independentEnd=at; if(std::getenv("KALANDRA_QKV_TRACE")) std::fprintf(stderr,"QKV_GROUP %zu %zu\n",p.pc,at); return; }
      ++at;
      if(at<p.code.size() && p.code[at].kind==K::MatrixShape) {
        if(at+1>=p.code.size() || p.code[at].inputs!=p.code[at+1].inputs) return;
        ++at;
      }
    }
  }
  static id<MTLComputeCommandEncoder> independentComputeEncoder(Frame &f,id<MTLCommandBuffer> command) {
    if(f.independentEnd==SIZE_MAX) return [command computeCommandEncoder];
    if(!f.independentEncoder)
      f.independentEncoder=[command computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
    return f.independentEncoder;
  }
  static void finishIndependentDispatch(Frame &f,id<MTLComputeCommandEncoder> encoder) {
    if(encoder!=f.independentEncoder) [encoder endEncoding];
    else if(f.packet.numericProgram->pc==f.independentEnd) closeIndependentEncoder(f);
  }
